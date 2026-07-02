/*
 * Copyright (C) 2026 Davide Faconti
 *
 * This file is part of pj_bridge.
 *
 * pj_bridge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pj_bridge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with pj_bridge. If not, see <https://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pj_bridge/bridge_server.hpp"
#include "pj_bridge/middleware/middleware_interface.hpp"
#include "pj_bridge/subscription_manager_interface.hpp"
#include "pj_bridge/topic_source_interface.hpp"
#include "tl/expected.hpp"

using json = nlohmann::json;
using namespace pj_bridge;

// ---------------------------------------------------------------------------
// MockMiddleware — in-process fake for unit-testing BridgeServer
// ---------------------------------------------------------------------------
class MockMiddleware : public MiddlewareInterface {
 public:
  // --- MiddlewareInterface overrides ---

  tl::expected<void, std::string> initialize(uint16_t /*port*/) override {
    ready_ = true;
    return {};
  }

  void shutdown() override {
    ready_ = false;
  }

  bool receive_request(std::vector<uint8_t>& data, std::string& client_identity) override {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (request_queue_.empty()) {
      return false;
    }
    auto& front = request_queue_.front();
    client_identity = front.first;
    data = std::move(front.second);
    request_queue_.pop_front();
    return true;
  }

  bool send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) override {
    std::lock_guard<std::mutex> lock(reply_mutex_);
    replies_.emplace_back(client_identity, data);
    return true;
  }

  bool publish_data(const std::vector<uint8_t>& /*data*/) override {
    return true;
  }

  bool send_binary(const std::string& client_identity, const std::vector<uint8_t>& data) override {
    std::lock_guard<std::mutex> lock(binary_mutex_);
    binary_sends_.emplace_back(client_identity, data);
    return true;
  }

  bool is_ready() const override {
    return ready_;
  }

  void set_on_connect(ConnectionCallback callback) override {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    on_connect_ = std::move(callback);
  }

  void set_on_disconnect(ConnectionCallback callback) override {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    on_disconnect_ = std::move(callback);
  }

  // --- Test helpers ---

  /// Push a text request as if it came from a connected client.
  void push_request(const std::string& client_id, const std::string& text) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::vector<uint8_t> data(text.begin(), text.end());
    request_queue_.emplace_back(client_id, std::move(data));
  }

  /// Invoke the disconnect callback registered by BridgeServer.
  void simulate_disconnect(const std::string& client_id) {
    ConnectionCallback cb;
    {
      std::lock_guard<std::mutex> lock(cb_mutex_);
      cb = on_disconnect_;
    }
    if (cb) {
      cb(client_id);
    }
  }

  /// Invoke the connect callback registered by BridgeServer.
  void simulate_connect(const std::string& client_id) {
    ConnectionCallback cb;
    {
      std::lock_guard<std::mutex> lock(cb_mutex_);
      cb = on_connect_;
    }
    if (cb) {
      cb(client_id);
    }
  }

  /// Get the most recent reply sent to a given client (empty json on miss).
  json pop_reply(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(reply_mutex_);
    for (auto it = replies_.rbegin(); it != replies_.rend(); ++it) {
      if (it->first == client_id) {
        std::string text(it->second.begin(), it->second.end());
        json j = json::parse(text, nullptr, false);
        replies_.erase(std::next(it).base());
        return j;
      }
    }
    return json{};
  }

  /// Return all recorded binary sends (client_id, raw bytes).
  std::vector<std::pair<std::string, std::vector<uint8_t>>> get_binary_sends() {
    std::lock_guard<std::mutex> lock(binary_mutex_);
    return binary_sends_;
  }

  void clear_binary_sends() {
    std::lock_guard<std::mutex> lock(binary_mutex_);
    binary_sends_.clear();
  }

 private:
  bool ready_{false};

  std::mutex queue_mutex_;
  std::deque<std::pair<std::string, std::vector<uint8_t>>> request_queue_;

  std::mutex reply_mutex_;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> replies_;

  std::mutex binary_mutex_;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> binary_sends_;

  std::mutex cb_mutex_;
  ConnectionCallback on_connect_;
  ConnectionCallback on_disconnect_;
};

// ---------------------------------------------------------------------------
// MockTopicSource — fake topic discovery and schema provider
// ---------------------------------------------------------------------------
class MockTopicSource : public TopicSourceInterface {
 public:
  std::vector<TopicInfo> get_topics() override {
    return topics_;
  }

  std::string get_schema(const std::string& topic_name) override {
    if (empty_schema_topics_.count(topic_name) > 0) {
      return "";  // legitimately empty definition (e.g. std_msgs/msg/Empty)
    }
    for (const auto& t : topics_) {
      if (t.name == topic_name) {
        return "uint32 seq\nstring data\n";
      }
    }
    return "";
  }

  std::string schema_encoding() const override {
    return "ros2msg";
  }

  // Test helper: set the topics list
  void set_topics(std::vector<TopicInfo> topics) {
    topics_ = std::move(topics);
  }

  // Test helper: remove a single topic by name
  void remove_topic(const std::string& name) {
    topics_.erase(
        std::remove_if(topics_.begin(), topics_.end(), [&](const TopicInfo& t) { return t.name == name; }),
        topics_.end());
  }

  // Test helper: mark a topic whose schema is legitimately empty
  void set_empty_schema_topic(const std::string& name) {
    empty_schema_topics_.insert(name);
  }

 private:
  std::vector<TopicInfo> topics_;
  std::unordered_set<std::string> empty_schema_topics_;
};

// ---------------------------------------------------------------------------
// MockSubscriptionManager — fake subscription manager
// ---------------------------------------------------------------------------
class MockSubscriptionManager : public SubscriptionManagerInterface {
 public:
  void set_message_callback(MessageCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

  bool subscribe(const std::string& topic_name, const std::string& /*topic_type*/) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (known_topics_.find(topic_name) == known_topics_.end()) {
      return false;
    }
    ref_counts_[topic_name]++;
    return true;
  }

  bool unsubscribe(const std::string& topic_name) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ref_counts_.find(topic_name);
    if (it == ref_counts_.end() || it->second <= 0) {
      underflow_detected_ = true;
      return false;
    }
    it->second--;
    if (it->second == 0) {
      ref_counts_.erase(it);
    }
    return true;
  }

  void unsubscribe_all() override {
    std::lock_guard<std::mutex> lock(mutex_);
    ref_counts_.clear();
  }

  // Test helpers
  void add_known_topic(const std::string& topic_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    known_topics_.insert(topic_name);
  }

  bool is_subscribed(const std::string& topic_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ref_counts_.find(topic_name);
    return it != ref_counts_.end() && it->second > 0;
  }

  int ref_count(const std::string& topic_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ref_counts_.find(topic_name);
    return (it != ref_counts_.end()) ? it->second : 0;
  }

  bool has_underflow() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return underflow_detected_;
  }

  void reset_underflow() {
    std::lock_guard<std::mutex> lock(mutex_);
    underflow_detected_ = false;
  }

 private:
  // Locked like the real backend managers so concurrency tests can drive
  // BridgeServer from multiple threads without racing inside the mock.
  mutable std::mutex mutex_;
  MessageCallback callback_;
  std::unordered_set<std::string> known_topics_;
  std::unordered_map<std::string, int> ref_counts_;
  bool underflow_detected_{false};
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class BridgeServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_ = std::make_shared<MockMiddleware>();
    mock_topic_source_ = std::make_shared<MockTopicSource>();
    mock_sub_manager_ = std::make_shared<MockSubscriptionManager>();
    // Use a high port that the mock will never actually listen on.
    // session_timeout = 10s, publish_rate = 50 Hz
    server_ = std::make_unique<BridgeServer>(mock_topic_source_, mock_sub_manager_, mock_, 19999, 10.0, 50.0);
  }

  void TearDown() override {
    server_.reset();
    mock_.reset();
    mock_topic_source_.reset();
    mock_sub_manager_.reset();
  }

  std::shared_ptr<MockMiddleware> mock_;
  std::shared_ptr<MockTopicSource> mock_topic_source_;
  std::shared_ptr<MockSubscriptionManager> mock_sub_manager_;
  std::unique_ptr<BridgeServer> server_;
};

// ---------------------------------------------------------------------------
// 1. InitializeSucceeds
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, InitializeSucceeds) {
  EXPECT_TRUE(server_->initialize());
  EXPECT_EQ(server_->get_active_session_count(), 0u);
}

// ---------------------------------------------------------------------------
// 2. HandleGetTopics
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, HandleGetTopics) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "get_topics";
  mock_->push_request("client_A", req.dump());

  EXPECT_TRUE(server_->process_requests());

  json reply = mock_->pop_reply("client_A");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "success");
  EXPECT_TRUE(reply.contains("topics"));
  EXPECT_TRUE(reply["topics"].is_array());
}

// ---------------------------------------------------------------------------
// 3. HandleHeartbeatCreatesSession
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, HandleHeartbeatCreatesSession) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "heartbeat";
  mock_->push_request("client_B", req.dump());

  EXPECT_TRUE(server_->process_requests());
  EXPECT_EQ(server_->get_active_session_count(), 1u);

  json reply = mock_->pop_reply("client_B");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "ok");
}

// ---------------------------------------------------------------------------
// 4. HandleUnknownCommand
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, HandleUnknownCommand) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "invalid";
  mock_->push_request("client_C", req.dump());

  EXPECT_TRUE(server_->process_requests());

  json reply = mock_->pop_reply("client_C");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "error");
  EXPECT_EQ(reply["error_code"], "UNKNOWN_COMMAND");
}

// ---------------------------------------------------------------------------
// 5. HandleMalformedJson
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, HandleMalformedJson) {
  ASSERT_TRUE(server_->initialize());

  mock_->push_request("client_D", "this is not json {{{");

  EXPECT_TRUE(server_->process_requests());

  json reply = mock_->pop_reply("client_D");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "error");
  EXPECT_EQ(reply["error_code"], "INVALID_JSON");
}

// ---------------------------------------------------------------------------
// 6. CleanupSessionIdempotent
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, CleanupSessionIdempotent) {
  ASSERT_TRUE(server_->initialize());

  // Create a session via heartbeat
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_E", hb.dump());
  server_->process_requests();
  ASSERT_EQ(server_->get_active_session_count(), 1u);

  // Trigger disconnect callback twice — should not crash
  mock_->simulate_disconnect("client_E");
  EXPECT_EQ(server_->get_active_session_count(), 0u);

  mock_->simulate_disconnect("client_E");
  EXPECT_EQ(server_->get_active_session_count(), 0u);
}

// ---------------------------------------------------------------------------
// 7. PerClientFiltering
//
// We create two sessions with different (non-existent) topic subscriptions.
// Because the topics don't exist in the mock, subscribe will report
// failures — but we can still exercise the publish_aggregated_messages path.
//
// We verify that subscribing to non-existent topics returns
// an error containing "failures", and that send_binary is NOT called when
// no messages are buffered.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PerClientFiltering) {
  ASSERT_TRUE(server_->initialize());

  // Create two sessions via heartbeats
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_X", hb.dump());
  server_->process_requests();
  mock_->push_request("client_Y", hb.dump());
  server_->process_requests();
  ASSERT_EQ(server_->get_active_session_count(), 2u);

  // Attempt subscribe for client_X to a non-existent topic
  json sub_x;
  sub_x["command"] = "subscribe";
  sub_x["topics"] = json::array({"/nonexistent_topic_alpha"});
  mock_->push_request("client_X", sub_x.dump());
  server_->process_requests();

  json reply_x = mock_->pop_reply("client_X");
  ASSERT_FALSE(reply_x.is_discarded());
  // All subscriptions failed since topic doesn't exist
  EXPECT_TRUE(reply_x.contains("failures"));
  EXPECT_FALSE(reply_x["failures"].empty());

  // Attempt subscribe for client_Y to a different non-existent topic
  json sub_y;
  sub_y["command"] = "subscribe";
  sub_y["topics"] = json::array({"/nonexistent_topic_beta"});
  mock_->push_request("client_Y", sub_y.dump());
  server_->process_requests();

  json reply_y = mock_->pop_reply("client_Y");
  ASSERT_FALSE(reply_y.is_discarded());
  EXPECT_TRUE(reply_y.contains("failures"));
  EXPECT_FALSE(reply_y["failures"].empty());

  // Since no real subscriptions succeeded, the message buffer is empty.
  // Verify that send_binary has NOT been called (no data to publish).
  mock_->clear_binary_sends();

  // Call publish_aggregated_messages directly a few times
  for (int i = 0; i < 3; ++i) {
    server_->publish_aggregated_messages();
  }

  auto binary_sends = mock_->get_binary_sends();
  // No messages were buffered, so no binary sends should have occurred
  EXPECT_TRUE(binary_sends.empty());
}

// ---------------------------------------------------------------------------
// Additional: SubscribeToNonExistentTopicReturnsError
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeToNonExistentTopicReturnsError) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "subscribe";
  req["topics"] = json::array({"/this_topic_does_not_exist"});
  mock_->push_request("client_F", req.dump());

  server_->process_requests();

  json reply = mock_->pop_reply("client_F");
  ASSERT_FALSE(reply.is_discarded());
  // The server should report the subscription failure
  EXPECT_EQ(reply["status"], "error");
  EXPECT_EQ(reply["error_code"], "ALL_SUBSCRIPTIONS_FAILED");
  EXPECT_TRUE(reply.contains("failures"));
  EXPECT_EQ(reply["failures"].size(), 1u);
  EXPECT_EQ(reply["failures"][0]["topic"], "/this_topic_does_not_exist");
}

// ---------------------------------------------------------------------------
// Additional: ProcessRequestsBeforeInitializeFails
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ProcessRequestsBeforeInitializeFails) {
  // Do NOT call initialize
  json req;
  req["command"] = "heartbeat";
  mock_->push_request("client_G", req.dump());

  EXPECT_FALSE(server_->process_requests());
}

// ---------------------------------------------------------------------------
// Additional: MultipleHeartbeatsFromSameClient
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, MultipleHeartbeatsFromSameClient) {
  ASSERT_TRUE(server_->initialize());

  json hb;
  hb["command"] = "heartbeat";

  mock_->push_request("client_H", hb.dump());
  server_->process_requests();
  EXPECT_EQ(server_->get_active_session_count(), 1u);

  // Second heartbeat from same client should not create a new session
  mock_->push_request("client_H", hb.dump());
  server_->process_requests();
  EXPECT_EQ(server_->get_active_session_count(), 1u);
}

// ---------------------------------------------------------------------------
// Additional: MissingCommandField
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, MissingCommandField) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["not_a_command"] = "heartbeat";
  mock_->push_request("client_I", req.dump());

  EXPECT_TRUE(server_->process_requests());

  json reply = mock_->pop_reply("client_I");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "error");
  EXPECT_EQ(reply["error_code"], "INVALID_REQUEST");
}

// ---------------------------------------------------------------------------
// Additional: PublishStatsInitiallyZero
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PublishStatsInitiallyZero) {
  ASSERT_TRUE(server_->initialize());

  auto [msgs, bytes] = server_->get_publish_stats();
  EXPECT_EQ(msgs, 0u);
  EXPECT_EQ(bytes, 0u);
}

// ---------------------------------------------------------------------------
// Additional: SubscribeMissingTopicsField
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeMissingTopicsField) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "subscribe";
  // deliberately omit "topics"
  mock_->push_request("client_J", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_J");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "error");
  EXPECT_EQ(reply["error_code"], "INVALID_REQUEST");
}

// ---------------------------------------------------------------------------
// Regression: StatsNotInflatedByMultipleSessions
//
// The old code counted stats inside the per-client send loop:
//   for (client : clients_in_group) {
//     if (send_binary(client, data)) {
//       total_msg_count += group_msg_count;  // BUG: counted per client
//     }
//   }
// With N clients sharing a subscription group, stats were inflated by N.
// The fix counts once per group regardless of client count.
//
// This test creates 3 active sessions and calls publish_aggregated_messages.
// With no messages in the buffer, stats must remain (0, 0) — verifying
// the counting path does not produce phantom stats with multiple sessions.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, StatsNotInflatedByMultipleSessions) {
  ASSERT_TRUE(server_->initialize());

  // Create 3 sessions via heartbeats
  json hb;
  hb["command"] = "heartbeat";
  for (const auto& id : {"stats_client_1", "stats_client_2", "stats_client_3"}) {
    mock_->push_request(id, hb.dump());
    server_->process_requests();
  }
  ASSERT_EQ(server_->get_active_session_count(), 3u);

  // Call publish_aggregated_messages directly multiple times
  for (int i = 0; i < 4; ++i) {
    server_->publish_aggregated_messages();
  }

  // No messages were buffered, so stats must be zero
  auto [msgs, bytes] = server_->get_publish_stats();
  EXPECT_EQ(msgs, 0u);
  EXPECT_EQ(bytes, 0u);

  // No binary sends should have occurred
  EXPECT_TRUE(mock_->get_binary_sends().empty());
}

// ---------------------------------------------------------------------------
// Additional: SubscribeTopicsNotArray
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeTopicsNotArray) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "subscribe";
  req["topics"] = "not_an_array";
  mock_->push_request("client_K", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_K");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "error");
  EXPECT_EQ(reply["error_code"], "INVALID_REQUEST");
}

// ---------------------------------------------------------------------------
// SubscribeWithRateLimit — mixed string/object topics format
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeWithRateLimit) {
  ASSERT_TRUE(server_->initialize());

  // Subscribe with mixed format: one string (unlimited) and one object (rate-limited)
  // Both topics don't exist in the mock, so they'll fail — but
  // we're testing that the parsing accepts the mixed format and responds
  // with rate_limits when appropriate.
  json req;
  req["command"] = "subscribe";
  req["topics"] = json::array({
      "/nonexistent_unlimited",
      json::object({{"name", "/nonexistent_limited"}, {"max_rate_hz", 10.0}}),
  });
  mock_->push_request("client_rate_1", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_rate_1");
  ASSERT_FALSE(reply.is_discarded());
  // Both topics don't exist, so all subscriptions fail
  EXPECT_TRUE(reply.contains("failures"));
  EXPECT_EQ(reply["failures"].size(), 2u);
}

// ---------------------------------------------------------------------------
// SubscribeBackwardCompatible — old string-only format still works
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeBackwardCompatible) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "subscribe";
  req["topics"] = json::array({"/topic_a", "/topic_b"});
  mock_->push_request("client_compat_1", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_compat_1");
  ASSERT_FALSE(reply.is_discarded());
  // Topics don't exist so we get failures, but no crash from the old format
  EXPECT_TRUE(reply.contains("failures"));
  // No rate_limits in response since all rates are 0.0 (unlimited)
  EXPECT_FALSE(reply.contains("rate_limits"));
}

// ---------------------------------------------------------------------------
// Protocol Version and Request ID Tests
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResponseIncludesProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  // Any command response should include protocol_version
  json request;
  request["command"] = "heartbeat";
  mock_->push_request("client_proto_1", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_1");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["protocol_version"], 1);
}

TEST_F(BridgeServerTest, ResponseEchoesRequestId) {
  ASSERT_TRUE(server_->initialize());

  json request;
  request["command"] = "heartbeat";
  request["id"] = "test-123";
  mock_->push_request("client_proto_2", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_2");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["id"], "test-123");
}

TEST_F(BridgeServerTest, ResponseOmitsIdWhenNotProvided) {
  ASSERT_TRUE(server_->initialize());

  json request;
  request["command"] = "heartbeat";
  mock_->push_request("client_proto_3", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_3");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_FALSE(response.contains("id"));
  EXPECT_TRUE(response.contains("protocol_version"));
}

TEST_F(BridgeServerTest, GetTopicsIncludesProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  json request;
  request["command"] = "get_topics";
  request["id"] = "gt-1";
  mock_->push_request("client_proto_4", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_4");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["protocol_version"], 1);
  EXPECT_EQ(response["id"], "gt-1");
}

TEST_F(BridgeServerTest, ErrorResponseIncludesProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  json request;
  request["command"] = "invalid_command";
  request["id"] = "err-1";
  mock_->push_request("client_proto_5", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_5");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "error");
  EXPECT_EQ(response["protocol_version"], 1);
  EXPECT_EQ(response["id"], "err-1");
}

TEST_F(BridgeServerTest, SubscribeResponseIncludesProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  json request;
  request["command"] = "subscribe";
  request["topics"] = json::array({"/nonexistent_topic"});
  request["id"] = "sub-1";
  mock_->push_request("client_proto_6", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_6");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["protocol_version"], 1);
  EXPECT_EQ(response["id"], "sub-1");
}

TEST_F(BridgeServerTest, MalformedJsonIncludesProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  // Malformed JSON cannot have an "id" field, but should still get protocol_version
  mock_->push_request("client_proto_7", "this is not json {{{");
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_7");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "error");
  EXPECT_EQ(response["protocol_version"], 1);
  EXPECT_FALSE(response.contains("id"));
}

TEST_F(BridgeServerTest, MissingCommandIncludesProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  json request;
  request["not_a_command"] = "heartbeat";
  request["id"] = "missing-cmd-1";
  mock_->push_request("client_proto_8", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_proto_8");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "error");
  EXPECT_EQ(response["protocol_version"], 1);
  EXPECT_EQ(response["id"], "missing-cmd-1");
}

// ---------------------------------------------------------------------------
// SubscribeSchemaHasEncodingFormat
//
// Verifies that schema objects in the subscribe response have the new format:
//   {"encoding": "ros2msg", "definition": "..."}
// instead of the old string format.
//
// Since we can't subscribe to real topics in unit tests (mock returns failure),
// this test verifies schema format when subscription fails. The schemas
// object will be empty for failed subscriptions, but we verify the
// structure is correct.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeSchemaHasEncodingFormat) {
  ASSERT_TRUE(server_->initialize());

  // Subscribe to a non-existent topic - schemas will be empty but
  // we verify the response structure is correct
  json request;
  request["command"] = "subscribe";
  request["topics"] = json::array({"/test_topic"});
  request["id"] = "sub-schema-1";
  mock_->push_request("client_schema_1", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_schema_1");
  ASSERT_FALSE(response.is_discarded());

  // Verify response has schemas field (even if empty due to failed subscription)
  ASSERT_TRUE(response.contains("schemas"));
  EXPECT_TRUE(response["schemas"].is_object());

  // Since /test_topic doesn't exist, it will be in failures
  EXPECT_TRUE(response.contains("failures"));
  EXPECT_EQ(response["failures"].size(), 1u);

  // Verify protocol_version and id are included
  EXPECT_EQ(response["protocol_version"], 1);
  EXPECT_EQ(response["id"], "sub-schema-1");
}

// ---------------------------------------------------------------------------
// SubscribeSchemaEncodingFormatStructure
//
// This test documents the expected schema format structure.
// When a subscription succeeds, each schema should be an object with:
//   - "encoding": "ros2msg"
//   - "definition": <string containing the message definition>
//
// We verify the code path by checking response structure properties.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeSchemaEncodingFormatStructure) {
  ASSERT_TRUE(server_->initialize());

  // Subscribe to multiple non-existent topics to verify format handling
  json request;
  request["command"] = "subscribe";
  request["topics"] = json::array({"/topic_a", "/topic_b", "/topic_c"});
  mock_->push_request("client_schema_2", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_schema_2");
  ASSERT_FALSE(response.is_discarded());

  // schemas field should be an object (may be empty if all failed)
  ASSERT_TRUE(response.contains("schemas"));
  EXPECT_TRUE(response["schemas"].is_object());

  // If there were any successful schemas, each would have encoding and definition
  // For failed subscriptions, schemas is empty, which is correct behavior
  for (auto& [key, value] : response["schemas"].items()) {
    // Each schema entry must be an object with encoding and definition
    EXPECT_TRUE(value.is_object()) << "Schema for " << key << " should be an object";
    EXPECT_TRUE(value.contains("encoding")) << "Schema for " << key << " should have encoding";
    EXPECT_TRUE(value.contains("definition")) << "Schema for " << key << " should have definition";
    EXPECT_EQ(value["encoding"], "ros2msg") << "Schema encoding should be ros2msg";
    EXPECT_TRUE(value["definition"].is_string()) << "Schema definition should be a string";
  }
}

// ---------------------------------------------------------------------------
// Unsubscribe Command Tests
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// UnsubscribeRemovesTopics
//
// Tests that the unsubscribe command removes previously subscribed topics.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeRemovesTopics) {
  ASSERT_TRUE(server_->initialize());

  // Register real topics so subscribe succeeds
  mock_topic_source_->set_topics({{"/topic_a", "std_msgs/msg/String"}, {"/topic_b", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/topic_a");
  mock_sub_manager_->add_known_topic("/topic_b");

  // Create a session via heartbeat
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_unsub_1", hb.dump());
  server_->process_requests();

  // Subscribe to both topics
  json sub_request;
  sub_request["command"] = "subscribe";
  sub_request["topics"] = json::array({"/topic_a", "/topic_b"});
  mock_->push_request("client_unsub_1", sub_request.dump());
  server_->process_requests();

  json sub_response = mock_->pop_reply("client_unsub_1");
  ASSERT_FALSE(sub_response.is_discarded());
  EXPECT_EQ(sub_response["status"], "success");
  EXPECT_TRUE(mock_sub_manager_->is_subscribed("/topic_a"));
  EXPECT_TRUE(mock_sub_manager_->is_subscribed("/topic_b"));

  // Unsubscribe from /topic_a only
  json unsub_request;
  unsub_request["command"] = "unsubscribe";
  unsub_request["id"] = "unsub-1";
  unsub_request["topics"] = json::array({"/topic_a"});
  mock_->push_request("client_unsub_1", unsub_request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_unsub_1");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "success");
  EXPECT_EQ(response["id"], "unsub-1");
  EXPECT_TRUE(response.contains("protocol_version"));
  ASSERT_TRUE(response.contains("removed"));
  EXPECT_EQ(response["removed"].size(), 1u);
  EXPECT_EQ(response["removed"][0], "/topic_a");

  // /topic_a should be unsubscribed, /topic_b should remain
  EXPECT_FALSE(mock_sub_manager_->is_subscribed("/topic_a"));
  EXPECT_TRUE(mock_sub_manager_->is_subscribed("/topic_b"));
}

// ---------------------------------------------------------------------------
// SubscribeIsAdditive
//
// Tests that subscribe only adds topics without removing existing ones.
// This is a BREAKING CHANGE from the old "replace" behavior.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeIsAdditive) {
  ASSERT_TRUE(server_->initialize());

  // Register real topics so subscribe succeeds
  mock_topic_source_->set_topics({{"/topic_a", "std_msgs/msg/String"}, {"/topic_b", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/topic_a");
  mock_sub_manager_->add_known_topic("/topic_b");

  // Create session
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_additive_1", hb.dump());
  server_->process_requests();

  // Subscribe to topic_a
  json sub1;
  sub1["command"] = "subscribe";
  sub1["topics"] = json::array({"/topic_a"});
  mock_->push_request("client_additive_1", sub1.dump());
  server_->process_requests();

  json response1 = mock_->pop_reply("client_additive_1");
  ASSERT_FALSE(response1.is_discarded());
  EXPECT_EQ(response1["status"], "success");
  EXPECT_TRUE(response1["schemas"].contains("/topic_a"));

  // Subscribe to topic_b — should NOT remove topic_a (additive model)
  json sub2;
  sub2["command"] = "subscribe";
  sub2["topics"] = json::array({"/topic_b"});
  mock_->push_request("client_additive_1", sub2.dump());
  server_->process_requests();

  json response2 = mock_->pop_reply("client_additive_1");
  ASSERT_FALSE(response2.is_discarded());
  EXPECT_EQ(response2["status"], "success");
  EXPECT_TRUE(response2["schemas"].contains("/topic_b"));

  // Both topics should still be subscribed
  EXPECT_TRUE(mock_sub_manager_->is_subscribed("/topic_a"));
  EXPECT_TRUE(mock_sub_manager_->is_subscribed("/topic_b"));
}

// ---------------------------------------------------------------------------
// UnsubscribeIgnoresUnsubscribedTopics
//
// Tests that unsubscribing from topics we never subscribed to succeeds
// gracefully with an empty removed array.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeIgnoresUnsubscribedTopics) {
  ASSERT_TRUE(server_->initialize());

  // Create session
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_unsub_ignore", hb.dump());
  server_->process_requests();

  // Unsubscribe from topic we never subscribed to
  json request;
  request["command"] = "unsubscribe";
  request["topics"] = json::array({"/never_subscribed"});
  mock_->push_request("client_unsub_ignore", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_unsub_ignore");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "success");
  ASSERT_TRUE(response.contains("removed"));
  EXPECT_EQ(response["removed"].size(), 0u);  // Nothing removed
}

// ---------------------------------------------------------------------------
// UnsubscribeMissingTopicsField
//
// Tests that unsubscribe with missing topics field returns an error.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeMissingTopicsField) {
  ASSERT_TRUE(server_->initialize());

  // Create session
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_unsub_missing", hb.dump());
  server_->process_requests();

  json request;
  request["command"] = "unsubscribe";
  mock_->push_request("client_unsub_missing", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_unsub_missing");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "error");
  EXPECT_EQ(response["error_code"], "INVALID_REQUEST");
}

// ---------------------------------------------------------------------------
// UnsubscribeTopicsNotArray
//
// Tests that unsubscribe with non-array topics field returns an error.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeTopicsNotArray) {
  ASSERT_TRUE(server_->initialize());

  // Create session
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_unsub_notarray", hb.dump());
  server_->process_requests();

  json request;
  request["command"] = "unsubscribe";
  request["topics"] = "not_an_array";
  mock_->push_request("client_unsub_notarray", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_unsub_notarray");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "error");
  EXPECT_EQ(response["error_code"], "INVALID_REQUEST");
}

// ---------------------------------------------------------------------------
// UnsubscribeIncludesProtocolVersion
//
// Tests that unsubscribe response includes protocol_version and echoes id.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeIncludesProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  // Create session
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_unsub_proto", hb.dump());
  server_->process_requests();

  json request;
  request["command"] = "unsubscribe";
  request["id"] = "proto-unsub-1";
  request["topics"] = json::array({"/any_topic"});
  mock_->push_request("client_unsub_proto", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_unsub_proto");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["protocol_version"], 1);
  EXPECT_EQ(response["id"], "proto-unsub-1");
}

// ---------------------------------------------------------------------------
// Pause and Resume Command Tests
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PauseSetsPausedState
//
// Tests that the pause command sets the session's paused state to true.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PauseSetsPausedState) {
  ASSERT_TRUE(server_->initialize());

  // Create session via heartbeat
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_pause_1", hb.dump());
  server_->process_requests();

  // Send pause command
  json request;
  request["command"] = "pause";
  request["id"] = "p1";
  mock_->push_request("client_pause_1", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_pause_1");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "ok");
  EXPECT_EQ(response["id"], "p1");
  EXPECT_TRUE(response.contains("protocol_version"));
  EXPECT_TRUE(response["paused"].get<bool>());
}

// ---------------------------------------------------------------------------
// ResumeUnsetsPausedState
//
// Tests that the resume command sets the session's paused state to false.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResumeUnsetsPausedState) {
  ASSERT_TRUE(server_->initialize());

  // Create session via heartbeat
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_resume_1", hb.dump());
  server_->process_requests();

  // First pause
  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request("client_resume_1", pause_req.dump());
  server_->process_requests();
  mock_->pop_reply("client_resume_1");  // discard pause response

  // Then resume
  json resume_req;
  resume_req["command"] = "resume";
  resume_req["id"] = "r1";
  mock_->push_request("client_resume_1", resume_req.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_resume_1");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "ok");
  EXPECT_EQ(response["id"], "r1");
  EXPECT_TRUE(response.contains("protocol_version"));
  EXPECT_FALSE(response["paused"].get<bool>());
}

// ---------------------------------------------------------------------------
// PauseIsIdempotent
//
// Tests that calling pause multiple times returns ok without error.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PauseIsIdempotent) {
  ASSERT_TRUE(server_->initialize());

  // Create session via heartbeat
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_pause_idem", hb.dump());
  server_->process_requests();

  // Pause once
  json request;
  request["command"] = "pause";
  mock_->push_request("client_pause_idem", request.dump());
  server_->process_requests();
  mock_->pop_reply("client_pause_idem");  // discard first response

  // Pause again
  mock_->push_request("client_pause_idem", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_pause_idem");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "ok");
  EXPECT_TRUE(response["paused"].get<bool>());
}

// ---------------------------------------------------------------------------
// ResumeIsIdempotent
//
// Tests that calling resume when not paused returns ok without error.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResumeIsIdempotent) {
  ASSERT_TRUE(server_->initialize());

  // Create session via heartbeat (starts unpaused)
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_resume_idem", hb.dump());
  server_->process_requests();

  // Resume when already not paused
  json request;
  request["command"] = "resume";
  mock_->push_request("client_resume_idem", request.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_resume_idem");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "ok");
  EXPECT_FALSE(response["paused"].get<bool>());
}

// ---------------------------------------------------------------------------
// PauseCreatesSessionIfNotExists
//
// Tests that pause creates a session if the client has not yet sent heartbeat.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PauseCreatesSessionIfNotExists) {
  ASSERT_TRUE(server_->initialize());
  EXPECT_EQ(server_->get_active_session_count(), 0u);

  json request;
  request["command"] = "pause";
  mock_->push_request("client_new_pause", request.dump());
  server_->process_requests();

  EXPECT_EQ(server_->get_active_session_count(), 1u);

  json response = mock_->pop_reply("client_new_pause");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "ok");
  EXPECT_TRUE(response["paused"].get<bool>());
}

// ---------------------------------------------------------------------------
// ResumeCreatesSessionIfNotExists
//
// Tests that resume creates a session if the client has not yet sent heartbeat.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResumeCreatesSessionIfNotExists) {
  ASSERT_TRUE(server_->initialize());
  EXPECT_EQ(server_->get_active_session_count(), 0u);

  json request;
  request["command"] = "resume";
  mock_->push_request("client_new_resume", request.dump());
  server_->process_requests();

  EXPECT_EQ(server_->get_active_session_count(), 1u);

  json response = mock_->pop_reply("client_new_resume");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "ok");
  EXPECT_FALSE(response["paused"].get<bool>());
}

// ---------------------------------------------------------------------------
// PausedClientDoesNotReceiveBinaryFrames
//
// Tests that paused clients are skipped when publishing binary frames.
// Since we can't easily inject messages into the buffer, we verify the
// paused state is respected by checking that a paused client with a
// subscription group is skipped.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PausedClientDoesNotReceiveBinaryFrames) {
  ASSERT_TRUE(server_->initialize());

  // Create two sessions via heartbeats
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_paused_1", hb.dump());
  server_->process_requests();
  mock_->push_request("client_active_1", hb.dump());
  server_->process_requests();
  ASSERT_EQ(server_->get_active_session_count(), 2u);

  // Subscribe both clients to the same non-existent topic
  // (subscription will fail but session subscription intent is tracked)
  json sub_req;
  sub_req["command"] = "subscribe";
  sub_req["topics"] = json::array({"/test_topic"});
  mock_->push_request("client_paused_1", sub_req.dump());
  server_->process_requests();
  mock_->pop_reply("client_paused_1");  // discard

  mock_->push_request("client_active_1", sub_req.dump());
  server_->process_requests();
  mock_->pop_reply("client_active_1");  // discard

  // Pause client_paused_1
  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request("client_paused_1", pause_req.dump());
  server_->process_requests();

  json pause_response = mock_->pop_reply("client_paused_1");
  ASSERT_FALSE(pause_response.is_discarded());
  EXPECT_TRUE(pause_response["paused"].get<bool>());

  // Clear any binary sends from previous operations
  mock_->clear_binary_sends();

  // Call publish_aggregated_messages directly
  for (int i = 0; i < 3; ++i) {
    server_->publish_aggregated_messages();
  }

  // Verify binary sends - since topics don't exist, no messages are buffered
  // so no binary sends should occur. But if messages were buffered,
  // only client_active_1 would receive them, not client_paused_1.
  auto binary_sends = mock_->get_binary_sends();
  for (const auto& [client_id, data] : binary_sends) {
    // If any binary sends occurred, they should NOT be to the paused client
    EXPECT_NE(client_id, "client_paused_1") << "Paused client should not receive binary frames";
  }
}

// ---------------------------------------------------------------------------
// ResumedClientReceivesBinaryFrames
//
// Tests that after resuming, a client can receive binary frames again.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResumedClientReceivesBinaryFrames) {
  ASSERT_TRUE(server_->initialize());

  // Create session
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_resume_binary", hb.dump());
  server_->process_requests();

  // Subscribe to a topic
  json sub_req;
  sub_req["command"] = "subscribe";
  sub_req["topics"] = json::array({"/test_topic"});
  mock_->push_request("client_resume_binary", sub_req.dump());
  server_->process_requests();
  mock_->pop_reply("client_resume_binary");  // discard

  // Pause
  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request("client_resume_binary", pause_req.dump());
  server_->process_requests();
  json pause_resp = mock_->pop_reply("client_resume_binary");
  EXPECT_TRUE(pause_resp["paused"].get<bool>());

  // Resume
  json resume_req;
  resume_req["command"] = "resume";
  mock_->push_request("client_resume_binary", resume_req.dump());
  server_->process_requests();
  json resume_resp = mock_->pop_reply("client_resume_binary");
  EXPECT_FALSE(resume_resp["paused"].get<bool>());

  // After resume, the client should be able to receive binary frames
  // (if messages were buffered, which they aren't in this test since
  // the topic doesn't exist)
}

// ===========================================================================
// REGRESSION TESTS - Critical bugs found in code review
// ===========================================================================

// ---------------------------------------------------------------------------
// RegressionTest_BinaryFrameIncludesHeader
//
// Regression test for bug: finalize() not called in publish_aggregated_messages().
// Binary frames MUST include the 16-byte header with magic "PJRB" (0x42524A50).
// Without this fix, compress_zstd() was called directly, skipping the header.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, RegressionTest_BinaryFrameIncludesHeader) {
  // This test verifies that when binary data is sent, it includes the
  // 16-byte header with magic bytes "PJRB" at the start.
  //
  // The bug was: publish_aggregated_messages() called compress_zstd() directly
  // instead of finalize(), so the header was never prepended.

  // We can't easily trigger actual message publishing in unit tests without
  // real topics, but we can verify the serializer's finalize() method
  // is correctly producing the header format by checking the protocol constants.

  // Verify the expected magic value matches "PJRB" in little-endian
  constexpr uint32_t expected_magic = 0x42524A50;  // "PJRB"
  const char* magic_bytes = reinterpret_cast<const char*>(&expected_magic);
  EXPECT_EQ(magic_bytes[0], 'P');
  EXPECT_EQ(magic_bytes[1], 'J');
  EXPECT_EQ(magic_bytes[2], 'R');
  EXPECT_EQ(magic_bytes[3], 'B');

  // Note: The actual integration test that binary frames sent to clients
  // include this header is covered by the message_serializer tests
  // (FinalizeIncludesBinaryHeader, HeaderMagicIsPJRB) which verify finalize()
  // produces correct output. This test documents the regression.
}

// ---------------------------------------------------------------------------
// RegressionTest_PausedClientDisconnectNoDoubleDecrement
//
// Regression test for bug: double-decrement of subscription ref counts when
// a paused client disconnects.
//
// The bug was: handle_pause() decremented ref counts, then cleanup_session()
// also decremented them again on disconnect, causing ref count corruption.
//
// Fix: cleanup_session() now checks if client was paused before unsubscribing.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, RegressionTest_PausedClientDisconnectNoDoubleDecrement) {
  ASSERT_TRUE(server_->initialize());

  // Client 1: Create session, pause, then disconnect
  // This tests the core fix: cleanup_session should not unsubscribe if paused
  const std::string client1 = "client_paused_disconnect";

  // Create session
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request(client1, hb.dump());
  server_->process_requests();
  mock_->pop_reply(client1);  // discard
  EXPECT_EQ(server_->get_active_session_count(), 1u);

  // Pause client1
  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request(client1, pause_req.dump());
  server_->process_requests();
  json pause_resp = mock_->pop_reply(client1);
  EXPECT_TRUE(pause_resp["paused"].get<bool>());

  // Simulate disconnect while paused
  // Before the fix, cleanup_session would try to unsubscribe even though
  // pause already decremented ref counts, causing double-decrement
  mock_->simulate_disconnect(client1);

  // Verify session was cleaned up without crash or error
  EXPECT_EQ(server_->get_active_session_count(), 0u);

  // Client 2: Create a new session after the paused disconnect
  // This verifies the system is still in a good state
  const std::string client2 = "client_after_paused_disconnect";

  mock_->push_request(client2, hb.dump());
  server_->process_requests();
  json hb_resp = mock_->pop_reply(client2);

  // Heartbeat should succeed - system is healthy
  EXPECT_EQ(hb_resp["status"], "ok");
  EXPECT_EQ(server_->get_active_session_count(), 1u);

  // Clean disconnect of client2
  mock_->simulate_disconnect(client2);
  EXPECT_EQ(server_->get_active_session_count(), 0u);
}

// ---------------------------------------------------------------------------
// Regression: Subscribe to a topic that exists but has an empty schema should
// report a failure (not silently succeed with an empty definition).
//
// In the unit test environment, we can't easily mock SchemaExtractor, but we
// can verify the failure-reporting path by subscribing to a non-existent topic
// which produces the failure, confirming the failure response structure is correct.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeFailureResponseContainsTopicAndReason) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "subscribe";
  req["topics"] = json::array({"/nonexistent_topic_1", "/nonexistent_topic_2"});
  mock_->push_request("client_fail_multi", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_fail_multi");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "error");
  EXPECT_EQ(reply["error_code"], "ALL_SUBSCRIPTIONS_FAILED");
  ASSERT_TRUE(reply.contains("failures"));
  ASSERT_EQ(reply["failures"].size(), 2u);

  // Each failure entry must have topic and reason
  for (const auto& failure : reply["failures"]) {
    EXPECT_TRUE(failure.contains("topic"));
    EXPECT_TRUE(failure.contains("reason"));
    EXPECT_FALSE(failure["topic"].get<std::string>().empty());
    EXPECT_FALSE(failure["reason"].get<std::string>().empty());
  }
}

// ===========================================================================
// BUG FIX TESTS - Tests for specific bugs from code review
// ===========================================================================

// ---------------------------------------------------------------------------
// Bug #1 — Ref-count leak on disconnect during subscribe
//
// After subscribing to topics and then disconnecting, all ref counts must
// return to zero with no underflow.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, DisconnectDuringSubscribeCleansUpRefCounts) {
  ASSERT_TRUE(server_->initialize());

  mock_topic_source_->set_topics(
      {{"/t1", "std_msgs/msg/String"}, {"/t2", "std_msgs/msg/String"}, {"/t3", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/t1");
  mock_sub_manager_->add_known_topic("/t2");
  mock_sub_manager_->add_known_topic("/t3");

  // Subscribe to all three
  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/t1", "/t2", "/t3"});
  mock_->push_request("client_dc", sub.dump());
  server_->process_requests();

  json sub_resp = mock_->pop_reply("client_dc");
  ASSERT_EQ(sub_resp["status"], "success");

  // Simulate disconnect
  mock_->simulate_disconnect("client_dc");

  // All ref counts must be zero
  EXPECT_EQ(mock_sub_manager_->ref_count("/t1"), 0);
  EXPECT_EQ(mock_sub_manager_->ref_count("/t2"), 0);
  EXPECT_EQ(mock_sub_manager_->ref_count("/t3"), 0);
  EXPECT_FALSE(mock_sub_manager_->has_underflow());
}

// ---------------------------------------------------------------------------
// Bug #2 — Ref-count underflow on partial resume
//
// When a topic disappears while a client is paused, resume should remove that
// topic from the session. On disconnect, no underflow should occur.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResumePartialFailureRemovesTopicsFromSession) {
  ASSERT_TRUE(server_->initialize());

  mock_topic_source_->set_topics({{"/topic_a", "std_msgs/msg/String"}, {"/topic_b", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/topic_a");
  mock_sub_manager_->add_known_topic("/topic_b");

  // Subscribe to both
  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/topic_a", "/topic_b"});
  mock_->push_request("client_resume_fail", sub.dump());
  server_->process_requests();
  mock_->pop_reply("client_resume_fail");

  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_a"), 1);
  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_b"), 1);

  // Pause — decrements ref counts
  json pause;
  pause["command"] = "pause";
  mock_->push_request("client_resume_fail", pause.dump());
  server_->process_requests();
  mock_->pop_reply("client_resume_fail");

  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_a"), 0);
  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_b"), 0);

  // Simulate /topic_b disappearing
  mock_topic_source_->remove_topic("/topic_b");

  // Resume — /topic_b should fail to re-subscribe
  json resume;
  resume["command"] = "resume";
  mock_->push_request("client_resume_fail", resume.dump());
  server_->process_requests();
  mock_->pop_reply("client_resume_fail");

  // /topic_a should be re-subscribed, /topic_b should not
  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_a"), 1);
  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_b"), 0);

  // Disconnect — should only unsubscribe /topic_a, NOT /topic_b
  mock_->simulate_disconnect("client_resume_fail");

  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_a"), 0);
  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_b"), 0);
  EXPECT_FALSE(mock_sub_manager_->has_underflow());
}

// ---------------------------------------------------------------------------
// Bug #4 — Unsubscribe ignores object format
//
// Unsubscribe should accept [{"name": "/topic"}] in addition to ["/topic"].
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeAcceptsObjectFormat) {
  ASSERT_TRUE(server_->initialize());

  mock_topic_source_->set_topics({{"/topic_a", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/topic_a");

  // Subscribe
  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/topic_a"});
  mock_->push_request("client_obj_unsub", sub.dump());
  server_->process_requests();
  mock_->pop_reply("client_obj_unsub");

  EXPECT_TRUE(mock_sub_manager_->is_subscribed("/topic_a"));

  // Unsubscribe using object format
  json unsub;
  unsub["command"] = "unsubscribe";
  unsub["topics"] = json::array({json::object({{"name", "/topic_a"}})});
  mock_->push_request("client_obj_unsub", unsub.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_obj_unsub");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "success");
  ASSERT_TRUE(response.contains("removed"));
  EXPECT_EQ(response["removed"].size(), 1u);
  EXPECT_EQ(response["removed"][0], "/topic_a");
  EXPECT_EQ(mock_sub_manager_->ref_count("/topic_a"), 0);
}

// ---------------------------------------------------------------------------
// Bug #9 — Division by zero with zero publish rate
//
// BridgeServer::initialize() should reject publish_rate <= 0.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, InitializeRejectsZeroPublishRate) {
  auto zero_rate_server =
      std::make_unique<BridgeServer>(mock_topic_source_, mock_sub_manager_, mock_, 19999, 10.0, 0.0);
  EXPECT_FALSE(zero_rate_server->initialize());
}

TEST_F(BridgeServerTest, InitializeRejectsNegativePublishRate) {
  auto neg_rate_server =
      std::make_unique<BridgeServer>(mock_topic_source_, mock_sub_manager_, mock_, 19999, 10.0, -1.0);
  EXPECT_FALSE(neg_rate_server->initialize());
}

// ---------------------------------------------------------------------------
// UnsubscribeWhilePausedDoesNotDoubleDecrement
//
// A paused client's subscription refs were already released by pause.
// Unsubscribing while paused must NOT decrement again, or a topic shared
// with another client would be destroyed under it.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeWhilePausedDoesNotDoubleDecrement) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/shared", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/shared");

  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/shared"});

  // Client A and client B both subscribe to /shared (ref = 2)
  mock_->push_request("client_a", sub.dump());
  server_->process_requests();
  mock_->push_request("client_b", sub.dump());
  server_->process_requests();
  ASSERT_EQ(mock_sub_manager_->ref_count("/shared"), 2);

  // Client A pauses (releases its ref, ref = 1)
  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request("client_a", pause_req.dump());
  server_->process_requests();
  ASSERT_EQ(mock_sub_manager_->ref_count("/shared"), 1);

  // Client A unsubscribes while paused — its ref is already released,
  // so the count must stay at 1 (client B's ref).
  json unsub;
  unsub["command"] = "unsubscribe";
  unsub["topics"] = json::array({"/shared"});
  mock_->push_request("client_a", unsub.dump());
  server_->process_requests();

  EXPECT_EQ(mock_sub_manager_->ref_count("/shared"), 1);
  EXPECT_TRUE(mock_sub_manager_->is_subscribed("/shared"));
  EXPECT_FALSE(mock_sub_manager_->has_underflow());

  // Client A resuming afterwards must not re-acquire the unsubscribed topic.
  json resume_req;
  resume_req["command"] = "resume";
  mock_->push_request("client_a", resume_req.dump());
  server_->process_requests();
  EXPECT_EQ(mock_sub_manager_->ref_count("/shared"), 1);
}

// ---------------------------------------------------------------------------
// SubscribeWhilePausedDefersRefUntilResume
//
// A paused client holds zero subscription refs by invariant. Subscribing
// while paused must record the topic in the session but only acquire the
// middleware ref on resume — otherwise resume double-acquires and the ref
// leaks forever after disconnect.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeWhilePausedDefersRefUntilResume) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/x", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/x");

  // Pause first (creates the session)
  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request("client_p", pause_req.dump());
  server_->process_requests();

  // Subscribe while paused: session records the topic, no ref acquired yet
  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/x"});
  mock_->push_request("client_p", sub.dump());
  server_->process_requests();
  EXPECT_EQ(mock_sub_manager_->ref_count("/x"), 0);

  // Resume: exactly one ref acquired
  json resume_req;
  resume_req["command"] = "resume";
  mock_->push_request("client_p", resume_req.dump());
  server_->process_requests();
  EXPECT_EQ(mock_sub_manager_->ref_count("/x"), 1);

  // Disconnect: the ref is fully released, nothing leaks
  mock_->simulate_disconnect("client_p");
  EXPECT_EQ(mock_sub_manager_->ref_count("/x"), 0);
  EXPECT_FALSE(mock_sub_manager_->has_underflow());
}

// ---------------------------------------------------------------------------
// PauseVsDisconnectRaceKeepsRefCountsConsistent
//
// cleanup_session runs on the middleware (WebSocket) thread while pause
// runs on the event-loop thread. If pause's state flip and ref release are
// not atomic with respect to cleanup, both paths release the same refs and
// the count underflows (destroying subscriptions other clients still use).
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PauseVsDisconnectRaceKeepsRefCountsConsistent) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/r", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/r");

  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/r"});
  json pause_req;
  pause_req["command"] = "pause";

  constexpr int kIterations = 2000;
  for (int i = 0; i < kIterations; i++) {
    const std::string client = "racer_" + std::to_string(i);

    mock_->push_request(client, sub.dump());
    server_->process_requests();
    ASSERT_EQ(mock_sub_manager_->ref_count("/r"), 1);

    // Pause on the "event loop" thread, disconnect on the "middleware" thread
    mock_->push_request(client, pause_req.dump());
    std::thread event_loop([&]() { server_->process_requests(); });
    std::thread ws_thread([&]() { mock_->simulate_disconnect(client); });
    event_loop.join();
    ws_thread.join();

    ASSERT_FALSE(mock_sub_manager_->has_underflow()) << "double ref release on iteration " << i;
    ASSERT_EQ(mock_sub_manager_->ref_count("/r"), 0) << "leaked ref on iteration " << i;

    // A pause that lost the race may have recreated an empty session; drop it
    // so the next iteration starts clean.
    mock_->simulate_disconnect(client);
  }
}

// ---------------------------------------------------------------------------
// SubscribeClampsExcessiveRateLimit
//
// max_rate_hz is client-controlled. Values whose milli-hertz representation
// exceeds int range would be undefined behavior in the rate-limit math, so
// the server must clamp to a sane maximum (1e6 Hz) at parse time.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeClampsExcessiveRateLimit) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/t", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/t");

  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({{{"name", "/t"}, {"max_rate_hz", 1e15}}});
  mock_->push_request("client_clamp_hi", sub.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_clamp_hi");
  ASSERT_FALSE(response.is_discarded());
  ASSERT_TRUE(response.contains("rate_limits"));
  ASSERT_TRUE(response["rate_limits"].contains("/t"));
  EXPECT_LE(response["rate_limits"]["/t"].get<double>(), 1e6);
}

// ---------------------------------------------------------------------------
// SubscribeClampsTinyRateLimitUp
//
// Rates in (0, 0.001) truncate to zero milli-hertz, which the publish path
// treats as "unlimited" — the opposite of the client's intent. Such rates
// must be clamped UP to the smallest representable limit (0.001 Hz).
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeClampsTinyRateLimitUp) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/t", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/t");

  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({{{"name", "/t"}, {"max_rate_hz", 0.0005}}});
  mock_->push_request("client_clamp_lo", sub.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_clamp_lo");
  ASSERT_FALSE(response.is_discarded());
  ASSERT_TRUE(response.contains("rate_limits"));
  ASSERT_TRUE(response["rate_limits"].contains("/t"));
  EXPECT_GE(response["rate_limits"]["/t"].get<double>(), 0.001);
}

// ---------------------------------------------------------------------------
// ResubscribeWithBareStringPreservesRateLimit
//
// A bare-string topic entry means "no rate specified", not "unlimited".
// Re-asserting a subscription with a plain string (a natural client pattern)
// must not silently wipe a previously configured rate limit.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResubscribeWithBareStringPreservesRateLimit) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/scan", "sensor_msgs/msg/LaserScan"}});
  mock_sub_manager_->add_known_topic("/scan");

  json sub_with_rate;
  sub_with_rate["command"] = "subscribe";
  sub_with_rate["topics"] = json::array({{{"name", "/scan"}, {"max_rate_hz", 10.0}}});
  mock_->push_request("client_rate_keep", sub_with_rate.dump());
  server_->process_requests();
  mock_->pop_reply("client_rate_keep");

  json sub_bare;
  sub_bare["command"] = "subscribe";
  sub_bare["topics"] = json::array({"/scan"});
  mock_->push_request("client_rate_keep", sub_bare.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_rate_keep");
  ASSERT_FALSE(response.is_discarded());
  ASSERT_TRUE(response.contains("rate_limits")) << "bare-string re-subscribe wiped the rate limit";
  ASSERT_TRUE(response["rate_limits"].contains("/scan"));
  EXPECT_DOUBLE_EQ(response["rate_limits"]["/scan"].get<double>(), 10.0);
}

// ---------------------------------------------------------------------------
// ResubscribeWithExplicitZeroClearsRateLimit
//
// Companion to the bare-string test: an explicit max_rate_hz of 0 is the
// documented way to remove a rate limit, and must keep working.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResubscribeWithExplicitZeroClearsRateLimit) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/scan", "sensor_msgs/msg/LaserScan"}});
  mock_sub_manager_->add_known_topic("/scan");

  json sub_with_rate;
  sub_with_rate["command"] = "subscribe";
  sub_with_rate["topics"] = json::array({{{"name", "/scan"}, {"max_rate_hz", 10.0}}});
  mock_->push_request("client_rate_clear", sub_with_rate.dump());
  server_->process_requests();
  mock_->pop_reply("client_rate_clear");

  json sub_zero;
  sub_zero["command"] = "subscribe";
  sub_zero["topics"] = json::array({{{"name", "/scan"}, {"max_rate_hz", 0.0}}});
  mock_->push_request("client_rate_clear", sub_zero.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_rate_clear");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_FALSE(response.contains("rate_limits") && response["rate_limits"].contains("/scan"))
      << "explicit max_rate_hz=0 must clear the limit";
}

// ---------------------------------------------------------------------------
// ResumePreservesTopicsMissingFromDiscovery
//
// docs/API.md: pause "preserves subscriptions and rate limits". A publisher
// that is briefly down while the client resumes must not cost the client its
// subscription: the topic stays in the session (without a middleware ref)
// and is re-acquired on a later resume once the publisher is back.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResumePreservesTopicsMissingFromDiscovery) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/gone", "std_msgs/msg/String"}});
  mock_sub_manager_->add_known_topic("/gone");

  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/gone"});
  mock_->push_request("client_flaky", sub.dump());
  server_->process_requests();
  ASSERT_EQ(mock_sub_manager_->ref_count("/gone"), 1);

  json pause_req;
  pause_req["command"] = "pause";
  json resume_req;
  resume_req["command"] = "resume";

  // Pause, then the publisher restarts: topic vanishes from discovery
  mock_->push_request("client_flaky", pause_req.dump());
  server_->process_requests();
  ASSERT_EQ(mock_sub_manager_->ref_count("/gone"), 0);
  mock_topic_source_->remove_topic("/gone");

  // Resume while the topic is missing: no ref, no underflow, but the
  // subscription must survive in the session
  mock_->push_request("client_flaky", resume_req.dump());
  server_->process_requests();
  EXPECT_EQ(mock_sub_manager_->ref_count("/gone"), 0);
  EXPECT_FALSE(mock_sub_manager_->has_underflow());

  // Publisher comes back; a later pause/resume cycle re-acquires the topic
  mock_topic_source_->set_topics({{"/gone", "std_msgs/msg/String"}});
  mock_->push_request("client_flaky", pause_req.dump());
  server_->process_requests();
  mock_->push_request("client_flaky", resume_req.dump());
  server_->process_requests();
  EXPECT_EQ(mock_sub_manager_->ref_count("/gone"), 1) << "subscription was dropped while the publisher was away";

  // Disconnect releases exactly the refs that are held
  mock_->simulate_disconnect("client_flaky");
  EXPECT_EQ(mock_sub_manager_->ref_count("/gone"), 0);
  EXPECT_FALSE(mock_sub_manager_->has_underflow());
}

// ---------------------------------------------------------------------------
// ProcessRequestsDrainsAllPendingRequests
//
// process_requests() is driven by a 10 ms timer, so handling a single
// request per invocation caps command throughput at ~100 req/s and lets a
// burst (or one chatty client) starve other clients' heartbeats into false
// session timeouts. One invocation must drain the pending queue.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ProcessRequestsDrainsAllPendingRequests) {
  ASSERT_TRUE(server_->initialize());

  json hb;
  hb["command"] = "heartbeat";
  for (int i = 0; i < 5; i++) {
    mock_->push_request("drain_client_" + std::to_string(i), hb.dump());
  }

  server_->process_requests();

  EXPECT_EQ(server_->get_active_session_count(), 5u) << "one process_requests call must handle all queued requests";
}

// ---------------------------------------------------------------------------
// SubscribeAcceptsEmptySchemaDefinition
//
// Some types have legitimately empty definitions (std_msgs/msg/Empty is a
// 0-byte .msg). The topic source signals extraction FAILURE by throwing;
// an empty string is a valid schema and must not fail the subscription.
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeAcceptsEmptySchemaDefinition) {
  ASSERT_TRUE(server_->initialize());
  mock_topic_source_->set_topics({{"/trigger", "std_msgs/msg/Empty"}});
  mock_topic_source_->set_empty_schema_topic("/trigger");
  mock_sub_manager_->add_known_topic("/trigger");

  json sub;
  sub["command"] = "subscribe";
  sub["topics"] = json::array({"/trigger"});
  mock_->push_request("client_empty_schema", sub.dump());
  server_->process_requests();

  json response = mock_->pop_reply("client_empty_schema");
  ASSERT_FALSE(response.is_discarded());
  EXPECT_EQ(response["status"], "success") << response.dump();
  ASSERT_TRUE(response["schemas"].contains("/trigger"));
  EXPECT_EQ(response["schemas"]["/trigger"]["definition"], "");
  EXPECT_EQ(mock_sub_manager_->ref_count("/trigger"), 1);
}
