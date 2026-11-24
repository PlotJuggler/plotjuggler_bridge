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

/// Delivery class of a per-client binary frame. A `kHeavy` (large,
/// single-message) frame waits behind the `kNormal` frames queued for the same
/// client, and replaces a still-queued heavy frame with the same key, so a slow
/// client gets the newest large message instead of a growing backlog. See
/// docs/API.md, "Slow clients / backpressure".
enum class FramePriority { kNormal, kHeavy };

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

  /// Queue a text reply for a specific client. Never blocks; replies are
  /// never dropped and keep their order relative to kNormal binary frames.
  /// @return true if the message was queued, false if the client is gone.
  virtual bool send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) = 0;

  /// Queue binary data for a specific client (per-client aggregated frames).
  /// Never blocks: frames to a slow client queue up and are dropped by the
  /// implementation's backpressure policy instead of stalling the caller.
  /// @param priority see FramePriority.
  /// @param key for kHeavy frames, the coalescing key (the topic name): a newer
  ///        heavy frame replaces a queued one with the same key. Ignored for kNormal.
  /// @return false if the client is gone (nothing was queued).
  virtual bool send_binary(
      const std::string& client_identity, const std::vector<uint8_t>& data,
      FramePriority priority = FramePriority::kNormal, const std::string& key = {}) = 0;

  /// Discard any queued outbound data for this client (e.g. when its session
  /// is destroyed server-side while the socket stays open). Default no-op for
  /// implementations without per-client outbound queues.
  virtual void drop_pending(const std::string& /*client_identity*/) {}

  /// @return true while the transport is in the middle of writing a frame to
  ///         this client. On a slow link that write can outlast the session
  ///         timeout, and the client's heartbeats may sit unread behind it, so
  ///         BridgeServer does not time out a client it is still sending to.
  virtual bool is_sending(const std::string& /*client_identity*/) const {
    return false;
  }

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
