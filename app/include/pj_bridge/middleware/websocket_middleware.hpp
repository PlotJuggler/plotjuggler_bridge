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

#pragma once

#include <cstdint>
#include <string>

namespace pj_bridge {

/// Server-side TLS configuration (certificate + private key paths). Both
/// files are validated for existence/readability in initialize().
struct TlsConfig {
  std::string certfile;
  std::string keyfile;
};

}  // namespace pj_bridge

#ifdef PJ_BRIDGE_WS_IX
#include "pj_bridge/middleware/websocket_middleware_ix.hpp"
#else

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "pj_bridge/middleware/middleware_interface.hpp"
#include "pj_bridge/middleware/outbound_queue.hpp"

namespace pj_bridge {

/// WebSocket server on Boost.Beast, driven by a single io thread.
///
/// Every connection keeps one asynchronous read and at most one asynchronous
/// write outstanding on that thread, so a slow client only delays its own
/// frames: send_reply()/send_binary() push to the client's OutboundQueue and
/// wake its writer, they never block. Each accepted socket gets
/// TCP_NOTSENT_LOWAT (kMaxUnsentBytes), so the kernel holds little unsent data
/// and the backlog stays in the OutboundQueue, where old frames can still be
/// dropped (docs/API.md, "Slow clients / backpressure").
///
/// Connect/disconnect callbacks run on the io thread; shutdown() joins it, so
/// no callback runs after shutdown() returns.
///
/// Thread safety: all public methods may be called from any thread, except
/// shutdown() and the destructor, which must not run on the io thread (from a
/// callback).
class WebSocketMiddleware : public MiddlewareInterface {
 public:
  /// Default per-client limit of queued normal frames: about 20 s at the
  /// default 50 Hz publish rate, enough to keep small topics whole while a
  /// large frame crosses a slow link. Memory is bounded separately, by bytes
  /// (OutboundQueue::kDefaultMaxNormalBytes).
  static constexpr int kDefaultClientBacklogSize = 1000;

  explicit WebSocketMiddleware(
      size_t client_backlog_size = kDefaultClientBacklogSize, std::optional<TlsConfig> tls = std::nullopt);
  ~WebSocketMiddleware() override;

  WebSocketMiddleware(const WebSocketMiddleware&) = delete;
  WebSocketMiddleware& operator=(const WebSocketMiddleware&) = delete;
  WebSocketMiddleware(WebSocketMiddleware&&) = delete;
  WebSocketMiddleware& operator=(WebSocketMiddleware&&) = delete;

  tl::expected<void, std::string> initialize(uint16_t port) override;
  void shutdown() override;
  bool receive_request(std::vector<uint8_t>& data, std::string& client_identity) override;
  bool send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) override;
  // NOTE: default arguments live only on the base MiddlewareInterface declaration.
  bool send_binary(
      const std::string& client_identity, const std::vector<uint8_t>& data, FramePriority priority,
      const std::string& key) override;
  bool is_ready() const override;
  void set_on_connect(ConnectionCallback callback) override;
  void set_on_disconnect(ConnectionCallback callback) override;
  void drop_pending(const std::string& client_identity) override;
  bool heard_from_client_within(const std::string& client_identity, std::chrono::milliseconds window) const override;

  /// kNormal frames dropped for slow clients (backlog overflow or drop_pending),
  /// summed over the middleware's lifetime.
  uint64_t dropped_frame_count() const;

  /// kHeavy frames replaced by a newer frame of the same topic before they
  /// were sent, summed over the middleware's lifetime.
  uint64_t heavy_shed_count() const;

  /// TCP_NOTSENT_LOWAT set on every client socket: the most unsent data the
  /// kernel buffers before the socket stops being writable. At 128 KiB/s
  /// (about 1 Mbit/s) this adds at most one second of latency, while the bytes
  /// already in flight (and so the throughput of fast links) are not limited.
  static constexpr int kMaxUnsentBytes = 128 * 1024;

 private:
  struct Io;      // io_context, acceptor, TLS context and io thread (Asio types stay in the .cpp)
  class Session;  // one connection; lives on the io thread
  template <class Stream>
  class SessionImpl;  // plain or TLS

  struct IncomingRequest {
    std::string client_id;
    std::vector<uint8_t> data;
  };

  // Io-thread events from a Session.
  void on_open(const std::shared_ptr<Session>& session);
  void on_message(const std::string& client_id, std::string text);
  void on_closed(const std::string& client_id);
  void do_accept(Io& io);
  /// Push to a connected client's queue and wake its writer. Under
  /// clients_mutex_, so no Session handle escapes the map (shutdown() destroys
  /// the io_context only after emptying it).
  template <class Push>
  bool push_to(const std::string& client_identity, Push&& push);

  std::unique_ptr<Io> io_;       // guarded by state_mutex_; set by initialize(), moved out by shutdown()
  uint64_t next_client_id_ = 0;  // io thread only

  std::deque<IncomingRequest> incoming_queue_;
  mutable std::mutex queue_mutex_;

  // Connected clients and the counters of departed ones. Guarded by clients_mutex_.
  std::unordered_map<std::string, std::shared_ptr<Session> > clients_;
  uint64_t dropped_from_departed_ = 0;
  uint64_t shed_from_departed_ = 0;
  mutable std::mutex clients_mutex_;

  size_t client_backlog_size_;
  std::optional<TlsConfig> tls_;

  ConnectionCallback on_connect_;
  ConnectionCallback on_disconnect_;
  mutable std::mutex state_mutex_;
  bool initialized_ = false;

  static constexpr int kShutdownTimeoutSeconds = 3;
  static constexpr size_t kMaxIncomingQueueSize = 1024;
};

}  // namespace pj_bridge

#endif  // PJ_BRIDGE_WS_IX
