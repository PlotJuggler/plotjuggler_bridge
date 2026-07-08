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
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pj_bridge/middleware/backpressure.hpp"
#include "pj_bridge/middleware/bounded_frame_queue.hpp"
#include "pj_bridge/middleware/middleware_interface.hpp"

namespace pj_bridge {

/// Server-side TLS configuration (certificate + private key paths). Both
/// files are validated for existence/readability in initialize(), before any
/// IXWebSocket TLS API is touched.
struct TlsConfig {
  std::string certfile;
  std::string keyfile;
};

class WebSocketMiddleware : public MiddlewareInterface {
 public:
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
  bool publish_data(const std::vector<uint8_t>& data) override;
  bool send_binary(
      const std::string& client_identity, const std::vector<uint8_t>& data,
      FramePriority priority = FramePriority::kNormal) override;
  bool is_ready() const override;
  void set_on_connect(ConnectionCallback callback) override;
  void set_on_disconnect(ConnectionCallback callback) override;
  void drop_pending(const std::string& client_identity) override;

  /// Total number of frames dropped due to slow-client backpressure, summed
  /// across all clients (currently connected and already disconnected).
  uint64_t dropped_frame_count() const;

  /// Total number of kHeavy frames shed before transmit under congestion
  /// (dropped instead of queued), summed over the middleware's lifetime.
  uint64_t heavy_shed_count() const;

 private:
  struct IncomingRequest {
    std::string client_id;
    std::vector<uint8_t> data;
  };

  std::shared_ptr<ix::WebSocketServer> server_;

  std::queue<IncomingRequest> incoming_queue_;
  mutable std::mutex queue_mutex_;

  std::unordered_map<std::string, std::shared_ptr<ix::WebSocket>> clients_;
  // Per-client backlog of frames withheld while the client's socket buffer is
  // over the high watermark, and the steady-clock time each client's drop
  // warning was last logged. Both are guarded by `clients_mutex_` (the same
  // lock as `clients_`), not a separate mutex, so client lifecycle and
  // pending-queue lifecycle stay consistent with each other.
  std::unordered_map<std::string, BoundedFrameQueue> pending_frames_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_drop_warn_;
  mutable std::mutex clients_mutex_;

  // Sum of dropped_total() from clients that have since disconnected (their
  // BoundedFrameQueue entry is erased from pending_frames_ on disconnect, so
  // its count is folded in here to keep dropped_frame_count() a lifetime
  // total). Guarded by clients_mutex_.
  uint64_t dropped_from_disconnected_{0};

  // Lifetime count of kHeavy frames shed before transmit under congestion
  // (dropped rather than queued). Distinct from dropped_frame_count(), which
  // counts backlog-overflow drops of kNormal frames. Guarded by clients_mutex_.
  uint64_t heavy_shed_total_{0};

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

  // Lossy-send policy adapted from foxglove_bridge (MIT License, Copyright
  // (c) Foxglove Technologies Inc): once a client's outgoing socket buffer
  // exceeds this watermark, new frames are queued (dropping the oldest on
  // overflow) instead of blocking or disconnecting the client.
  static constexpr size_t kSocketBufferHighWatermark = 1u << 20;  // 1 MiB
  static constexpr int kDropWarnIntervalSeconds = 30;
};

}  // namespace pj_bridge
