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

// End-to-end behaviour of the bridge for a client on a slow link (issue #19).
//
// Everything is observed from the wire only: a real BridgeServer and
// WebSocketMiddleware, a synthetic publisher, a bandwidth-limited TCP relay
// and real WebSocket clients that decode the binary frames. Nothing here
// depends on how the middleware queues or sends, so the same test checks any
// transport implementation.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iterator>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pj_bridge/bridge_server.hpp"
#include "pj_bridge/message_serializer.hpp"
#include "pj_bridge/middleware/websocket_middleware.hpp"
#include "pj_bridge/protocol_constants.hpp"

using namespace pj_bridge;
using Clock = std::chrono::steady_clock;

namespace {

uint64_t wall_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// ---------------------------------------------------------------------------
// SlowLinkRelay: TCP proxy whose server->client direction is limited to a
// fixed byte rate, like a slow network link. Client->server is unlimited.
// ---------------------------------------------------------------------------
class SlowLinkRelay {
 public:
  SlowLinkRelay(uint16_t listen_port, uint16_t target_port, size_t bytes_per_second)
      : target_port_(target_port), rate_(bytes_per_second) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr = loopback(listen_port);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd_, 8) != 0) {
      throw std::runtime_error("relay: cannot listen");
    }
    acceptor_ = std::thread([this] { accept_loop(); });
  }

  ~SlowLinkRelay() {
    stop_ = true;
    acceptor_.join();
    for (auto& t : pipes_) {
      t.join();
    }
    for (int fd : fds_) {
      close(fd);
    }
    close(listen_fd_);
  }

 private:
  static sockaddr_in loopback(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
  }

  // poll() for @p events with a short timeout, so stop_ is noticed.
  bool wait_for(int fd, short events) {
    pollfd p{fd, events, 0};
    while (!stop_) {
      if (poll(&p, 1, 50) > 0) {
        return true;
      }
    }
    return false;
  }

  void accept_loop() {
    while (wait_for(listen_fd_, POLLIN)) {
      int client = accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      int server = socket(AF_INET, SOCK_STREAM, 0);
      // A small receive buffer on the relay keeps the link's own buffering
      // small, so backpressure reaches the bridge as on a real slow link.
      int rcvbuf = 64 * 1024;
      setsockopt(server, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
      sockaddr_in addr = loopback(target_port_);
      if (connect(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(client);
        close(server);
        continue;
      }
      fds_.push_back(client);
      fds_.push_back(server);
      pipes_.emplace_back([this, client, server] { pipe(client, server, 0); });
      pipes_.emplace_back([this, client, server] { pipe(server, client, rate_); });
    }
  }

  // Copy @p from -> @p to, at most @p rate bytes/s (0 = unlimited).
  void pipe(int from, int to, size_t rate) {
    std::vector<char> buf(4096);
    const auto start = Clock::now();
    size_t total = 0;
    while (wait_for(from, POLLIN)) {
      ssize_t n = recv(from, buf.data(), buf.size(), 0);
      if (n <= 0) {
        break;
      }
      for (ssize_t off = 0; off < n;) {
        if (!wait_for(to, POLLOUT)) {
          return;
        }
        ssize_t w = send(to, buf.data() + off, static_cast<size_t>(n - off), MSG_NOSIGNAL);
        if (w <= 0) {
          shutdown(from, SHUT_RDWR);
          return;
        }
        off += w;
      }
      total += static_cast<size_t>(n);
      if (rate > 0) {
        std::this_thread::sleep_until(start + std::chrono::microseconds(total * 1'000'000 / rate));
      }
    }
    shutdown(to, SHUT_WR);
  }

  uint16_t target_port_;
  size_t rate_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread acceptor_;
  std::vector<std::thread> pipes_;  // only touched by acceptor_ until the destructor joins it
  std::vector<int> fds_;
};

// ---------------------------------------------------------------------------
// Synthetic backend: two topics, delivered while subscribed.
// ---------------------------------------------------------------------------
class FakeTopicSource : public TopicSourceInterface {
 public:
  std::vector<TopicInfo> get_topics() override {
    return {{"/cloud", "sensor_msgs/msg/PointCloud2"}, {"/seq", "std_msgs/msg/UInt64"}};
  }
  std::string get_schema(const std::string&) override {
    return "uint64 data\n";
  }
  std::string schema_encoding() const override {
    return "ros2msg";
  }
};

class FakeSubscriptionManager : public SubscriptionManagerInterface {
 public:
  void set_message_callback(MessageCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }
  bool subscribe(const std::string& topic, const std::string&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++refs_[topic];
    return true;
  }
  bool unsubscribe(const std::string& topic) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return refs_[topic] > 0 && --refs_[topic] >= 0;
  }
  void unsubscribe_all() override {
    std::lock_guard<std::mutex> lock(mutex_);
    refs_.clear();
  }
  void deliver(const std::string& topic, std::shared_ptr<std::vector<std::byte>> data, uint64_t ts) {
    MessageCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (refs_[topic] <= 0) {
        return;
      }
      cb = callback_;
    }
    if (cb) {
      cb(topic, std::move(data), ts);
    }
  }

 private:
  std::mutex mutex_;
  MessageCallback callback_;
  std::unordered_map<std::string, int> refs_;
};

// ---------------------------------------------------------------------------
// Client: subscribes, heartbeats, and decodes every binary frame it receives.
// ---------------------------------------------------------------------------
struct Received {
  std::string topic;
  uint64_t seq;          // first 8 bytes of the payload
  double latency_s;      // receive wall time - publish timestamp
  Clock::time_point at;  // receive time
};

class Client {
 public:
  Client(uint16_t port, std::vector<std::string> topics) : topics_(std::move(topics)) {
    ws_.setUrl("ws://127.0.0.1:" + std::to_string(port));
    ws_.disablePerMessageDeflate();
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Open) {
        nlohmann::json sub;
        sub["command"] = "subscribe";
        sub["topics"] = topics_;
        ws_.send(sub.dump());
      } else if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
        decode(msg->str);
      }
    });
    ws_.start();
    heartbeat_ = std::thread([this] {
      while (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ws_.send(R"({"command":"heartbeat"})");
      }
    });
  }

  ~Client() {
    stop_ = true;
    heartbeat_.join();
    ws_.stop();
  }

  std::vector<Received> received(const std::string& topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Received> out;
    std::copy_if(received_.begin(), received_.end(), std::back_inserter(out), [&](const Received& r) {
      return r.topic == topic;
    });
    return out;
  }

 private:
  template <typename T>
  static T read_le(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
  }

  void decode(const std::string& frame) {
    const auto now = Clock::now();
    const uint64_t now_ns = wall_ns();
    const auto* bytes = reinterpret_cast<const uint8_t*>(frame.data());
    ASSERT_GE(frame.size(), 16u);
    ASSERT_EQ(read_le<uint32_t>(bytes), kBinaryFrameMagic);
    std::vector<uint8_t> compressed(bytes + 16, bytes + frame.size());
    std::vector<uint8_t> payload;
    AggregatedMessageSerializer::decompress_zstd(compressed, payload);
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t off = 0; off < payload.size();) {
      const auto len = read_le<uint16_t>(&payload[off]);
      std::string topic(reinterpret_cast<const char*>(&payload[off + 2]), len);
      off += 2 + len;
      const auto ts = read_le<uint64_t>(&payload[off]);
      const auto size = read_le<uint32_t>(&payload[off + 8]);
      const uint64_t seq = size >= 8 ? read_le<uint64_t>(&payload[off + 12]) : 0;
      off += 12 + size;
      received_.push_back({topic, seq, (static_cast<double>(now_ns) - static_cast<double>(ts)) * 1e-9, now});
    }
  }

  std::vector<std::string> topics_;
  ix::WebSocket ws_;
  std::thread heartbeat_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::vector<Received> received_;
};

size_t sequence_gaps(const std::vector<Received>& msgs) {
  size_t gaps = 0;
  for (size_t i = 1; i < msgs.size(); ++i) {
    if (msgs[i].seq != msgs[i - 1].seq + 1) {
      ++gaps;
    }
  }
  return gaps;
}

// A real bridge (BridgeServer + WebSocketMiddleware) driven like the
// standalone entry point, fed by a synthetic publisher: "/seq" at 50 Hz (8-byte
// sequence number) and "/cloud" at 10 Hz (incompressible, so ZSTD cannot
// shrink it; its first 8 bytes are a sequence number too).
class Bridge {
 public:
  Bridge(uint16_t port, double publish_rate, double session_timeout, size_t cloud_bytes)
      : server_(topics_, subs_, middleware_, make_config(port, publish_rate, session_timeout)),
        publish_period_(std::chrono::duration<double>(1.0 / publish_rate)) {
    if (!server_.initialize()) {
      throw std::runtime_error("bridge: initialize failed");
    }
    loop_ = std::thread([this] { run_loop(); });
    publisher_ = std::thread([this, cloud_bytes] { publish(cloud_bytes); });
  }

  ~Bridge() {
    stop_ = true;
    loop_.join();
    publisher_.join();
  }

  size_t session_count() const {
    return server_.get_active_session_count();
  }

 private:
  static BridgeServerConfig make_config(uint16_t port, double publish_rate, double session_timeout) {
    BridgeServerConfig config;
    config.port = port;
    config.publish_rate = publish_rate;
    config.session_timeout = session_timeout;
    return config;
  }

  void run_loop() {
    auto next_publish = Clock::now();
    auto next_timeout_check = Clock::now();
    while (!stop_) {
      server_.process_requests();
      if (Clock::now() >= next_publish) {
        server_.publish_aggregated_messages();
        next_publish += std::chrono::duration_cast<Clock::duration>(publish_period_);
      }
      if (Clock::now() >= next_timeout_check) {
        server_.check_session_timeouts();
        next_timeout_check = Clock::now() + std::chrono::milliseconds(200);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void publish(size_t cloud_bytes) {
    std::vector<std::byte> cloud(cloud_bytes);
    std::mt19937_64 rng(42);
    for (auto& b : cloud) {
      b = static_cast<std::byte>(rng());
    }
    uint64_t seq = 0;
    uint64_t cloud_seq = 0;
    auto next = Clock::now();
    while (!stop_) {
      auto small = std::make_shared<std::vector<std::byte>>(8);
      std::memcpy(small->data(), &seq, 8);
      subs_->deliver("/seq", small, wall_ns());
      if (seq % 5 == 0) {  // 10 Hz
        auto big = std::make_shared<std::vector<std::byte>>(cloud);
        std::memcpy(big->data(), &cloud_seq, 8);
        ++cloud_seq;
        subs_->deliver("/cloud", big, wall_ns());
      }
      ++seq;
      next += std::chrono::milliseconds(20);  // 50 Hz
      std::this_thread::sleep_until(next);
    }
  }

  std::shared_ptr<FakeTopicSource> topics_ = std::make_shared<FakeTopicSource>();
  std::shared_ptr<FakeSubscriptionManager> subs_ = std::make_shared<FakeSubscriptionManager>();
  std::shared_ptr<WebSocketMiddleware> middleware_ = std::make_shared<WebSocketMiddleware>();
  BridgeServer server_;
  std::chrono::duration<double> publish_period_;
  std::atomic<bool> stop_{false};
  std::thread loop_;
  std::thread publisher_;
};

}  // namespace

// A slow client next to a fast one, both subscribed while a 1 MiB point cloud
// (10 Hz) and a small sequence-numbered topic (50 Hz) are published. The slow
// link carries 256 KiB/s: one cloud takes ~4 s, twice the session timeout.
TEST(SlowLinkIntegrationTest, SlowClientStaysLiveCompleteAndFreshWithoutHurtingOthers) {
  constexpr uint16_t kBridgePort = 18140;
  constexpr uint16_t kRelayPort = 18141;
  Bridge bridge(kBridgePort, /*publish_rate=*/50.0, /*session_timeout=*/2.0, /*cloud_bytes=*/1 << 20);
  SlowLinkRelay relay(kRelayPort, kBridgePort, /*bytes_per_second=*/256 * 1024);

  Client slow(kRelayPort, {"/cloud", "/seq"});
  Client fast(kBridgePort, {"/cloud", "/seq"});
  std::this_thread::sleep_for(std::chrono::seconds(16));

  const auto slow_seq = slow.received("/seq");
  const auto slow_cloud = slow.received("/cloud");
  const auto fast_seq = fast.received("/seq");
  const auto fast_cloud = fast.received("/cloud");

  // 1. The small topic arrives complete on the slow link, although a cloud
  //    keeps the link busy for seconds at a time.
  ASSERT_GT(slow_seq.size(), 100u);
  EXPECT_EQ(sequence_gaps(slow_seq), 0u) << "small-topic messages were dropped on the slow link";

  // 2. Clouds keep arriving and their latency stays bounded (newest data,
  //    no backlog growing without limit). One cloud takes ~4 s on the link.
  ASSERT_GE(slow_cloud.size(), 2u);
  // Worst case: wait for the cloud in flight, then send this one (~8 s).
  for (const auto& c : slow_cloud) {
    EXPECT_LT(c.latency_s, 12.0) << "cloud " << c.seq << " arrived " << c.latency_s << " s after publishing";
  }
  EXPECT_GT(slow_cloud.back().seq - slow_cloud.front().seq, slow_cloud.size() - 1)
      << "clouds were delivered in full order instead of skipping to the newest";

  // 3. The slow client keeps its session: a timed-out session loses its
  //    subscriptions, so data would stop reaching the client.
  EXPECT_EQ(bridge.session_count(), 2u);
  EXPECT_LT(Clock::now() - slow_seq.back().at, std::chrono::seconds(6)) << "data stopped reaching the slow client";

  // 4. The fast client is not slowed down by the slow one, and gets every
  //    message of both topics.
  ASSERT_GT(fast_seq.size(), 500u);
  EXPECT_EQ(sequence_gaps(fast_seq), 0u);
  ASSERT_GT(fast_cloud.size(), 100u);
  EXPECT_EQ(sequence_gaps(fast_cloud), 0u) << "clouds were dropped for a client that keeps up";
  std::vector<double> latencies;
  for (const auto& r : fast_seq) {
    latencies.push_back(r.latency_s);
  }
  std::sort(latencies.begin(), latencies.end());
  EXPECT_LT(latencies[latencies.size() * 99 / 100], 0.3) << "the fast client was delayed by the slow one";
}

// Publishing slower than a heavy topic puts several of its messages in one
// publish cycle. A client that keeps up must still get all of them: only a
// client that falls behind may skip to the newest.
TEST(SlowLinkIntegrationTest, ClientThatKeepsUpGetsEveryHeavyMessageWhenPublishingSlowerThanTheTopic) {
  constexpr uint16_t kBridgePort = 18142;
  Bridge bridge(kBridgePort, /*publish_rate=*/4.0, /*session_timeout=*/5.0, /*cloud_bytes=*/512 * 1024);

  Client client(kBridgePort, {"/cloud"});
  std::this_thread::sleep_for(std::chrono::seconds(4));

  const auto clouds = client.received("/cloud");
  ASSERT_GT(clouds.size(), 20u);
  EXPECT_EQ(sequence_gaps(clouds), 0u) << "heavy messages of one publish cycle replaced each other";
}

int main(int argc, char** argv) {
  ix::initNetSystem();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
