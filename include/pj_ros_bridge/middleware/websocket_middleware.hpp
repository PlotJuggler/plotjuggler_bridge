/*
 * Copyright (C) 2026 Davide Faconti
 *
 * This file is part of pj_ros_bridge.
 *
 * pj_ros_bridge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pj_ros_bridge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with pj_ros_bridge. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PJ_ROS_BRIDGE__MIDDLEWARE__WEBSOCKET_MIDDLEWARE_HPP_
#define PJ_ROS_BRIDGE__MIDDLEWARE__WEBSOCKET_MIDDLEWARE_HPP_

#include <ixwebsocket/IXWebSocketServer.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_ros_bridge/middleware/middleware_interface.hpp"

namespace pj_ros_bridge {

/**
 * @brief WebSocket implementation of MiddlewareInterface using IXWebSocket
 *
 * Uses a single WebSocket server port. Each connected client gets a unique
 * identity from the connection state. Text frames are used for JSON API
 * requests/responses, binary frames for aggregated message data.
 *
 * Thread safety: All public methods are thread-safe.
 */
class WebSocketMiddleware : public MiddlewareInterface {
 public:
  WebSocketMiddleware();
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
  bool send_binary(const std::string& client_identity, const std::vector<uint8_t>& data) override;
  bool is_ready() const override;
  void set_on_connect(ConnectionCallback callback) override;
  void set_on_disconnect(ConnectionCallback callback) override;

 private:
  struct IncomingRequest {
    std::string client_id;
    std::vector<uint8_t> data;
  };

  std::unique_ptr<ix::WebSocketServer> server_;

  // Thread-safe incoming message queue
  std::queue<IncomingRequest> incoming_queue_;
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;

  // Lock ordering (to prevent deadlock):
  //   state_mutex_ > clients_mutex_ > queue_mutex_

  // Connected clients: client_id -> WebSocket shared_ptr
  std::unordered_map<std::string, std::shared_ptr<ix::WebSocket>> clients_;
  mutable std::mutex clients_mutex_;

  // Callbacks
  ConnectionCallback on_connect_;
  ConnectionCallback on_disconnect_;

  mutable std::mutex state_mutex_;
  bool initialized_;

  static constexpr int kReceiveTimeoutMs = 10;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MIDDLEWARE__WEBSOCKET_MIDDLEWARE_HPP_
