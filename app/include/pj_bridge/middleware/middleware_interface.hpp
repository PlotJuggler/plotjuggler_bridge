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
#include <functional>
#include <string>
#include <vector>

#include "tl/expected.hpp"

namespace pj_bridge {

/// Delivery priority for a per-client binary frame. Under socket congestion a
/// `kHeavy` (large/size-class) frame is dropped before transmit rather than
/// queued, so one big frame cannot starve the small frames behind it; a
/// `kNormal` frame instead falls back to the queue-with-drop-oldest backlog.
/// (When the socket has room, both are sent immediately.) See docs/API.md.
enum class FramePriority { kNormal, kHeavy };

/// Outcome of send_binary(): how the transport handled the frame. Only
/// `kDelivered` and `kQueued` are (or will be) put on the wire; `kShed` and
/// `kClientGone` are never delivered, so callers must NOT count them as sent.
enum class SendResult {
  kDelivered,   ///< written to the socket now (or flushed from the backlog)
  kQueued,      ///< enqueued for later delivery (kNormal frame, socket congested)
  kShed,        ///< dropped before transmit (kHeavy frame, socket congested)
  kClientGone,  ///< the client disconnected; nothing was sent
};

/// Abstract transport layer between BridgeServer and clients.
///
/// Implementations handle connection management and bidirectional messaging.
/// The interface separates two data channels:
///   - **Text** (request/reply): JSON API commands (subscribe, heartbeat, etc.)
///   - **Binary** (per-client push): ZSTD-compressed aggregated message frames
///
/// Thread safety: implementations must be safe to call from multiple threads.
/// BridgeServer calls receive_request() and send_reply() from the request-processing
/// loop, and send_binary() from the publish loop, potentially concurrently.
class MiddlewareInterface {
 public:
  virtual ~MiddlewareInterface() = default;

  /// Start listening on the given port. Must be called before any other method.
  /// @return void on success, or an error string describing the failure.
  virtual tl::expected<void, std::string> initialize(uint16_t port) = 0;

  /// Stop the server and close all client connections.
  /// After this call, is_ready() must return false.
  virtual void shutdown() = 0;

  /// Poll for the next incoming text request (non-blocking or short-blocking).
  /// @param[out] data       the raw request bytes (UTF-8 JSON)
  /// @param[out] client_identity  opaque client identifier for send_reply()
  /// @return true if a request was dequeued, false if the queue was empty.
  virtual bool receive_request(std::vector<uint8_t>& data, std::string& client_identity) = 0;

  /// Send a text reply to a specific client.
  /// @return true if the message was sent, false if the client is gone.
  virtual bool send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) = 0;

  /// Broadcast binary data to all connected clients.
  /// @return true if at least one client received the data.
  virtual bool publish_data(const std::vector<uint8_t>& data) = 0;

  /// Send binary data to a specific client (used for per-client aggregated frames).
  /// @param priority kHeavy frames are shed before transmit under congestion
  ///        instead of queued (default kNormal preserves the legacy behavior).
  /// @return how the frame was handled (see SendResult). Callers counting
  ///         forwarded bytes/messages must treat only kDelivered/kQueued as
  ///         forwarded — kShed and kClientGone never reach the client.
  virtual SendResult send_binary(
      const std::string& client_identity, const std::vector<uint8_t>& data,
      FramePriority priority = FramePriority::kNormal) = 0;

  /// Discard any queued outbound data for this client (e.g. when its session
  /// is destroyed server-side while the socket stays open). Default no-op for
  /// implementations without per-client outbound queues.
  virtual void drop_pending(const std::string& /*client_identity*/) {}

  /// @return true if initialize() succeeded and shutdown() has not been called.
  virtual bool is_ready() const = 0;

  using ConnectionCallback = std::function<void(const std::string& client_id)>;

  /// Register a callback invoked when a new client connects.
  virtual void set_on_connect(ConnectionCallback callback) = 0;

  /// Register a callback invoked when a client disconnects.
  /// BridgeServer uses this to trigger session cleanup.
  virtual void set_on_disconnect(ConnectionCallback callback) = 0;
};

}  // namespace pj_bridge
