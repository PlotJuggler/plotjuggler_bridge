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
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "pj_bridge/middleware/websocket_middleware.hpp"

using namespace pj_bridge;

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
  EXPECT_EQ(middleware_->send_binary("nonexistent_client", data, FramePriority::kNormal), SendResult::kClientGone);
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
  EXPECT_EQ(middleware_->send_binary(client_id, binary_payload, FramePriority::kNormal), SendResult::kDelivered);

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

// With the socket watermark forced to 0, every connected client is treated as
// congested, which makes the otherwise-unreachable over-watermark path testable
// with a real connection: a kHeavy frame is shed (and counted) before transmit,
// while a kNormal frame is queued. This covers the shed-counter wiring that
// lives outside the pure run_backpressure() policy.
TEST(WebSocketMiddlewareShedTest, HeavyFrameShedUnderForcedCongestionIncrementsCounter) {
  WebSocketMiddleware middleware(/*client_backlog_size=*/100, std::nullopt, /*socket_buffer_watermark=*/0);
  ASSERT_TRUE(middleware.initialize(18110).has_value());

  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18110");
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr& /*msg*/) {});
  client.start();
  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  client.send("register");
  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(middleware, data, client_id));
  ASSERT_FALSE(client_id.empty());

  // The middleware is content-agnostic; it acts on the priority argument, not
  // the frame bytes. A >=16-byte buffer is a realistic frame size.
  std::vector<uint8_t> frame(32, 0x7F);

  EXPECT_EQ(middleware.heavy_shed_count(), 0u);

  EXPECT_EQ(middleware.send_binary(client_id, frame, FramePriority::kHeavy), SendResult::kShed);
  EXPECT_EQ(middleware.heavy_shed_count(), 1u);

  EXPECT_EQ(middleware.send_binary(client_id, frame, FramePriority::kHeavy), SendResult::kShed);
  EXPECT_EQ(middleware.heavy_shed_count(), 2u);

  // A normal frame under the same congestion is queued, not shed.
  EXPECT_EQ(middleware.send_binary(client_id, frame, FramePriority::kNormal), SendResult::kQueued);
  EXPECT_EQ(middleware.heavy_shed_count(), 2u);

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

// ---------------------------------------------------------------------------
// Bug #6 — Unbounded incoming queue
//
// If messages arrive faster than they are consumed, the incoming queue must
// be bounded to prevent unbounded memory growth.
// ---------------------------------------------------------------------------
TEST_F(WebSocketMiddlewareTest, IncomingQueueBounded) {
  auto result = middleware_->initialize(18095);
  ASSERT_TRUE(result.has_value());

  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18095");
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr& /*msg*/) {});
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Send more messages than the queue limit (kMaxIncomingQueueSize = 1024)
  const int num_messages = 2000;
  for (int i = 0; i < num_messages; ++i) {
    client.send("msg_" + std::to_string(i));
  }

  // Give time for all messages to arrive at the server
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // Drain the queue and count how many we got
  int received_count = 0;
  std::vector<uint8_t> data;
  std::string client_id;
  while (middleware_->receive_request(data, client_id)) {
    received_count++;
  }

  // We should have received at most kMaxIncomingQueueSize messages
  EXPECT_LE(received_count, 1024) << "Queue should be bounded to 1024 messages";
  EXPECT_GT(received_count, 0) << "Should have received at least some messages";

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ---------------------------------------------------------------------------
// Task 4 — Slow-client backpressure (bounded queue, drop-oldest)
//
// The drop-oldest policy itself is exercised by BoundedFrameQueue's own unit
// tests (test_bounded_frame_queue.cpp), since forcing a real socket's
// bufferedAmount() over the 1 MiB watermark is not realistically achievable
// in a fast unit test. These tests instead cover the parts that only make
// sense with a live WebSocketMiddleware: normal traffic never counts as
// dropped, and the constructor accepts an explicit backlog size.
// ---------------------------------------------------------------------------
TEST_F(WebSocketMiddlewareTest, DroppedFrameCountZeroAfterNormalTraffic) {
  auto result = middleware_->initialize(18096);
  ASSERT_TRUE(result.has_value());

  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18096");
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr& /*msg*/) {});
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  client.send("register");
  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(*middleware_, data, client_id));
  ASSERT_FALSE(client_id.empty());

  std::vector<uint8_t> payload = {1, 2, 3, 4};
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(middleware_->send_binary(client_id, payload, FramePriority::kNormal), SendResult::kDelivered);
  }

  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Regression guard for the disconnect-race fix in send_binary's enqueue path:
// a client that is not (or no longer) in clients_ must never get a
// pending_frames_ entry created for it. The over-watermark branch itself
// cannot be reached from a unit test (we cannot force a real socket's
// bufferedAmount() past 1 MiB), so this verifies the shared contract via the
// public API: send_binary for an id absent from clients_ returns false and
// leaves no pending state behind — dropped_frame_count() stays 0 even after
// repeated attempts. The in-branch re-check under clients_mutex_ enforces the
// same "client gone -> return false, no queue created" rule; only the actual
// interleaving with a concurrent disconnect is not exercised here.
TEST_F(WebSocketMiddlewareTest, SendBinaryToAbsentClientCreatesNoPendingState) {
  auto result = middleware_->initialize(18098);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data = {1, 2, 3};
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(middleware_->send_binary("never-connected", data, FramePriority::kNormal), SendResult::kClientGone);
  }
  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  // Shutdown folds any (erroneously) surviving per-client queues into the
  // disconnected-clients accumulator — the count must still be zero after.
  middleware_->shutdown();
  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);
}

// drop_pending discards a client's backpressure backlog when its session is
// destroyed server-side (heartbeat timeout) while the socket stays open. A
// unit test cannot force the over-watermark path that populates the backlog,
// so this verifies the reachable contract: drop_pending is a safe idempotent
// no-op for clients with no pending state (never connected, or connected but
// never backlogged), never skews dropped_frame_count(), and leaves a live
// client's socket fully usable — send_binary still succeeds afterwards.
TEST_F(WebSocketMiddlewareTest, DropPendingIsSafeNoOpAndKeepsSocketUsable) {
  auto result = middleware_->initialize(18099);
  ASSERT_TRUE(result.has_value());

  // No such client: must not crash or create state.
  middleware_->drop_pending("never-connected");
  middleware_->drop_pending("never-connected");
  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  // Connected client with an empty backlog: drop_pending must not touch the
  // socket itself.
  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18099");
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr& /*msg*/) {});
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  client.send("register");
  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(*middleware_, data, client_id));
  ASSERT_FALSE(client_id.empty());

  middleware_->drop_pending(client_id);
  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  std::vector<uint8_t> payload = {1, 2, 3, 4};
  EXPECT_EQ(middleware_->send_binary(client_id, payload, FramePriority::kNormal), SendResult::kDelivered);
  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST(WebSocketMiddlewareBacklogConstructionTest, ExplicitBacklogSizeConstructsAndInitializes) {
  WebSocketMiddleware middleware(5);
  EXPECT_EQ(middleware.dropped_frame_count(), 0u);

  auto result = middleware.initialize(18097);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(middleware.is_ready());

  middleware.shutdown();
}

// ---------------------------------------------------------------------------
// Task 6 — TLS (wss://) via OpenSSL, server certificate only
// ---------------------------------------------------------------------------

TEST(WebSocketMiddlewareTlsTest, MissingCertFileFailsInitializeWithPathInMessage) {
  pj_bridge::TlsConfig tls{"/nonexistent/cert.pem", "/nonexistent/key.pem"};
  WebSocketMiddleware middleware(100, tls);

  auto result = middleware.initialize(18099);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().find("/nonexistent/cert.pem"), std::string::npos)
      << "Error message should mention the missing cert file path: " << result.error();
}

#ifdef IXWEBSOCKET_USE_TLS
namespace {

// Generates a throwaway self-signed cert/key pair under `dir` using the
// system `openssl` binary. Returns true on success; the caller should
// GTEST_SKIP() when this fails (missing openssl binary, sandboxed
// environment, etc.) rather than fail the test outright.
bool generate_self_signed_cert(const std::string& certfile, const std::string& keyfile) {
  std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + keyfile + " -out " + certfile +
                    " -days 1 -nodes -subj /CN=localhost 2>/dev/null";
  int rc = std::system(cmd.c_str());
  return rc == 0 && std::filesystem::exists(certfile) && std::filesystem::exists(keyfile);
}

}  // namespace

TEST(WebSocketMiddlewareTlsTest, TlsRoundTrip) {
  auto dir = std::filesystem::temp_directory_path() / "pj_bridge_tls_test";
  std::filesystem::create_directories(dir);
  std::string certfile = (dir / "cert.pem").string();
  std::string keyfile = (dir / "key.pem").string();

  if (!generate_self_signed_cert(certfile, keyfile)) {
    GTEST_SKIP() << "Could not generate a self-signed certificate (openssl binary unavailable or failed)";
  }

  pj_bridge::TlsConfig tls{certfile, keyfile};
  WebSocketMiddleware middleware(100, tls);

  auto result = middleware.initialize(18199);
  ASSERT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error());
  ASSERT_TRUE(middleware.is_ready());

  ix::WebSocket client;
  client.setUrl("wss://127.0.0.1:18199");

  ix::SocketTLSOptions client_tls_options;
  client_tls_options.caFile = "NONE";  // self-signed cert: disable peer verification
  client.setTLSOptions(client_tls_options);

  std::mutex msg_mutex;
  std::condition_variable msg_cv;
  std::string received_message;
  bool message_received = false;

  client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Message && !msg->binary) {
      std::lock_guard<std::mutex> lock(msg_mutex);
      received_message = msg->str;
      message_received = true;
      msg_cv.notify_one();
    }
  });
  client.start();

  ASSERT_TRUE(wait_for_client_open(client)) << "TLS client failed to connect";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const std::string test_message = "hello over wss";
  client.send(test_message);

  std::vector<uint8_t> data;
  std::string client_id;
  ASSERT_TRUE(poll_receive_request(middleware, data, client_id)) << "receive_request() never returned the message";
  std::string received(data.begin(), data.end());
  EXPECT_EQ(received, test_message);

  const std::string reply_text = "reply over wss";
  std::vector<uint8_t> reply_data(reply_text.begin(), reply_text.end());
  EXPECT_TRUE(middleware.send_reply(client_id, reply_data));

  {
    std::unique_lock<std::mutex> lock(msg_mutex);
    ASSERT_TRUE(msg_cv.wait_for(lock, std::chrono::seconds(2), [&] { return message_received; }))
        << "TLS client never received the reply";
  }
  EXPECT_EQ(received_message, reply_text);

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  middleware.shutdown();

  std::filesystem::remove_all(dir);
}
#endif  // IXWEBSOCKET_USE_TLS

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
