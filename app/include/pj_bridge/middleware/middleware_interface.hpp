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
  /// @return true if the message was sent, false if the client is gone.
  virtual bool send_binary(const std::string& client_identity, const std::vector<uint8_t>& data) = 0;

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
