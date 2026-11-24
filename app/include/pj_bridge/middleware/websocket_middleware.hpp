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

#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pj_bridge/middleware/middleware_interface.hpp"
#include "pj_bridge/middleware/outbound_queue.hpp"

namespace pj_bridge {

/// Server-side TLS configuration (certificate + private key paths). Both
/// files are validated for existence/readability in initialize(), before any
/// IXWebSocket TLS API is touched.
struct TlsConfig {
  std::string certfile;
  std::string keyfile;
};

/// IXWebSocket server with one sender thread per client.
///
/// IXWebSocket's server-side send blocks until the kernel has taken every byte,
/// so calling it from the bridge's event loop let one slow client stall every
/// other client. Here send_reply()/send_binary() only push to the client's
/// OutboundQueue, and the client's own sender thread does the blocking send.
/// Each accepted socket also gets TCP_NOTSENT_LOWAT (kMaxUnsentBytes), so the
/// kernel holds little unsent data and the backlog stays in the OutboundQueue,
/// where old frames can still be dropped (docs/API.md, "Slow clients / backpressure").
///
/// Thread safety: all public methods may be called from any thread.
class WebSocketMiddleware : public MiddlewareInterface {
 public:
  /// @param client_backlog_size max kNormal frames queued per client before the oldest is dropped.
  explicit WebSocketMiddleware(size_t client_backlog_size = 100, std::optional<TlsConfig> tls = std::nullopt);
  ~WebSocketMiddleware() override;

  WebSocketMiddleware(const WebSocketMiddleware&) = delete;
  WebSocketMiddleware& operator=(const WebSocketMiddleware&) = delete;
  WebSocketMiddleware(WebSocketMiddleware&&) = delete;
  WebSocketMiddleware& operator=(WebSocketMiddleware&&) = delete;

  tl::expected<void, std::string> initialize(uint16_t port) override;
  void shutdown() override;
  bool receive_request(std::vector<uint8_t>& data, std::string& client_identity) override;
  bool send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) override;
  // NOTE: default arguments live only on the base MiddlewareInterface
  // declaration (defaults are bound statically, so repeating them here could
  // silently diverge).
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
  /// kernel buffers before a send waits. At 128 KiB/s (about 1 Mbit/s) this
  /// adds at most one second of latency, while the bytes already in flight
  /// (and so the throughput of fast links) are not limited.
  static constexpr int kMaxUnsentBytes = 128 * 1024;

 private:
  struct IncomingRequest {
    std::string client_id;
    std::vector<uint8_t> data;
  };

  /// Shared between a client's sender thread and the middleware.
  struct SenderState {
    std::atomic<bool> done{false};  ///< the sender thread has returned
    /// IXWebSocket's connection thread no longer uses the WebSocket.
    std::atomic<bool> connection_ended{false};
  };

  /// A connected (or departed, not yet reaped) client. The entry owns a
  /// reference to the WebSocket until both its sender thread and IXWebSocket's
  /// connection thread are done with it, so the WebSocket is always destroyed
  /// by the bridge's thread, after both.
  struct Client {
    std::string id;
    std::shared_ptr<ix::WebSocket> ws;
    std::shared_ptr<OutboundQueue> queue;
    std::thread sender;
    std::shared_ptr<SenderState> state;
  };

  /// Copy of a connected client's handles, or an empty Client if unknown.
  Client find_client(const std::string& client_identity) const;
  /// Fold a departing client's counters into the lifetime totals, stop its
  /// queue and park the client until it can be reaped. Requires clients_mutex_.
  void retire_client(Client& client);
  /// IXWebSocket's connection thread for @p client_id has finished.
  void mark_connection_ended(const std::string& client_id);

  /// Every IXWebSocket callback runs under a shared lock of this gate; shutdown()
  /// closes it under the exclusive lock, which also waits for callbacks already
  /// running. IXWebSocket's callback object itself is never replaced, since
  /// IXWebSocket does not synchronize that.
  struct CallbackGate {
    std::shared_mutex mutex;
    bool open = true;
  };
  std::shared_ptr<CallbackGate> gate_;
  /// Reap departed clients whose sender and connection threads have both
  /// finished. Called from the
  /// bridge's polling thread, never from an IXWebSocket callback.
  void reap_finished_senders();
  /// Remove parked clients from retired_ (all, or only those whose sender
  /// finished) for joining outside the lock. Requires clients_mutex_.
  std::vector<Client> take_retired(bool all);
  static void join_senders(std::vector<Client>& clients);

  std::shared_ptr<ix::WebSocketServer> server_;

  std::deque<IncomingRequest> incoming_queue_;
  mutable std::mutex queue_mutex_;

  // Connected clients, the sender threads of departed clients (joined later,
  // never from the IXWebSocket thread that reports the disconnect), and the
  // counters of departed clients. All guarded by clients_mutex_.
  std::unordered_map<std::string, Client> clients_;
  /// Socket of each live connection, by client id (from accept to connection end).
  std::unordered_map<std::string, int> connection_fds_;
  std::vector<Client> retired_;
  bool accepting_clients_ = false;
  uint64_t dropped_from_departed_ = 0;
  uint64_t shed_from_departed_ = 0;
  mutable std::mutex clients_mutex_;

  size_t client_backlog_size_;
  std::optional<TlsConfig> tls_;

  ConnectionCallback on_connect_;
  ConnectionCallback on_disconnect_;

  mutable std::mutex state_mutex_;
  bool initialized_;
  /// Stop thread that exceeded the shutdown timeout; joined in the destructor.
  std::thread pending_stop_thread_;

  static constexpr int kShutdownTimeoutSeconds = 3;
  static constexpr size_t kMaxIncomingQueueSize = 1024;
};

}  // namespace pj_bridge
