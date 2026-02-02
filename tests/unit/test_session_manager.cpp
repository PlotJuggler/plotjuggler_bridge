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

#include <chrono>
#include <thread>

#include "pj_ros_bridge/session_manager.hpp"

using namespace pj_ros_bridge;

class SessionManagerTest : public ::testing::Test {
 protected:
  SessionManager manager_{10.0};  // 10 second timeout
};

TEST_F(SessionManagerTest, CreateSession) {
  EXPECT_TRUE(manager_.create_session("client1"));
  EXPECT_EQ(manager_.session_count(), 1);
  EXPECT_TRUE(manager_.session_exists("client1"));

  // Creating same session again should fail
  EXPECT_FALSE(manager_.create_session("client1"));
  EXPECT_EQ(manager_.session_count(), 1);

  // Create another session
  EXPECT_TRUE(manager_.create_session("client2"));
  EXPECT_EQ(manager_.session_count(), 2);
  EXPECT_TRUE(manager_.session_exists("client2"));
}

TEST_F(SessionManagerTest, UpdateHeartbeat) {
  EXPECT_TRUE(manager_.create_session("client1"));

  // Get initial heartbeat
  Session session1;
  EXPECT_TRUE(manager_.get_session("client1", session1));
  auto initial_heartbeat = session1.last_heartbeat;

  // Wait a bit and update heartbeat
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(manager_.update_heartbeat("client1"));

  // Get updated session
  Session session2;
  EXPECT_TRUE(manager_.get_session("client1", session2));
  EXPECT_GT(session2.last_heartbeat, initial_heartbeat);

  // Updating non-existent session should fail
  EXPECT_FALSE(manager_.update_heartbeat("client_nonexistent"));
}

TEST_F(SessionManagerTest, GetSession) {
  EXPECT_TRUE(manager_.create_session("client1"));

  Session session;
  EXPECT_TRUE(manager_.get_session("client1", session));
  EXPECT_EQ(session.client_id, "client1");
  EXPECT_TRUE(session.subscribed_topics.empty());

  // Getting non-existent session should fail
  Session session2;
  EXPECT_FALSE(manager_.get_session("client_nonexistent", session2));
}

TEST_F(SessionManagerTest, UpdateSubscriptions) {
  EXPECT_TRUE(manager_.create_session("client1"));

  std::unordered_set<std::string> topics = {"/topic1", "/topic2", "/topic3"};
  EXPECT_TRUE(manager_.update_subscriptions("client1", topics));

  Session session;
  EXPECT_TRUE(manager_.get_session("client1", session));
  EXPECT_EQ(session.subscribed_topics.size(), 3);
  EXPECT_TRUE(session.subscribed_topics.count("/topic1") > 0);
  EXPECT_TRUE(session.subscribed_topics.count("/topic2") > 0);
  EXPECT_TRUE(session.subscribed_topics.count("/topic3") > 0);

  // Update with different topics
  std::unordered_set<std::string> new_topics = {"/topic4"};
  EXPECT_TRUE(manager_.update_subscriptions("client1", new_topics));

  EXPECT_TRUE(manager_.get_session("client1", session));
  EXPECT_EQ(session.subscribed_topics.size(), 1);
  EXPECT_TRUE(session.subscribed_topics.count("/topic4") > 0);

  // Updating non-existent session should fail
  EXPECT_FALSE(manager_.update_subscriptions("client_nonexistent", topics));
}

TEST_F(SessionManagerTest, GetSubscriptions) {
  EXPECT_TRUE(manager_.create_session("client1"));

  // Initially empty
  auto subs = manager_.get_subscriptions("client1");
  EXPECT_TRUE(subs.empty());

  // Add subscriptions
  std::unordered_set<std::string> topics = {"/topic1", "/topic2"};
  EXPECT_TRUE(manager_.update_subscriptions("client1", topics));

  subs = manager_.get_subscriptions("client1");
  EXPECT_EQ(subs.size(), 2);
  EXPECT_TRUE(subs.count("/topic1") > 0);
  EXPECT_TRUE(subs.count("/topic2") > 0);

  // Getting subscriptions for non-existent session should return empty set
  auto empty_subs = manager_.get_subscriptions("client_nonexistent");
  EXPECT_TRUE(empty_subs.empty());
}

TEST_F(SessionManagerTest, RemoveSession) {
  EXPECT_TRUE(manager_.create_session("client1"));
  EXPECT_TRUE(manager_.create_session("client2"));
  EXPECT_EQ(manager_.session_count(), 2);

  EXPECT_TRUE(manager_.remove_session("client1"));
  EXPECT_EQ(manager_.session_count(), 1);
  EXPECT_FALSE(manager_.session_exists("client1"));
  EXPECT_TRUE(manager_.session_exists("client2"));

  // Removing non-existent session should fail
  EXPECT_FALSE(manager_.remove_session("client1"));
  EXPECT_EQ(manager_.session_count(), 1);

  EXPECT_TRUE(manager_.remove_session("client2"));
  EXPECT_EQ(manager_.session_count(), 0);
}

TEST_F(SessionManagerTest, TimeoutDetection) {
  // Use a very short timeout for testing (0.1 seconds)
  SessionManager short_timeout_manager(0.1);

  EXPECT_TRUE(short_timeout_manager.create_session("client1"));
  EXPECT_TRUE(short_timeout_manager.create_session("client2"));

  // Update heartbeat for client2 only
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_TRUE(short_timeout_manager.update_heartbeat("client2"));

  // Wait for client1 to timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  auto timed_out = short_timeout_manager.get_timed_out_sessions();

  // client1 should have timed out, client2 should not
  EXPECT_EQ(timed_out.size(), 1);
  EXPECT_EQ(timed_out[0], "client1");

  // Wait for client2 to timeout as well
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  timed_out = short_timeout_manager.get_timed_out_sessions();
  EXPECT_EQ(timed_out.size(), 2);

  // Check that both clients are in the list
  bool found_client1 = false;
  bool found_client2 = false;
  for (const auto& client_id : timed_out) {
    if (client_id == "client1") {
      found_client1 = true;
    }
    if (client_id == "client2") {
      found_client2 = true;
    }
  }
  EXPECT_TRUE(found_client1);
  EXPECT_TRUE(found_client2);
}

TEST_F(SessionManagerTest, GetActiveSessions) {
  EXPECT_TRUE(manager_.create_session("client1"));
  EXPECT_TRUE(manager_.create_session("client2"));
  EXPECT_TRUE(manager_.create_session("client3"));

  auto active_sessions = manager_.get_active_sessions();
  EXPECT_EQ(active_sessions.size(), 3);

  // Check that all clients are in the list
  bool found_client1 = false;
  bool found_client2 = false;
  bool found_client3 = false;
  for (const auto& client_id : active_sessions) {
    if (client_id == "client1") {
      found_client1 = true;
    }
    if (client_id == "client2") {
      found_client2 = true;
    }
    if (client_id == "client3") {
      found_client3 = true;
    }
  }
  EXPECT_TRUE(found_client1);
  EXPECT_TRUE(found_client2);
  EXPECT_TRUE(found_client3);

  // Remove one session
  EXPECT_TRUE(manager_.remove_session("client2"));
  active_sessions = manager_.get_active_sessions();
  EXPECT_EQ(active_sessions.size(), 2);
}

TEST_F(SessionManagerTest, SessionTimestamps) {
  EXPECT_TRUE(manager_.create_session("client1"));

  Session session;
  EXPECT_TRUE(manager_.get_session("client1", session));

  // created_at and last_heartbeat should be approximately equal at creation
  auto diff = session.last_heartbeat - session.created_at;
  EXPECT_LE(diff, std::chrono::milliseconds(1));

  // Update heartbeat
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(manager_.update_heartbeat("client1"));

  Session updated_session;
  EXPECT_TRUE(manager_.get_session("client1", updated_session));

  // last_heartbeat should be after created_at now
  EXPECT_GT(updated_session.last_heartbeat, updated_session.created_at);
  EXPECT_EQ(updated_session.created_at, session.created_at);  // created_at should not change
}

TEST_F(SessionManagerTest, CleanupIdempotency) {
  // Create a session, remove it, remove again — second remove returns false
  EXPECT_TRUE(manager_.create_session("client1"));
  EXPECT_TRUE(manager_.session_exists("client1"));

  EXPECT_TRUE(manager_.remove_session("client1"));
  EXPECT_FALSE(manager_.session_exists("client1"));

  // Second remove is idempotent (returns false, no crash)
  EXPECT_FALSE(manager_.remove_session("client1"));
  EXPECT_EQ(manager_.session_count(), 0);
}

TEST_F(SessionManagerTest, EmptyManager) {
  EXPECT_EQ(manager_.session_count(), 0);
  EXPECT_FALSE(manager_.session_exists("any_client"));

  auto active = manager_.get_active_sessions();
  EXPECT_TRUE(active.empty());

  auto timed_out = manager_.get_timed_out_sessions();
  EXPECT_TRUE(timed_out.empty());

  Session session;
  EXPECT_FALSE(manager_.get_session("any_client", session));
  EXPECT_FALSE(manager_.remove_session("any_client"));
  EXPECT_FALSE(manager_.update_heartbeat("any_client"));
}
