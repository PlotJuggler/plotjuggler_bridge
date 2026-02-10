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
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "pj_ros_bridge/middleware/websocket_middleware.hpp"

using namespace pj_ros_bridge;

class WebSocketMiddlewareTest : public ::testing::Test {
 protected:
  void SetUp() override {
    middleware_ = std::make_unique<WebSocketMiddleware>();
  }

  void TearDown() override {
    middleware_.reset();
  }

  std::unique_ptr<WebSocketMiddleware> middleware_;
};

TEST_F(WebSocketMiddlewareTest, InitializationSuccess) {
  EXPECT_FALSE(middleware_->is_ready());
  auto result = middleware_->initialize(18080);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(middleware_->is_ready());
}

TEST_F(WebSocketMiddlewareTest, InitializationTwiceFails) {
  auto r1 = middleware_->initialize(18081);
  EXPECT_TRUE(r1.has_value());
  auto r2 = middleware_->initialize(18081);
  EXPECT_FALSE(r2.has_value());
}

TEST_F(WebSocketMiddlewareTest, ShutdownWithoutInit) {
  middleware_->shutdown();
  EXPECT_FALSE(middleware_->is_ready());
}

TEST_F(WebSocketMiddlewareTest, ShutdownAfterInit) {
  auto result = middleware_->initialize(18082);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(middleware_->is_ready());
  middleware_->shutdown();
  EXPECT_FALSE(middleware_->is_ready());
}

TEST_F(WebSocketMiddlewareTest, ReceiveRequestWithoutInit) {
  std::vector<uint8_t> data;
  std::string id;
  EXPECT_FALSE(middleware_->receive_request(data, id));
}

TEST_F(WebSocketMiddlewareTest, PublishDataWithoutInit) {
  std::vector<uint8_t> data = {1, 2, 3};
  EXPECT_FALSE(middleware_->publish_data(data));
}

TEST_F(WebSocketMiddlewareTest, PublishDataNoClients) {
  auto result = middleware_->initialize(18083);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data = {1, 2, 3};
  // No clients connected, should return false
  EXPECT_FALSE(middleware_->publish_data(data));
}

TEST_F(WebSocketMiddlewareTest, ReceiveRequestTimeout) {
  auto result = middleware_->initialize(18084);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data;
  std::string client_id;

  // Should timeout since no client is sending data
  EXPECT_FALSE(middleware_->receive_request(data, client_id));
}

TEST_F(WebSocketMiddlewareTest, SendReplyToUnknownClient) {
  auto result = middleware_->initialize(18085);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data = {1, 2, 3};
  EXPECT_FALSE(middleware_->send_reply("nonexistent_client", data));
}

TEST_F(WebSocketMiddlewareTest, SendBinaryToUnknownClient) {
  auto result = middleware_->initialize(18086);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data = {1, 2, 3};
  EXPECT_FALSE(middleware_->send_binary("nonexistent_client", data));
}

TEST_F(WebSocketMiddlewareTest, ReceiveRequestNotInitializedReturnsFast) {
  // Without initializing, receive_request should return false quickly
  std::vector<uint8_t> data;
  std::string client_id;

  auto start = std::chrono::steady_clock::now();
  bool result = middleware_->receive_request(data, client_id);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_FALSE(result);
  // Should return much faster than the 100ms timeout
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
}

// ---------------------------------------------------------------------------
// Real WebSocket client connection tests
// ---------------------------------------------------------------------------

// Helper: wait for the ix::WebSocket client to reach Open state (up to timeout_ms).
static bool wait_for_client_open(ix::WebSocket& client, int timeout_ms = 2000) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.getReadyState() == ix::ReadyState::Open) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// Helper: poll receive_request() in a loop until it succeeds or we time out.
static bool poll_receive_request(
    WebSocketMiddleware& mw, std::vector<uint8_t>& data, std::string& client_id, int timeout_ms = 2000) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (mw.receive_request(data, client_id)) {
      return true;
    }
  }
  return false;
}

TEST_F(WebSocketMiddlewareTest, ClientConnectAndSendMessage) {
  auto result = middleware_->initialize(18090);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(middleware_->is_ready());

  // Create an IXWebSocket client and connect to the server
  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18090");
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr& /*msg*/) {
    // No-op: required to avoid bad_function_call
  });
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";

  // Allow the server time to register the client
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send a text message from the client
  const std::string test_message = "hello from client";
  client.send(test_message);

  // Poll receive_request() until the message arrives
  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(*middleware_, data, client_id))
      << "receive_request() never returned the client message";

  // Verify the data matches what was sent
  std::string received(data.begin(), data.end());
  EXPECT_EQ(received, test_message);

  // Verify client_id is non-empty
  EXPECT_FALSE(client_id.empty());

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WebSocketMiddlewareTest, SendReplyToConnectedClient) {
  auto result = middleware_->initialize(18091);
  ASSERT_TRUE(result.has_value());

  // Accumulate received messages on the client side
  std::string received_reply;
  std::mutex reply_mutex;
  std::condition_variable reply_cv;
  bool reply_received = false;

  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18091");
  client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Message && !msg->binary) {
      std::lock_guard<std::mutex> lock(reply_mutex);
      received_reply = msg->str;
      reply_received = true;
      reply_cv.notify_one();
    }
  });
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send a text message so the server registers the client in its queue
  client.send("ping");

  // Get the client_id from receive_request()
  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(*middleware_, data, client_id));
  ASSERT_FALSE(client_id.empty());

  // Send a reply from the server to this client
  const std::string reply_text = "pong from server";
  std::vector<uint8_t> reply_data(reply_text.begin(), reply_text.end());
  EXPECT_TRUE(middleware_->send_reply(client_id, reply_data));

  // Wait for the client to receive the reply
  {
    std::unique_lock<std::mutex> lock(reply_mutex);
    ASSERT_TRUE(reply_cv.wait_for(lock, std::chrono::seconds(2), [&] { return reply_received; }))
        << "Client never received the reply";
  }

  EXPECT_EQ(received_reply, reply_text);

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WebSocketMiddlewareTest, DisconnectCallbackFires) {
  auto result = middleware_->initialize(18092);
  ASSERT_TRUE(result.has_value());

  // Set up a disconnect callback that records the disconnected client_id
  std::atomic<bool> disconnect_fired{false};
  std::string disconnected_id;
  std::mutex dc_mutex;
  std::condition_variable dc_cv;

  middleware_->set_on_disconnect([&](const std::string& cid) {
    std::lock_guard<std::mutex> lock(dc_mutex);
    disconnected_id = cid;
    disconnect_fired.store(true);
    dc_cv.notify_one();
  });

  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18092");
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr& /*msg*/) {
    // No-op: required to avoid bad_function_call
  });
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send a message so we can capture the client_id for comparison
  client.send("identify");
  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(*middleware_, data, client_id));
  ASSERT_FALSE(client_id.empty());

  // Now close the client — this should trigger the disconnect callback
  client.stop();

  // Wait for the callback to fire
  {
    std::unique_lock<std::mutex> lock(dc_mutex);
    ASSERT_TRUE(dc_cv.wait_for(lock, std::chrono::seconds(2), [&] { return disconnect_fired.load(); }))
        << "Disconnect callback never fired";
  }

  EXPECT_EQ(disconnected_id, client_id);
}

TEST_F(WebSocketMiddlewareTest, SendBinaryToConnectedClient) {
  auto result = middleware_->initialize(18093);
  ASSERT_TRUE(result.has_value());

  // Set up the client to capture binary messages
  std::vector<uint8_t> received_binary;
  std::mutex bin_mutex;
  std::condition_variable bin_cv;
  bool binary_received = false;

  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18093");
  client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
      std::lock_guard<std::mutex> lock(bin_mutex);
      received_binary.assign(msg->str.begin(), msg->str.end());
      binary_received = true;
      bin_cv.notify_one();
    }
  });
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Register the client by sending a text message and reading it on the server
  client.send("register");

  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(*middleware_, data, client_id));
  ASSERT_FALSE(client_id.empty());

  // Send binary data from the server to this client
  std::vector<uint8_t> binary_payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  EXPECT_TRUE(middleware_->send_binary(client_id, binary_payload));

  // Wait for the client to receive the binary data
  {
    std::unique_lock<std::mutex> lock(bin_mutex);
    ASSERT_TRUE(bin_cv.wait_for(lock, std::chrono::seconds(2), [&] { return binary_received; }))
        << "Client never received binary data";
  }

  ASSERT_EQ(received_binary.size(), binary_payload.size());
  EXPECT_EQ(received_binary, binary_payload);

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WebSocketMiddlewareTest, ShutdownWithConnectedClientDoesNotDeadlock) {
  auto result = middleware_->initialize(18094);
  ASSERT_TRUE(result.has_value());

  // Set a disconnect callback (simulates BridgeServer's callback).
  // The fix clears callbacks before server_->stop(), so this must NOT fire
  // during shutdown — otherwise it would call into a potentially dead object.
  std::atomic<bool> disconnect_fired{false};
  middleware_->set_on_disconnect([&](const std::string& /*client_id*/) { disconnect_fired.store(true); });

  // Connect a real WebSocket client
  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18094");
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr& /*msg*/) {});
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Call shutdown() in a separate thread so we can detect a deadlock via timeout.
  // Before the fix, shutdown() held state_mutex_ while calling server_->stop();
  // the Close event fired on IX IO threads tried to acquire the same mutex → deadlock.
  std::atomic<bool> shutdown_completed{false};
  std::thread shutdown_thread([&]() {
    middleware_->shutdown();
    shutdown_completed.store(true);
  });

  // Wait up to 5 seconds — far more than enough for a clean shutdown
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!shutdown_completed.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  EXPECT_TRUE(shutdown_completed.load()) << "shutdown() deadlocked with a connected client";

  if (shutdown_thread.joinable()) {
    shutdown_thread.join();
  }

  EXPECT_FALSE(middleware_->is_ready());

  // Disconnect callback must NOT have fired during shutdown — callbacks are
  // cleared before the server is stopped to prevent use-after-free.
  EXPECT_FALSE(disconnect_fired.load());

  client.stop();
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
