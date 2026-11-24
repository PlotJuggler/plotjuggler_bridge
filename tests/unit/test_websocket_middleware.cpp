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

#include <arpa/inet.h>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <ixwebsocket/IXWebSocket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "pj_bridge/middleware/websocket_middleware.hpp"

using namespace pj_bridge;

// Whether this build serves wss://: the IX transport needs an IXWebSocket built
// with TLS, the Beast one PJ_BRIDGE_TLS.
#if (defined(PJ_BRIDGE_WS_IX) && defined(IXWEBSOCKET_USE_TLS)) || (!defined(PJ_BRIDGE_WS_IX) && defined(PJ_BRIDGE_TLS))
#define PJ_TEST_SERVER_TLS 1
#endif

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
  EXPECT_FALSE(middleware_->send_binary("nonexistent_client", data, FramePriority::kNormal, ""));
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

// Helper: poll @p condition every 5 ms until it holds or @p timeout passes.
static bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// Helper: wait for the ix::WebSocket client to reach Open state (up to timeout_ms).
static bool wait_for_client_open(ix::WebSocket& client, int timeout_ms = 2000) {
  return wait_until(
      [&] { return client.getReadyState() == ix::ReadyState::Open; }, std::chrono::milliseconds(timeout_ms));
}

// Helper: point @p client at the local server on @p port, ignoring what it receives, and start it.
static void start_client(ix::WebSocket& client, int port) {
  client.setUrl("ws://127.0.0.1:" + std::to_string(port));
  client.setOnMessageCallback([](const ix::WebSocketMessagePtr&) {});
  client.start();
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

// Binary frames are already zstd-compressed; negotiating permessage-deflate
// would re-compress them with zlib on the publish thread.
TEST_F(WebSocketMiddlewareTest, ServerDeclinesPerMessageDeflate) {
  auto result = middleware_->initialize(18099);
  ASSERT_TRUE(result.has_value());

  std::mutex headers_mutex;
  std::string extensions_header;
  ix::WebSocket client;
  client.setUrl("ws://127.0.0.1:18099");
  client.setPerMessageDeflateOptions(ix::WebSocketPerMessageDeflateOptions(true));
  client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      std::lock_guard<std::mutex> lock(headers_mutex);
      auto it = msg->openInfo.headers.find("Sec-WebSocket-Extensions");
      if (it != msg->openInfo.headers.end()) {
        extensions_header = it->second;
      }
    }
  });
  client.start();
  ASSERT_TRUE(wait_for_client_open(client)) << "Client failed to connect";

  {
    std::lock_guard<std::mutex> lock(headers_mutex);
    EXPECT_EQ(extensions_header.find("permessage-deflate"), std::string::npos)
        << "server negotiated: " << extensions_header;
  }
  client.stop();
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
  EXPECT_TRUE(middleware_->send_binary(client_id, binary_payload, FramePriority::kNormal, ""));

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
// Slow-client backpressure: normal traffic never counts as dropped, and the
// constructor accepts an explicit backlog size. The queueing policy itself is
// covered by test_outbound_queue.cpp, stalled clients at the end of this file.
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
    EXPECT_TRUE(middleware_->send_binary(client_id, payload, FramePriority::kNormal, ""));
  }

  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  client.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// send_binary for an id that is not connected fails and leaves no per-client
// state behind: dropped_frame_count() stays 0, also after shutdown.
TEST_F(WebSocketMiddlewareTest, SendBinaryToAbsentClientCreatesNoPendingState) {
  auto result = middleware_->initialize(18098);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data = {1, 2, 3};
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(middleware_->send_binary("never-connected", data, FramePriority::kNormal, ""));
  }
  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);

  // Shutdown folds any (erroneously) surviving per-client queues into the
  // disconnected-clients accumulator — the count must still be zero after.
  middleware_->shutdown();
  EXPECT_EQ(middleware_->dropped_frame_count(), 0u);
}

// drop_pending discards a client's queued frames when its session is destroyed
// server-side (heartbeat timeout) while the socket stays open. It must be a
// safe no-op for unknown clients and leave a live client's socket usable.
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
  EXPECT_TRUE(middleware_->send_binary(client_id, payload, FramePriority::kNormal, ""));
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
#ifdef PJ_TEST_SERVER_TLS
  EXPECT_NE(result.error().find("/nonexistent/cert.pem"), std::string::npos)
      << "Error message should mention the missing cert file path: " << result.error();
#else
  EXPECT_NE(result.error().find("without TLS support"), std::string::npos) << result.error();
#endif
}

#if defined(IXWEBSOCKET_USE_TLS) && defined(PJ_TEST_SERVER_TLS)
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
#endif  // TLS

// ---------------------------------------------------------------------------
// Slow / stalled clients
// ---------------------------------------------------------------------------

namespace {

// A real client whose receive path stalls: its message callback parks on a
// gate, so IXWebSocket stops reading the socket and TCP backpressure builds up
// toward the server, as on a slow link. release() lets it drain.
class StalledClient {
 public:
  explicit StalledClient(int port) {
    ws_.setUrl("ws://127.0.0.1:" + std::to_string(port));
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (msg->type != ix::WebSocketMessageType::Message || !msg->binary) {
        return;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return released_; });
      binary_frames_.push_back(msg->str);
      cv_.notify_all();
    });
    ws_.start();
  }

  ~StalledClient() {
    release();
    ws_.stop();
  }

  ix::WebSocket& ws() {
    return ws_;
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

  /// Wait until at least @p count binary frames arrived (after release()).
  std::vector<std::string> wait_for_frames(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return binary_frames_.size() >= count; });
    return binary_frames_;
  }

 private:
  ix::WebSocket ws_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool released_ = false;
  std::vector<std::string> binary_frames_;
};

// Connect @p ws, send one text message and return the server-side client id.
std::string register_client(WebSocketMiddleware& mw, ix::WebSocket& ws) {
  if (!wait_for_client_open(ws)) {
    return {};
  }
  ws.send("register");
  std::vector<uint8_t> data;
  std::string client_id;
  return poll_receive_request(mw, data, client_id) ? client_id : std::string{};
}

}  // namespace

// Reproduces the "one slow client stalls the bridge" bug: IXWebSocket server
// sends block until the kernel accepts every byte, so pushing frames to a
// client that stopped reading parked the caller (the bridge's single
// request/publish/timeout thread) and with it every other client.
TEST(WebSocketMiddlewareSlowClientTest, StalledClientNeverBlocksSenderOrOtherClients) {
  WebSocketMiddleware mw;
  ASSERT_TRUE(mw.initialize(18120).has_value());

  StalledClient slow(18120);
  const std::string slow_id = register_client(mw, slow.ws());
  ASSERT_FALSE(slow_id.empty());

  std::atomic<int> healthy_replies{0};
  ix::WebSocket healthy;
  healthy.setUrl("ws://127.0.0.1:18120");
  healthy.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Message && !msg->binary) {
      healthy_replies++;
    }
  });
  healthy.start();
  const std::string healthy_id = register_client(mw, healthy);
  ASSERT_FALSE(healthy_id.empty());

  // Far more than the loopback socket buffers can absorb (a few MB each way).
  const std::vector<uint8_t> frame(1 << 20, 0x5A);
  const std::vector<uint8_t> reply = {'o', 'k'};
  auto sender = std::async(std::launch::async, [&] {
    for (int i = 0; i < 32; ++i) {
      mw.send_binary(slow_id, frame, FramePriority::kNormal, "");
    }
    mw.send_reply(healthy_id, reply);
  });
  const bool sender_finished = sender.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

  wait_until([&] { return healthy_replies > 0; }, std::chrono::seconds(2));
  const int replies_while_stalled = healthy_replies;

  slow.release();  // let a blocked sender (pre-fix) finish before asserting
  sender.wait();

  EXPECT_TRUE(sender_finished) << "send_binary blocked on a client that stopped reading";
  EXPECT_EQ(replies_while_stalled, 1) << "a healthy client was starved by a stalled one";

  healthy.stop();
}

// Issue #19: a slow client got every large message in order, so its latency
// grew without bound. Now a heavy frame still queued is replaced by the newer
// one of the same topic, and the client ends with the newest.
TEST(WebSocketMiddlewareSlowClientTest, StalledClientGetsTheNewestHeavyFrame) {
  WebSocketMiddleware mw;
  ASSERT_TRUE(mw.initialize(18122).has_value());

  StalledClient slow(18122);
  const std::string slow_id = register_client(mw, slow.ws());
  ASSERT_FALSE(slow_id.empty());

  constexpr int kFrames = 20;
  for (int i = 1; i <= kFrames; ++i) {
    const std::vector<uint8_t> frame(1 << 20, static_cast<uint8_t>(i));  // first byte numbers the frame
    ASSERT_TRUE(mw.send_binary(slow_id, frame, FramePriority::kHeavy, "/cloud"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const uint64_t shed = mw.heavy_shed_count();
  // The socket buffers absorb a few frames before the stall reaches the
  // sender; every later frame but the newest must have been replaced.
  EXPECT_GE(shed, 5u);

  slow.release();
  const auto frames = slow.wait_for_frames(kFrames - shed, std::chrono::seconds(5));
  ASSERT_EQ(frames.size(), kFrames - shed);
  EXPECT_EQ(static_cast<uint8_t>(frames.back()[0]), kFrames) << "the client must end with the newest frame";
}

// A sender thread blocked in a send to a client that stopped reading must not
// keep shutdown() (and so the bridge's exit) waiting.
TEST(WebSocketMiddlewareSlowClientTest, ShutdownWithStalledClientDoesNotHang) {
  WebSocketMiddleware mw;
  ASSERT_TRUE(mw.initialize(18123).has_value());

  StalledClient slow(18123);
  const std::string slow_id = register_client(mw, slow.ws());
  ASSERT_FALSE(slow_id.empty());

  const std::vector<uint8_t> frame(1 << 20, 0x5A);
  for (int i = 0; i < 16; ++i) {
    mw.send_binary(slow_id, frame, FramePriority::kNormal, "");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // the sender is now blocked mid-send

  auto stopped = std::async(std::launch::async, [&] { mw.shutdown(); });
  const bool finished = stopped.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  slow.release();  // unblock a hung shutdown before asserting
  stopped.wait();
  EXPECT_TRUE(finished) << "shutdown() waited on a sender blocked by a stalled client";
}

// Reproduces the session-resurrection bug: text requests a client sent just
// before disconnecting stayed in the incoming queue, so processing them after
// the disconnect re-created (and immediately tore down) a session per request.
TEST(WebSocketMiddlewareSlowClientTest, RequestsOfDisconnectedClientAreDiscarded) {
  WebSocketMiddleware mw;
  std::atomic<bool> disconnected{false};
  mw.set_on_disconnect([&](const std::string&) { disconnected = true; });
  ASSERT_TRUE(mw.initialize(18121).has_value());

  ix::WebSocket client;
  start_client(client, 18121);
  ASSERT_TRUE(wait_for_client_open(client));
  for (int i = 0; i < 5; ++i) {
    client.send("{\"command\":\"heartbeat\"}");
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let the server queue them
  client.stop();

  ASSERT_TRUE(wait_until([&] { return disconnected.load(); }, std::chrono::seconds(2)));

  std::vector<uint8_t> data;
  std::string client_id;
  EXPECT_FALSE(mw.receive_request(data, client_id)) << "stale request from a disconnected client was delivered";
}

// On a slow link the server's IXWebSocket thread can sit in a long send while
// the client's heartbeats wait unread in the kernel. heard_from_client_within()
// reports what the kernel received, so the bridge can tell such a client from
// a dead one.
TEST(WebSocketMiddlewareSlowClientTest, HeardFromClientWithinReflectsWhatTheSocketReceived) {
  WebSocketMiddleware mw;
  ASSERT_TRUE(mw.initialize(18124).has_value());

  ix::WebSocket client;
  start_client(client, 18124);
  const std::string id = register_client(mw, client);
  ASSERT_FALSE(id.empty());

  EXPECT_TRUE(mw.heard_from_client_within(id, std::chrono::milliseconds(1000)));
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  EXPECT_FALSE(mw.heard_from_client_within(id, std::chrono::milliseconds(200))) << "the client sent nothing since";
  client.send("{\"command\":\"heartbeat\"}");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(mw.heard_from_client_within(id, std::chrono::milliseconds(200)));
  EXPECT_FALSE(mw.heard_from_client_within("never-connected", std::chrono::milliseconds(1000)));

  client.stop();
  mw.shutdown();
}

// A disconnect callback captures the BridgeServer: shutdown() must not return
// while one is still running, or the callback can outlive its target.
TEST(WebSocketMiddlewareSlowClientTest, ShutdownWaitsForARunningDisconnectCallback) {
  WebSocketMiddleware mw;
  std::atomic<bool> entered{false};
  std::atomic<bool> finished{false};
  mw.set_on_disconnect([&](const std::string&) {
    entered = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    finished = true;
  });
  ASSERT_TRUE(mw.initialize(18125).has_value());

  ix::WebSocket client;
  start_client(client, 18125);
  ASSERT_FALSE(register_client(mw, client).empty());
  client.stop();

  ASSERT_TRUE(wait_until([&] { return entered.load(); }, std::chrono::seconds(2)));
  mw.shutdown();
  EXPECT_TRUE(finished) << "shutdown() returned while the disconnect callback was still running";
}

// BridgeServer's disconnect callback takes locks that its publish cycle can
// hold while compressing frames: running it on the transport's I/O thread
// stalled every other client meanwhile.
TEST(WebSocketMiddlewareSlowClientTest, BlockedDisconnectCallbackDoesNotStallOtherClients) {
  WebSocketMiddleware mw;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<bool> entered{false};
  mw.set_on_disconnect([&entered, released](const std::string&) {
    entered = true;
    released.wait();
  });
  ASSERT_TRUE(mw.initialize(18126).has_value());

  ix::WebSocket leaving;
  ix::WebSocket staying;
  start_client(leaving, 18126);
  start_client(staying, 18126);
  ASSERT_FALSE(register_client(mw, leaving).empty());
  ASSERT_FALSE(register_client(mw, staying).empty());
  leaving.stop();
  ASSERT_TRUE(wait_until([&] { return entered.load(); }, std::chrono::seconds(2)));

  staying.send("still here");
  std::vector<uint8_t> data;
  std::string client_id;
  const bool received = poll_receive_request(mw, data, client_id, 1000);
  release.set_value();
  EXPECT_TRUE(received) << "a running disconnect callback stalled another client's requests";
  staying.stop();
  mw.shutdown();
}

#ifndef PJ_BRIDGE_WS_IX  // Beast transport only: the legacy IXWebSocket one lacks these guarantees

// Requests are small JSON: a huge message must not be buffered (Beast's
// default limit is 16 MiB per message, times 1024 queued requests).
TEST(WebSocketMiddlewareSlowClientTest, OversizedRequestClosesTheConnection) {
  WebSocketMiddleware mw;
  std::atomic<bool> disconnected{false};
  mw.set_on_disconnect([&](const std::string&) { disconnected = true; });
  ASSERT_TRUE(mw.initialize(18127).has_value());

  ix::WebSocket client;
  start_client(client, 18127);
  ASSERT_FALSE(register_client(mw, client).empty());
  client.send(std::string(2U << 20, 'x'));  // 2 MiB
  EXPECT_TRUE(wait_until([&] { return disconnected.load(); }, std::chrono::seconds(2)))
      << "a 2 MiB request was accepted";
  std::vector<uint8_t> data;
  std::string client_id;
  EXPECT_FALSE(mw.receive_request(data, client_id));
  client.stop();
}

// An exception thrown by a callback must not take the server down.
TEST(WebSocketMiddlewareSlowClientTest, ThrowingCallbackKeepsTheServerRunning) {
  WebSocketMiddleware mw;
  std::atomic<bool> threw{false};
  mw.set_on_disconnect([&threw](const std::string&) {
    threw = true;
    throw std::runtime_error("callback failed");
  });
  ASSERT_TRUE(mw.initialize(18128).has_value());

  ix::WebSocket first;
  start_client(first, 18128);
  ASSERT_FALSE(register_client(mw, first).empty());
  first.stop();
  ASSERT_TRUE(wait_until([&] { return threw.load(); }, std::chrono::seconds(2))) << "the callback never ran";

  ix::WebSocket second;
  start_client(second, 18128);
  EXPECT_FALSE(register_client(mw, second).empty()) << "the server stopped after a callback threw";
  second.stop();
  mw.shutdown();
}

namespace {

// A plain TCP connection to the local server that never sends the WebSocket
// upgrade request. Closes itself.
class RawConnection {
 public:
  explicit RawConnection(int port) : fd_(socket(AF_INET, SOCK_STREAM, 0)) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connected_ = fd_ >= 0 && connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  }
  ~RawConnection() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  RawConnection(const RawConnection&) = delete;
  RawConnection& operator=(const RawConnection&) = delete;

  bool connected() const {
    return connected_;
  }

  /// recv() of one byte, waiting at most @p timeout: 0 is an orderly close by
  /// the server, -1 with errno ECONNRESET a reset, -1 with EAGAIN a timeout.
  ssize_t recv_one(std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char byte = 0;
    return recv(fd_, &byte, 1, 0);
  }

 private:
  int fd_;
  bool connected_ = false;
};

// Runs a function when the scope ends, also when an ASSERT returns early.
template <class F>
struct OnExit {
  F f;
  ~OnExit() {
    f();
  }
};
template <class F>
OnExit(F) -> OnExit<F>;

// A gate a callback can wait on; open() may be called any number of times.
class Gate {
 public:
  void open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};

}  // namespace

// A connection that never completes the WebSocket handshake must not keep
// shutdown() waiting for its deadline.
TEST(WebSocketMiddlewareSlowClientTest, ShutdownClosesConnectionsStillInTheHandshake) {
  WebSocketMiddleware mw;
  ASSERT_TRUE(mw.initialize(18129).has_value());

  RawConnection raw(18129);
  ASSERT_TRUE(raw.connected());
  // Connections are accepted in order: once a later one is open, the server
  // has accepted the raw one and its session waits for the upgrade request.
  ix::WebSocket later;
  start_client(later, 18129);
  ASSERT_TRUE(wait_for_client_open(later));

  const auto start = std::chrono::steady_clock::now();
  mw.shutdown();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  later.stop();
  // The session closed the socket: an orderly end of stream (a connection
  // the server never accepted is reset instead when the listener closes).
  EXPECT_EQ(raw.recv_one(std::chrono::seconds(2)), 0) << "the session did not close the connection";
  EXPECT_LT(elapsed, std::chrono::seconds(1)) << "shutdown() waited for a connection still in the handshake";
}

// shutdown() may be called from several threads (a signal handler and a
// destructor): none may return while a callback that captured the caller's
// state is still running.
TEST(WebSocketMiddlewareSlowClientTest, ConcurrentShutdownWaitsForARunningCallback) {
  Gate gate;
  std::atomic<bool> entered{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> second_started{false};
  WebSocketMiddleware mw;
  mw.set_on_disconnect([&](const std::string&) {
    entered = true;
    gate.wait();
    finished = true;
  });
  ASSERT_TRUE(mw.initialize(18131).has_value());
  std::future<void> first;
  std::future<void> second;
  OnExit open_gate{[&] { gate.open(); }};  // destroyed first: no early return leaves the callback parked

  ix::WebSocket client;
  start_client(client, 18131);
  ASSERT_FALSE(register_client(mw, client).empty());
  client.stop();
  ASSERT_TRUE(wait_until([&] { return entered.load(); }, std::chrono::seconds(2)));

  first = std::async(std::launch::async, [&] { mw.shutdown(); });
  const bool first_began = wait_until([&] { return !mw.is_ready(); }, std::chrono::seconds(2));
  second = std::async(std::launch::async, [&] {
    second_started = true;
    mw.shutdown();
  });
  const bool second_began = wait_until([&] { return second_started.load(); }, std::chrono::seconds(2));
  const bool second_returned_early = second.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
  gate.open();
  first.wait();
  second.wait();
  EXPECT_TRUE(first_began);
  EXPECT_TRUE(second_began);
  EXPECT_FALSE(second_returned_early) << "a second shutdown() returned while the callback was still running";
  EXPECT_TRUE(finished);
}

// A client that does not read its replies is disconnected once they pass the
// reply budget, instead of queuing them without limit.
TEST(WebSocketMiddlewareSlowClientTest, ClientNotReadingItsRepliesIsDisconnected) {
  WebSocketMiddleware mw;
  std::atomic<bool> disconnected{false};
  mw.set_on_disconnect([&](const std::string&) { disconnected = true; });
  ASSERT_TRUE(mw.initialize(18133).has_value());

  StalledClient slow(18133);
  const std::string id = register_client(mw, slow.ws());
  ASSERT_FALSE(id.empty());
  ASSERT_TRUE(mw.send_binary(id, std::vector<uint8_t>(1, 0), FramePriority::kNormal, ""));  // parks the client

  const std::vector<uint8_t> reply(1U << 20, '"');
  int accepted = 0;
  while (accepted < 256 && mw.send_reply(id, reply)) {
    ++accepted;
  }
  EXPECT_GE(accepted, 64) << "replies were refused before the 64 MiB budget was used";
  EXPECT_LT(accepted, 256) << "256 MiB of unread replies were all queued";
  EXPECT_TRUE(wait_until([&] { return disconnected.load(); }, std::chrono::seconds(2)))
      << "the client over its reply budget was not disconnected";
  mw.shutdown();
}

// Every connection costs a session, so the server keeps a limit, as the
// IXWebSocket server did; connections still in the handshake count too.
TEST(WebSocketMiddlewareSlowClientTest, ConnectionsBeyondTheLimitAreRefused) {
  WebSocketMiddleware mw;
  ASSERT_TRUE(mw.initialize(18132).has_value());

  std::deque<RawConnection> held;  // fill every slot with a connection in the handshake
  for (size_t i = 0; i < WebSocketMiddleware::kMaxConnections; ++i) {
    held.emplace_back(18132);
    ASSERT_TRUE(held.back().connected());
  }
  // Accepted after all the others: refused at once, not left waiting.
  RawConnection probe(18132);
  ASSERT_TRUE(probe.connected());
  const ssize_t got = probe.recv_one(std::chrono::seconds(2));
  EXPECT_TRUE(got == 0 || (got < 0 && errno == ECONNRESET)) << "a connection beyond the limit was not closed";

  held.pop_back();  // frees one slot
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ix::WebSocket admitted;
  start_client(admitted, 18132);
  EXPECT_TRUE(wait_for_client_open(admitted, 2000)) << "a freed slot was not reused";
  admitted.stop();
  mw.shutdown();
}

// A client's Ping must not wait behind a large message crossing a slow link:
// a client with a short Pong deadline would drop a healthy connection.
TEST(WebSocketMiddlewareSlowClientTest, PongIsNotHeldBehindALargeMessage) {
  WebSocketMiddleware mw;
  ASSERT_TRUE(mw.initialize(18130).has_value());

  std::mutex mutex;
  std::condition_variable cv;
  bool released = false;
  bool parked = false;
  std::vector<std::string> events;  // what the client received, in order
  auto release = [&] {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    cv.notify_all();
  };
  ix::WebSocket client;
  OnExit unpark{release};  // destroyed before the client, whose stop() joins the parked callback
  client.setUrl("ws://127.0.0.1:18130");
  client.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    std::unique_lock<std::mutex> lock(mutex);
    if (msg->type == ix::WebSocketMessageType::Pong) {
      events.emplace_back("pong");
    } else if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
      if (msg->str.size() == 1) {  // the first frame parks the client: it stops reading
        parked = true;
        cv.notify_all();
        cv.wait(lock, [&] { return released; });
        return;
      }
      events.emplace_back("message");
    }
    cv.notify_all();
  });
  client.start();
  const std::string id = register_client(mw, client);
  ASSERT_FALSE(id.empty());

  ASSERT_TRUE(mw.send_binary(id, std::vector<uint8_t>(1, 0), FramePriority::kNormal, ""));
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return parked; }));
  }
  // Far more than the socket buffers hold, so the server is mid-write when the Ping arrives.
  ASSERT_TRUE(mw.send_binary(id, std::vector<uint8_t>(32U << 20, 0x5A), FramePriority::kNormal, ""));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  client.ping("are you there");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  release();
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return events.size() == 2; }));
  }
  EXPECT_EQ(events, (std::vector<std::string>{"pong", "message"})) << "the Pong waited for the whole large message";
  client.stop();
  mw.shutdown();
}
#endif  // PJ_BRIDGE_WS_IX

int main(int argc, char** argv) {
  // rcl_logging_spdlog registers a periodic flush thread on spdlog's global
  // registry (which we share) and its code lives in that library. rcl unloads
  // the library on shutdown, so after the suites' init/shutdown cycles the
  // thread would resume in unmapped memory at exit. Pin the library.
  dlopen("librcl_logging_spdlog.so", RTLD_NOW | RTLD_NODELETE);

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
