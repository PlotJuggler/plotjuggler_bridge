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
#include <memory>
#include <string>
#include <vector>

namespace pj_ros_bridge
{

/**
 * @brief Abstract interface for middleware implementations
 *
 * This interface provides an abstraction layer over the networking middleware,
 * allowing for future replacement of ZeroMQ with alternative implementations.
 *
 * Thread safety: Implementations should be thread-safe for concurrent calls.
 */
class MiddlewareInterface
{
public:
  virtual ~MiddlewareInterface() = default;

  /**
   * @brief Initialize the middleware
   *
   * @param req_port Port for REQ-REP pattern (client API requests)
   * @param pub_port Port for PUB-SUB pattern (data streaming)
   * @return true if initialization successful, false otherwise
   */
  virtual bool initialize(uint16_t req_port, uint16_t pub_port) = 0;

  /**
   * @brief Shutdown the middleware and cleanup resources
   */
  virtual void shutdown() = 0;

  /**
   * @brief Receive a request from a client (blocking)
   *
   * @param data Output buffer for received data
   * @param client_identity Output parameter for client identifier
   * @return true if request received successfully, false on error or timeout
   */
  virtual bool receive_request(std::vector<uint8_t>& data, std::string& client_identity) = 0;

  /**
   * @brief Send a reply to the last received request
   *
   * @param data Data to send as reply
   * @return true if reply sent successfully, false otherwise
   */
  virtual bool send_reply(const std::vector<uint8_t>& data) = 0;

  /**
   * @brief Publish data to all subscribers
   *
   * @param data Data to publish
   * @return true if data published successfully, false otherwise
   */
  virtual bool publish_data(const std::vector<uint8_t>& data) = 0;

  /**
   * @brief Get the client identity from the last received request
   *
   * @return Client identifier string
   */
  virtual std::string get_client_identity() const = 0;

  /**
   * @brief Check if middleware is initialized and ready
   *
   * @return true if ready, false otherwise
   */
  virtual bool is_ready() const = 0;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MIDDLEWARE__MIDDLEWARE_INTERFACE_HPP_
