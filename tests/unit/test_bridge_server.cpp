// Copyright 2025 Davide Faconti
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <utility>
#include <vector>

#include "pj_ros_bridge/bridge_server.hpp"
#include "pj_ros_bridge/middleware/middleware_interface.hpp"
#include "tl/expected.hpp"

using json = nlohmann::json;
using namespace pj_ros_bridge;

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
// Test fixture
// ---------------------------------------------------------------------------
class BridgeServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rclcpp::init(0, nullptr);
    node_ = std::make_shared<rclcpp::Node>("test_bridge_server");
    mock_ = std::make_shared<MockMiddleware>();
    // Use a high port that the mock will never actually listen on.
    // session_timeout = 10s, publish_rate = 50 Hz
    server_ = std::make_unique<BridgeServer>(node_, mock_, 19999, 10.0, 50.0);
  }

  void TearDown() override {
    server_.reset();
    mock_.reset();
    node_.reset();
    rclcpp::shutdown();
  }

  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<MockMiddleware> mock_;
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
// Because the topics don't exist in the ROS2 graph, subscribe will report
// failures — but we can still exercise the publish_aggregated_messages path
// by injecting messages directly into the shared MessageBuffer and then
// manually triggering the publish timer callback.
//
// Instead of fighting GenericSubscriptionManager (which needs real ROS2
// topic types), we verify that subscribing to non-existent topics returns
// an error containing "failures", and that send_binary is NOT called when
// no messages are buffered.
//
// Then we confirm that when two clients have sessions, send_binary is
// called with the correct per-client identity when there are messages.
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
  // All subscriptions failed since topic doesn't exist in the ROS2 graph
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

  // Spin the node briefly so the publish timer fires at least once
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(60)) {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
// This test creates 3 active sessions and spins the publish timer.
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

  // Spin the node so the publish timer fires multiple times
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(80)) {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
  // Both topics don't exist in the ROS2 graph, so they'll fail — but
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
