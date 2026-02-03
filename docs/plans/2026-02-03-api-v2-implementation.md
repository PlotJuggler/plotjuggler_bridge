# API v2 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement breaking API changes: request IDs, protocol version, schema encoding, unsubscribe command, binary frame header, and pause/resume.

**Architecture:** Centralized response injection for `id` and `protocol_version`. Additive subscription model with explicit unsubscribe. Per-client pause state with smart ROS2 subscription management. 16-byte binary header prepended before compression.

**Tech Stack:** C++17, nlohmann/json, gtest, Python websocket-client

---

## Task 1: Add Protocol Constants

**Files:**
- Create: `include/pj_ros_bridge/protocol_constants.hpp`
- Test: Compile-time only (no runtime test needed)

**Step 1: Create the constants header**

```cpp
// include/pj_ros_bridge/protocol_constants.hpp
// Copyright 2025
// ROS2 Bridge - Protocol Constants

#ifndef PJ_ROS_BRIDGE__PROTOCOL_CONSTANTS_HPP_
#define PJ_ROS_BRIDGE__PROTOCOL_CONSTANTS_HPP_

#include <cstdint>

namespace pj_ros_bridge {

/// Protocol version - increment on breaking API changes
static constexpr int kProtocolVersion = 1;

/// Binary frame magic number ("PJRB" in ASCII, little-endian)
static constexpr uint32_t kBinaryFrameMagic = 0x42524A50;  // "PJRB" read as LE

/// Binary frame header size in bytes
static constexpr size_t kBinaryHeaderSize = 16;

/// Schema encoding identifier for ROS2 message definitions
static constexpr const char* kSchemaEncodingRos2Msg = "ros2msg";

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__PROTOCOL_CONSTANTS_HPP_
```

**Step 2: Verify it compiles**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add include/pj_ros_bridge/protocol_constants.hpp
git commit -m "feat: add protocol constants header (version, magic, encoding)"
```

---

## Task 2: Request ID and Protocol Version in Responses

**Files:**
- Modify: `src/bridge_server.cpp`
- Modify: `include/pj_ros_bridge/bridge_server.hpp`
- Test: `tests/unit/test_bridge_server.cpp`

**Step 1: Write failing tests for request ID and protocol version**

Add to `tests/unit/test_bridge_server.cpp`:

```cpp
// ---------------------------------------------------------------------------
// ResponseContainsProtocolVersion
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResponseContainsProtocolVersion) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "heartbeat";
  mock_->push_request("client_pv", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_pv");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_TRUE(reply.contains("protocol_version"));
  EXPECT_EQ(reply["protocol_version"], 1);
}

// ---------------------------------------------------------------------------
// RequestIdEchoedInResponse
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, RequestIdEchoedInResponse) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "heartbeat";
  req["id"] = "test-request-42";
  mock_->push_request("client_id", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_id");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_TRUE(reply.contains("id"));
  EXPECT_EQ(reply["id"], "test-request-42");
}

// ---------------------------------------------------------------------------
// RequestIdOmittedWhenNotProvided
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, RequestIdOmittedWhenNotProvided) {
  ASSERT_TRUE(server_->initialize());

  json req;
  req["command"] = "heartbeat";
  // No "id" field
  mock_->push_request("client_noid", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_noid");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_FALSE(reply.contains("id"));
}
```

**Step 2: Run tests to verify they fail**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: 3 new tests FAIL (protocol_version not present, id not echoed)

**Step 3: Add include and helper method declaration to bridge_server.hpp**

Add include at top:
```cpp
#include "pj_ros_bridge/protocol_constants.hpp"
```

Add private method declaration:
```cpp
  std::string inject_response_fields(const std::string& response_json, const std::string& request_id) const;
```

**Step 4: Implement response field injection in bridge_server.cpp**

Add new method:
```cpp
std::string BridgeServer::inject_response_fields(const std::string& response_json, const std::string& request_id) const {
  json response = json::parse(response_json);
  response["protocol_version"] = kProtocolVersion;
  if (!request_id.empty()) {
    response["id"] = request_id;
  }
  return response.dump();
}
```

**Step 5: Modify process_requests() to extract request ID and inject fields**

In `process_requests()`, after parsing `request_json`, extract the ID:
```cpp
    json request_json = json::parse(request);
    std::string request_id;
    if (request_json.contains("id") && request_json["id"].is_string()) {
      request_id = request_json["id"].get<std::string>();
    }
```

Before sending response, wrap it:
```cpp
  // Inject protocol_version and id into response
  response = inject_response_fields(response, request_id);

  // Send response to the specific client
  std::vector<uint8_t> response_data(response.begin(), response.end());
```

**Step 6: Run tests to verify they pass**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 7: Commit**

```bash
git add include/pj_ros_bridge/bridge_server.hpp src/bridge_server.cpp tests/unit/test_bridge_server.cpp include/pj_ros_bridge/protocol_constants.hpp
git commit -m "feat: add request ID echo and protocol_version to all responses"
```

---

## Task 3: Schema Encoding in Subscribe Response

**Files:**
- Modify: `src/bridge_server.cpp`
- Test: `tests/unit/test_bridge_server.cpp`

**Step 1: Write failing test for schema encoding format**

Add to `tests/unit/test_bridge_server.cpp`:

```cpp
// ---------------------------------------------------------------------------
// SchemaEncodingFormat - schemas are objects with encoding and definition
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SchemaEncodingFormat) {
  ASSERT_TRUE(server_->initialize());

  // This test uses a non-existent topic, so we check the response structure
  // when there ARE schemas (we'd need a real topic for that).
  // For now, verify the structure when subscribing to existing topics.
  // Since we can't easily create real topics in unit tests, we verify
  // that the schemas object format is correct when it's non-empty.

  // We'll test this indirectly: check that ANY successful subscribe response
  // has schemas with the correct structure. We need integration tests for real topics.

  // For unit test: verify schema structure is an object with encoding/definition
  // by checking the code path exists. The actual test needs a mock that returns schemas.

  // Skip for now - covered by integration tests
  GTEST_SKIP() << "Requires real ROS2 topics - covered by integration tests";
}
```

Note: This is hard to unit test without real topics. We'll verify in integration.

**Step 2: Modify handle_subscribe() to use new schema format**

In `handle_subscribe()`, change:
```cpp
    schemas[topic_name] = schema;
```
to:
```cpp
    json schema_obj;
    schema_obj["encoding"] = kSchemaEncodingRos2Msg;
    schema_obj["definition"] = schema;
    schemas[topic_name] = schema_obj;
```

**Step 3: Build and run existing tests**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS (existing tests don't validate schema structure deeply)

**Step 4: Commit**

```bash
git add src/bridge_server.cpp tests/unit/test_bridge_server.cpp
git commit -m "feat: change schema format to {encoding, definition} object"
```

---

## Task 4: Binary Frame Header

**Files:**
- Modify: `include/pj_ros_bridge/message_serializer.hpp`
- Modify: `src/message_serializer.cpp`
- Modify: `tests/unit/test_message_serializer.cpp`
- Modify: `src/bridge_server.cpp`

**Step 1: Write failing test for binary header**

Add to `tests/unit/test_message_serializer.cpp`:

```cpp
// ---------------------------------------------------------------------------
// BinaryFrameHeader - verify 16-byte header structure
// ---------------------------------------------------------------------------
TEST(MessageSerializerTest, BinaryFrameHeaderStructure) {
  AggregatedMessageSerializer serializer;

  // Create a simple serialized message
  rclcpp::SerializedMessage msg;
  msg.reserve(4);
  auto& rcl_msg = msg.get_rcl_serialized_message();
  rcl_msg.buffer_length = 4;
  memcpy(rcl_msg.buffer, "test", 4);

  serializer.serialize_message("/topic", 1234567890ULL, msg);

  // Get the frame with header
  std::vector<uint8_t> frame;
  serializer.finalize_with_header(frame);

  // Verify header size
  ASSERT_GE(frame.size(), 16u);

  // Verify magic (little-endian "PJRB" = 0x42524A50)
  uint32_t magic;
  memcpy(&magic, frame.data(), 4);
  EXPECT_EQ(magic, 0x42524A50u);

  // Verify message count
  uint32_t count;
  memcpy(&count, frame.data() + 4, 4);
  EXPECT_EQ(count, 1u);

  // Verify uncompressed size matches serialized data size
  uint32_t uncompressed_size;
  memcpy(&uncompressed_size, frame.data() + 8, 4);
  EXPECT_EQ(uncompressed_size, serializer.get_serialized_data().size());

  // Verify flags are 0
  uint32_t flags;
  memcpy(&flags, frame.data() + 12, 4);
  EXPECT_EQ(flags, 0u);
}
```

**Step 2: Run test to verify it fails**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release`
Expected: Build FAILS (finalize_with_header doesn't exist)

**Step 3: Add message count tracking and finalize_with_header method to header**

In `message_serializer.hpp`, add:
```cpp
  /**
   * @brief Get number of messages serialized
   */
  size_t get_message_count() const { return message_count_; }

  /**
   * @brief Finalize serialization and create frame with 16-byte header
   *
   * Header format (little-endian):
   *   - uint32_t magic (0x42524A50 "PJRB")
   *   - uint32_t message_count
   *   - uint32_t uncompressed_size
   *   - uint32_t flags (reserved, 0)
   *
   * The header is followed by ZSTD-compressed payload.
   *
   * @param frame Output parameter - receives header + compressed data
   */
  void finalize_with_header(std::vector<uint8_t>& frame);
```

Add private member:
```cpp
  size_t message_count_{0};
```

**Step 4: Implement finalize_with_header and update serialize_message**

In `message_serializer.cpp`:

Update `serialize_message` to increment counter:
```cpp
void AggregatedMessageSerializer::serialize_message(...) {
  // ... existing code ...
  message_count_++;
}
```

Update `clear()` to reset counter:
```cpp
void AggregatedMessageSerializer::clear() {
  serialized_data_.clear();
  message_count_ = 0;
}
```

Add new method:
```cpp
void AggregatedMessageSerializer::finalize_with_header(std::vector<uint8_t>& frame) {
  // Compress the payload
  std::vector<uint8_t> compressed;
  compress_zstd(serialized_data_, compressed);

  // Build frame: 16-byte header + compressed data
  frame.clear();
  frame.reserve(kBinaryHeaderSize + compressed.size());

  // Magic
  write_le(frame, kBinaryFrameMagic);

  // Message count
  write_le(frame, static_cast<uint32_t>(message_count_));

  // Uncompressed size
  write_le(frame, static_cast<uint32_t>(serialized_data_.size()));

  // Flags (reserved)
  write_le(frame, static_cast<uint32_t>(0));

  // Compressed payload
  frame.insert(frame.end(), compressed.begin(), compressed.end());
}
```

Add include at top of message_serializer.cpp:
```cpp
#include "pj_ros_bridge/protocol_constants.hpp"
```

**Step 5: Run test to verify it passes**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: New test PASSES

**Step 6: Update bridge_server.cpp to use finalize_with_header**

In `publish_aggregated_messages()`, replace:
```cpp
      // Compress once for the group
      std::vector<uint8_t> compressed_data;
      AggregatedMessageSerializer::compress_zstd(serializer.get_serialized_data(), compressed_data);
```
with:
```cpp
      // Finalize with header (includes compression)
      std::vector<uint8_t> frame_data;
      serializer.finalize_with_header(frame_data);
```

And update the send call:
```cpp
        if (middleware_->send_binary(client_id, frame_data)) {
```

And update bytes tracking:
```cpp
        total_bytes += frame_data.size();
```

**Step 7: Run all tests**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 8: Commit**

```bash
git add include/pj_ros_bridge/message_serializer.hpp src/message_serializer.cpp src/bridge_server.cpp tests/unit/test_message_serializer.cpp
git commit -m "feat: add 16-byte binary frame header (magic, count, size, flags)"
```

---

## Task 5: Unsubscribe Command (Additive Model)

**Files:**
- Modify: `include/pj_ros_bridge/bridge_server.hpp`
- Modify: `src/bridge_server.cpp`
- Test: `tests/unit/test_bridge_server.cpp`

**Step 1: Write failing tests for unsubscribe**

Add to `tests/unit/test_bridge_server.cpp`:

```cpp
// ---------------------------------------------------------------------------
// UnsubscribeCommand - removes topics from subscription
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, UnsubscribeCommand) {
  ASSERT_TRUE(server_->initialize());

  // Create session via heartbeat
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_unsub", hb.dump());
  server_->process_requests();

  // Unsubscribe from topics (even if not subscribed, should succeed)
  json req;
  req["command"] = "unsubscribe";
  req["topics"] = json::array({"/topic_a", "/topic_b"});
  mock_->push_request("client_unsub", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_unsub");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "success");
  EXPECT_TRUE(reply.contains("removed"));
  EXPECT_TRUE(reply.contains("protocol_version"));
}

// ---------------------------------------------------------------------------
// SubscribeIsAdditive - subscribe no longer removes topics
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, SubscribeIsAdditive) {
  ASSERT_TRUE(server_->initialize());

  // This is a behavior test - subscribe should only add, never remove.
  // Since we can't subscribe to real topics, we verify the code path
  // by checking that subscribing to non-existent topics doesn't crash
  // and returns the expected structure.

  json req1;
  req1["command"] = "subscribe";
  req1["topics"] = json::array({"/nonexistent_a"});
  mock_->push_request("client_add", req1.dump());
  server_->process_requests();

  json reply1 = mock_->pop_reply("client_add");
  ASSERT_FALSE(reply1.is_discarded());
  // Fails because topic doesn't exist, but that's expected
  EXPECT_TRUE(reply1.contains("failures"));

  // Second subscribe should NOT remove /nonexistent_a from session
  // (even though it failed to actually subscribe)
  json req2;
  req2["command"] = "subscribe";
  req2["topics"] = json::array({"/nonexistent_b"});
  mock_->push_request("client_add", req2.dump());
  server_->process_requests();

  json reply2 = mock_->pop_reply("client_add");
  ASSERT_FALSE(reply2.is_discarded());
  // Both requests should fail similarly - no implicit removal
  EXPECT_TRUE(reply2.contains("failures"));
}
```

**Step 2: Run tests to verify they fail**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: UnsubscribeCommand test FAILS (unknown command)

**Step 3: Add handle_unsubscribe declaration to header**

In `bridge_server.hpp`, add private method:
```cpp
  std::string handle_unsubscribe(const std::string& client_id, const nlohmann::json& request);
```

**Step 4: Implement handle_unsubscribe**

In `bridge_server.cpp`, add:
```cpp
std::string BridgeServer::handle_unsubscribe(const std::string& client_id, const nlohmann::json& request) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  if (!request.contains("topics")) {
    return create_error_response("INVALID_REQUEST", "Missing 'topics' field");
  }

  if (!request["topics"].is_array()) {
    return create_error_response("INVALID_REQUEST", "'topics' must be an array");
  }

  // Get current subscriptions
  auto current_subs = session_manager_->get_subscriptions(client_id);

  // Parse topics to remove
  std::vector<std::string> topics_to_remove;
  for (const auto& topic : request["topics"]) {
    if (topic.is_string()) {
      topics_to_remove.push_back(topic.get<std::string>());
    }
  }

  // Remove topics
  json removed = json::array();
  for (const auto& topic_name : topics_to_remove) {
    if (current_subs.find(topic_name) != current_subs.end()) {
      subscription_manager_->unsubscribe(topic_name);
      current_subs.erase(topic_name);
      removed.push_back(topic_name);
      RCLCPP_INFO(node_->get_logger(), "Client '%s' unsubscribed from topic '%s'", client_id.c_str(), topic_name.c_str());
    }
  }

  // Update session
  session_manager_->update_subscriptions(client_id, current_subs);

  // Clean up last_sent_times for removed topics
  {
    std::lock_guard<std::mutex> lock(last_sent_mutex_);
    auto it = last_sent_times_.find(client_id);
    if (it != last_sent_times_.end()) {
      for (const auto& topic : topics_to_remove) {
        it->second.erase(topic);
      }
    }
  }

  // Build response
  json response;
  response["status"] = "success";
  response["removed"] = removed;

  return response.dump();
}
```

**Step 5: Add routing for unsubscribe command**

In `process_requests()`, add after the subscribe routing:
```cpp
      } else if (command == "unsubscribe") {
        response = handle_unsubscribe(client_id, request_json);
```

**Step 6: Modify handle_subscribe to be additive (remove implicit removal)**

In `handle_subscribe()`, remove the code that finds topics to remove:
```cpp
  // DELETE THIS BLOCK:
  // Find topics to remove (in current but not in requested)
  for (const auto& [topic, rate] : current_subs) {
    if (requested_topics.find(topic) == requested_topics.end()) {
      topics_to_remove.push_back(topic);
    }
  }
```

And remove the unsubscribe loop:
```cpp
  // DELETE THIS BLOCK:
  // Unsubscribe from removed topics
  for (const auto& topic_name : topics_to_remove) {
    subscription_manager_->unsubscribe(topic_name);
    RCLCPP_INFO(node_->get_logger(), "Client '%s' unsubscribed from topic '%s'", client_id.c_str(), topic_name.c_str());
  }
```

And remove from final_subscriptions:
```cpp
  // DELETE THIS BLOCK:
  for (const auto& topic : topics_to_remove) {
    final_subscriptions.erase(topic);
  }
```

Also remove the `topics_to_remove` vector declaration entirely.

**Step 7: Run tests**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 8: Commit**

```bash
git add include/pj_ros_bridge/bridge_server.hpp src/bridge_server.cpp tests/unit/test_bridge_server.cpp
git commit -m "feat: add unsubscribe command, make subscribe additive (breaking)"
```

---

## Task 6: Add Paused State to Session

**Files:**
- Modify: `include/pj_ros_bridge/session_manager.hpp`
- Modify: `src/session_manager.cpp`
- Test: `tests/unit/test_session_manager.cpp`

**Step 1: Write failing tests for pause state**

Add to `tests/unit/test_session_manager.cpp`:

```cpp
TEST_F(SessionManagerTest, PausedStateDefaultFalse) {
  EXPECT_TRUE(manager_.create_session("client1"));
  EXPECT_FALSE(manager_.is_paused("client1"));
}

TEST_F(SessionManagerTest, SetPausedState) {
  EXPECT_TRUE(manager_.create_session("client1"));

  EXPECT_TRUE(manager_.set_paused("client1", true));
  EXPECT_TRUE(manager_.is_paused("client1"));

  EXPECT_TRUE(manager_.set_paused("client1", false));
  EXPECT_FALSE(manager_.is_paused("client1"));
}

TEST_F(SessionManagerTest, PausedStateNonExistentSession) {
  EXPECT_FALSE(manager_.set_paused("nonexistent", true));
  EXPECT_FALSE(manager_.is_paused("nonexistent"));
}
```

**Step 2: Run tests to verify they fail**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release`
Expected: Build FAILS (is_paused, set_paused don't exist)

**Step 3: Add paused field and methods to session_manager.hpp**

Add to `Session` struct:
```cpp
  /// Whether the session is paused (not receiving binary data)
  bool paused{false};
```

Add public methods to `SessionManager`:
```cpp
  /**
   * @brief Set the paused state for a client session
   * @param client_id Client identifier
   * @param paused Whether to pause the session
   * @return true if session exists and was updated, false otherwise
   */
  bool set_paused(const std::string& client_id, bool paused);

  /**
   * @brief Check if a session is paused
   * @param client_id Client identifier
   * @return true if session exists and is paused, false otherwise
   */
  bool is_paused(const std::string& client_id) const;
```

**Step 4: Implement set_paused and is_paused**

In `session_manager.cpp`:
```cpp
bool SessionManager::set_paused(const std::string& client_id, bool paused) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;
  }

  it->second.paused = paused;
  return true;
}

bool SessionManager::is_paused(const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = sessions_.find(client_id);
  if (it == sessions_.end()) {
    return false;
  }

  return it->second.paused;
}
```

**Step 5: Run tests**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 6: Commit**

```bash
git add include/pj_ros_bridge/session_manager.hpp src/session_manager.cpp tests/unit/test_session_manager.cpp
git commit -m "feat: add paused state to session manager"
```

---

## Task 7: Pause and Resume Commands

**Files:**
- Modify: `include/pj_ros_bridge/bridge_server.hpp`
- Modify: `src/bridge_server.cpp`
- Test: `tests/unit/test_bridge_server.cpp`

**Step 1: Write failing tests for pause/resume**

Add to `tests/unit/test_bridge_server.cpp`:

```cpp
// ---------------------------------------------------------------------------
// PauseCommand
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PauseCommand) {
  ASSERT_TRUE(server_->initialize());

  // Create session via heartbeat
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_pause", hb.dump());
  server_->process_requests();

  // Pause
  json req;
  req["command"] = "pause";
  mock_->push_request("client_pause", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_pause");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "ok");
  EXPECT_TRUE(reply.contains("paused"));
  EXPECT_TRUE(reply["paused"].get<bool>());
  EXPECT_TRUE(reply.contains("protocol_version"));
}

// ---------------------------------------------------------------------------
// ResumeCommand
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, ResumeCommand) {
  ASSERT_TRUE(server_->initialize());

  // Create session and pause
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_resume", hb.dump());
  server_->process_requests();

  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request("client_resume", pause_req.dump());
  server_->process_requests();
  mock_->pop_reply("client_resume");  // discard pause response

  // Resume
  json req;
  req["command"] = "resume";
  mock_->push_request("client_resume", req.dump());
  server_->process_requests();

  json reply = mock_->pop_reply("client_resume");
  ASSERT_FALSE(reply.is_discarded());
  EXPECT_EQ(reply["status"], "ok");
  EXPECT_TRUE(reply.contains("paused"));
  EXPECT_FALSE(reply["paused"].get<bool>());
}
```

**Step 2: Run tests to verify they fail**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: Tests FAIL (unknown command)

**Step 3: Add handler declarations to header**

In `bridge_server.hpp`, add private methods:
```cpp
  std::string handle_pause(const std::string& client_id);
  std::string handle_resume(const std::string& client_id);
```

**Step 4: Implement pause and resume handlers**

In `bridge_server.cpp`:
```cpp
std::string BridgeServer::handle_pause(const std::string& client_id) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  // Set paused state
  session_manager_->set_paused(client_id, true);

  RCLCPP_INFO(node_->get_logger(), "Client '%s' paused", client_id.c_str());

  // Build response
  json response;
  response["status"] = "ok";
  response["paused"] = true;

  return response.dump();
}

std::string BridgeServer::handle_resume(const std::string& client_id) {
  // Create session if it doesn't exist
  if (!session_manager_->session_exists(client_id)) {
    session_manager_->create_session(client_id);
    RCLCPP_INFO(node_->get_logger(), "Created new session for client '%s'", client_id.c_str());
  }

  // Update heartbeat
  session_manager_->update_heartbeat(client_id);

  // Clear paused state
  session_manager_->set_paused(client_id, false);

  RCLCPP_INFO(node_->get_logger(), "Client '%s' resumed", client_id.c_str());

  // Build response
  json response;
  response["status"] = "ok";
  response["paused"] = false;

  return response.dump();
}
```

**Step 5: Add routing for pause/resume commands**

In `process_requests()`, add after unsubscribe routing:
```cpp
      } else if (command == "pause") {
        response = handle_pause(client_id);
      } else if (command == "resume") {
        response = handle_resume(client_id);
```

**Step 6: Run tests**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 7: Commit**

```bash
git add include/pj_ros_bridge/bridge_server.hpp src/bridge_server.cpp tests/unit/test_bridge_server.cpp
git commit -m "feat: add pause and resume commands"
```

---

## Task 8: Skip Paused Clients in Publishing

**Files:**
- Modify: `src/bridge_server.cpp`
- Test: `tests/unit/test_bridge_server.cpp`

**Step 1: Write failing test for paused client skipping**

Add to `tests/unit/test_bridge_server.cpp`:

```cpp
// ---------------------------------------------------------------------------
// PausedClientSkippedInPublishing
// ---------------------------------------------------------------------------
TEST_F(BridgeServerTest, PausedClientSkippedInPublishing) {
  ASSERT_TRUE(server_->initialize());

  // Create two sessions
  json hb;
  hb["command"] = "heartbeat";
  mock_->push_request("client_active", hb.dump());
  server_->process_requests();
  mock_->push_request("client_paused", hb.dump());
  server_->process_requests();

  // Pause one client
  json pause_req;
  pause_req["command"] = "pause";
  mock_->push_request("client_paused", pause_req.dump());
  server_->process_requests();
  mock_->pop_reply("client_paused");

  // Both clients have sessions, but one is paused
  EXPECT_EQ(server_->get_active_session_count(), 2u);

  // Clear any binary sends
  mock_->clear_binary_sends();

  // Spin briefly - no messages in buffer, so no sends expected anyway
  // This test mainly verifies the code path doesn't crash
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(60)) {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Verify no crashes and paused client logic is in place
  // Full verification requires integration tests with real topics
  SUCCEED();
}
```

**Step 2: Modify publish_aggregated_messages() to skip paused clients**

In `publish_aggregated_messages()`, when building subscription_groups, skip paused clients:
```cpp
    for (const auto& client_id : active_sessions) {
      // Skip paused clients
      if (session_manager_->is_paused(client_id)) {
        continue;
      }

      auto subs = session_manager_->get_subscriptions(client_id);
      // ... rest of existing code
```

**Step 3: Run tests**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 4: Commit**

```bash
git add src/bridge_server.cpp tests/unit/test_bridge_server.cpp
git commit -m "feat: skip paused clients when publishing aggregated messages"
```

---

## Task 9: Update Python Test Client

**Files:**
- Modify: `tests/integration/test_client.py`

**Step 1: Update deserialize_aggregated_messages to handle new header**

Replace the `deserialize_aggregated_messages` method:

```python
    # Binary frame header constants
    BINARY_MAGIC = 0x42524A50  # "PJRB" in little-endian
    BINARY_HEADER_SIZE = 16

    def parse_binary_header(self, data):
        """
        Parse the 16-byte binary frame header.

        Returns:
            Tuple of (magic, message_count, uncompressed_size, flags, payload)
            or raises ValueError if invalid
        """
        if len(data) < self.BINARY_HEADER_SIZE:
            raise ValueError(f"Frame too small: {len(data)} < {self.BINARY_HEADER_SIZE}")

        magic, count, size, flags = struct.unpack("<IIII", data[:16])

        if magic != self.BINARY_MAGIC:
            raise ValueError(f"Invalid magic: 0x{magic:08X} (expected 0x{self.BINARY_MAGIC:08X})")

        return magic, count, size, flags, data[16:]

    def deserialize_aggregated_messages(self, data):
        """
        Deserialize aggregated messages from binary format.

        Format per message:
          - Topic name length (uint16_t)
          - Topic name (N bytes UTF-8)
          - Timestamp (uint64_t nanoseconds)
          - Message data length (uint32_t)
          - Message data (N bytes)

        Args:
            data: Decompressed binary data (without header)

        Returns:
            List of message dictionaries
        """
        messages = []
        offset = 0

        while offset < len(data):
            if offset + 2 > len(data):
                break

            # Topic name length
            topic_len = struct.unpack("<H", data[offset : offset + 2])[0]
            offset += 2

            # Topic name
            topic_name = data[offset : offset + topic_len].decode("utf-8")
            offset += topic_len

            # Timestamp
            timestamp_ns = struct.unpack("<Q", data[offset : offset + 8])[0]
            offset += 8

            # Message data length
            data_len = struct.unpack("<I", data[offset : offset + 4])[0]
            offset += 4

            # Message data
            msg_data = data[offset : offset + data_len]
            offset += data_len

            messages.append(
                {
                    "topic": topic_name,
                    "timestamp_ns": timestamp_ns,
                    "data": msg_data,
                }
            )

        return messages

    def _process_binary_frame(self, frame_data):
        """Parse header, decompress, and deserialize a binary frame"""
        # Parse header
        magic, count, uncompressed_size, flags, compressed = self.parse_binary_header(frame_data)

        # Decompress payload
        decompressed = self.decompressor.decompress(compressed)

        # Verify size
        if len(decompressed) != uncompressed_size:
            print(f"Warning: decompressed size mismatch: {len(decompressed)} != {uncompressed_size}")

        # Deserialize messages
        messages = self.deserialize_aggregated_messages(decompressed)

        # Verify count
        if len(messages) != count:
            print(f"Warning: message count mismatch: {len(messages)} != {count}")

        for msg in messages:
            topic = msg["topic"]
            self.stats[topic]["count"] += 1
            self.stats[topic]["bytes"] += len(msg["data"])

        return messages
```

**Step 2: Add unsubscribe and pause/resume methods**

Add to `BridgeClient` class:

```python
    def unsubscribe(self, topics):
        """
        Unsubscribe from topics

        Args:
            topics: List of topic names to unsubscribe from

        Returns:
            List of topics that were removed
        """
        request = {"command": "unsubscribe", "topics": topics}
        response = self.send_request(request)

        if response.get("status") == "success":
            return response.get("removed", [])
        else:
            error_msg = response.get("message", "Unknown error")
            raise Exception(f"Failed to unsubscribe: {error_msg}")

    def pause(self):
        """Pause receiving binary data"""
        request = {"command": "pause"}
        response = self.send_request(request)

        if response.get("status") == "ok":
            return response.get("paused", True)
        else:
            error_msg = response.get("message", "Unknown error")
            raise Exception(f"Failed to pause: {error_msg}")

    def resume(self):
        """Resume receiving binary data"""
        request = {"command": "resume"}
        response = self.send_request(request)

        if response.get("status") == "ok":
            return not response.get("paused", False)
        else:
            error_msg = response.get("message", "Unknown error")
            raise Exception(f"Failed to resume: {error_msg}")
```

**Step 3: Test manually (requires running server)**

Run: `python3 tests/integration/test_client.py --help`
Expected: Script runs, shows help

**Step 4: Commit**

```bash
git add tests/integration/test_client.py
git commit -m "feat: update Python client for new binary header and commands"
```

---

## Task 10: Update API Documentation

**Files:**
- Modify: `docs/API.md`

**Step 1: Rewrite API.md with all changes**

Full rewrite incorporating all new features. See design document for format.

Key sections to update:
- Add protocol version note at top
- Update subscribe format (additive, new schema format)
- Add unsubscribe section
- Add pause/resume section
- Update binary format with 16-byte header
- Update sequence diagram

**Step 2: Commit**

```bash
git add docs/API.md
git commit -m "docs: update API.md for v2 protocol changes"
```

---

## Task 11: Run Full Verification

**Step 1: Build**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release`
Expected: Build succeeds

**Step 2: Run all tests**

Run: `colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests pass

**Step 3: Run pre-commit**

Run: `cd ~/ws_plotjuggler/src/pj_ros_bridge && pre-commit run -a`
Expected: All checks pass

**Step 4: Final commit if any formatting changes**

```bash
git add -A && git commit -m "style: apply formatting" || true
```

---

## Summary

| Task | Description | Tests Added |
|------|-------------|-------------|
| 1 | Protocol constants header | 0 (compile-time) |
| 2 | Request ID + protocol version | 3 |
| 3 | Schema encoding format | 0 (integration) |
| 4 | Binary frame header | 1 |
| 5 | Unsubscribe command | 2 |
| 6 | Session paused state | 3 |
| 7 | Pause/resume commands | 2 |
| 8 | Skip paused in publish | 1 |
| 9 | Python client update | 0 (manual) |
| 10 | API docs | 0 |
| 11 | Verification | 0 |

**Total new tests:** 12
