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

#ifndef PJ_ROS_BRIDGE__MIDDLEWARE__ZMQ_MIDDLEWARE_HPP_
#define PJ_ROS_BRIDGE__MIDDLEWARE__ZMQ_MIDDLEWARE_HPP_

#include <memory>
#include <mutex>
#include <zmq.hpp>

#include "pj_ros_bridge/middleware/middleware_interface.hpp"

namespace pj_ros_bridge {

/**
 * @brief ZeroMQ implementation of MiddlewareInterface
 *
 * Implements the middleware interface using ZeroMQ with:
 * - REP socket for request-reply pattern (client API)
 * - PUB socket for publish-subscribe pattern (data streaming)
 *
 * Thread safety: All public methods are thread-safe
 */
class ZmqMiddleware : public MiddlewareInterface {
 public:
  ZmqMiddleware();
  ~ZmqMiddleware() override;

  // Disable copy and move
  ZmqMiddleware(const ZmqMiddleware&) = delete;
  ZmqMiddleware& operator=(const ZmqMiddleware&) = delete;
  ZmqMiddleware(ZmqMiddleware&&) = delete;
  ZmqMiddleware& operator=(ZmqMiddleware&&) = delete;

  bool initialize(uint16_t req_port, uint16_t pub_port) override;
  void shutdown() override;
  bool receive_request(std::vector<uint8_t>& data, std::string& client_identity) override;
  bool send_reply(const std::vector<uint8_t>& data) override;
  bool publish_data(const std::vector<uint8_t>& data) override;
  std::string get_client_identity() const override;
  bool is_ready() const override;

 private:
  std::unique_ptr<zmq::context_t> context_;
  std::unique_ptr<zmq::socket_t> rep_socket_;
  std::unique_ptr<zmq::socket_t> pub_socket_;

  mutable std::mutex mutex_;
  bool initialized_;
  std::string last_client_identity_;
  uint16_t req_port_;
  uint16_t pub_port_;

  static constexpr int kZmqReceiveTimeout = 100;  // milliseconds
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MIDDLEWARE__ZMQ_MIDDLEWARE_HPP_
