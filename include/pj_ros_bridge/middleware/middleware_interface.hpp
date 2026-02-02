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

#ifndef PJ_ROS_BRIDGE__MIDDLEWARE__MIDDLEWARE_INTERFACE_HPP_
#define PJ_ROS_BRIDGE__MIDDLEWARE__MIDDLEWARE_INTERFACE_HPP_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "tl/expected.hpp"

namespace pj_ros_bridge {

/**
 * @brief Abstract interface for connection-oriented middleware implementations
 *
 * This interface provides an abstraction layer over the networking middleware.
 * It uses a connection-oriented model where each client has a unique identity,
 * and replies are sent to specific clients by identity.
 *
 * Thread safety: Implementations should be thread-safe for concurrent calls.
 */
class MiddlewareInterface {
 public:
  virtual ~MiddlewareInterface() = default;

  /**
   * @brief Initialize the middleware on a single port
   *
   * @param port Port to listen on
   * @return void on success, error message string on failure
   */
  virtual tl::expected<void, std::string> initialize(uint16_t port) = 0;

  /**
   * @brief Shutdown the middleware and cleanup resources
   */
  virtual void shutdown() = 0;

  /**
   * @brief Receive a request from a client (with timeout)
   *
   * @param data Output buffer for received data
   * @param client_identity Output parameter for client identifier
   * @return true if request received, false on timeout or error
   */
  virtual bool receive_request(std::vector<uint8_t>& data, std::string& client_identity) = 0;

  /**
   * @brief Send a reply to a specific client
   *
   * @param client_identity Client to send to (from receive_request)
   * @param data Data to send as reply
   * @return true if reply sent successfully, false otherwise
   */
  virtual bool send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) = 0;

  /**
   * @brief Broadcast binary data to all connected clients
   *
   * @param data Data to broadcast
   * @return true if data sent to at least one client, false otherwise
   */
  virtual bool publish_data(const std::vector<uint8_t>& data) = 0;

  /**
   * @brief Check if middleware is initialized and ready
   *
   * @return true if ready, false otherwise
   */
  virtual bool is_ready() const = 0;

  /// Callback for connection events
  using ConnectionCallback = std::function<void(const std::string& client_id)>;

  /**
   * @brief Set callback for new client connections
   */
  virtual void set_on_connect(ConnectionCallback callback) = 0;

  /**
   * @brief Set callback for client disconnections
   */
  virtual void set_on_disconnect(ConnectionCallback callback) = 0;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MIDDLEWARE__MIDDLEWARE_INTERFACE_HPP_
