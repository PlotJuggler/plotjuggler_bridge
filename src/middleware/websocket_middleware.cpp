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

#include "pj_ros_bridge/middleware/websocket_middleware.hpp"

namespace pj_ros_bridge {

WebSocketMiddleware::WebSocketMiddleware() : initialized_(false) {}

WebSocketMiddleware::~WebSocketMiddleware() {
  shutdown();
}

tl::expected<void, std::string> WebSocketMiddleware::initialize(uint16_t port) {
  std::lock_guard<std::mutex> lock(state_mutex_);

  if (initialized_) {
    return tl::unexpected<std::string>("WebSocket middleware already initialized");
  }

  server_ = std::make_unique<ix::WebSocketServer>(port, "0.0.0.0");

  server_->setOnClientMessageCallback([this](
                                          std::shared_ptr<ix::ConnectionState> connection_state,
                                          ix::WebSocket& web_socket, const ix::WebSocketMessagePtr& msg) {
    // Guard against callbacks arriving after shutdown() has moved server_ out.
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      if (!initialized_) {
        return;
      }
    }

    std::string client_id = connection_state->getId();

    if (msg->type == ix::WebSocketMessageType::Open) {
      // Store client connection
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        // Get a shared_ptr to this WebSocket from the server's client set
        for (const auto& client : server_->getClients()) {
          if (client.get() == &web_socket) {
            clients_[client_id] = client;
            break;
          }
        }
      }

      // Notify connect callback
      ConnectionCallback cb;
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        cb = on_connect_;
      }
      if (cb) {
        cb(client_id);
      }

    } else if (msg->type == ix::WebSocketMessageType::Close) {
      // Notify disconnect callback
      ConnectionCallback cb;
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        cb = on_disconnect_;
      }
      if (cb) {
        cb(client_id);
      }

      // Remove client connection
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        clients_.erase(client_id);
      }

    } else if (msg->type == ix::WebSocketMessageType::Message) {
      if (!msg->binary) {
        // Text message = API request, push to queue
        IncomingRequest req;
        req.client_id = client_id;
        req.data.assign(msg->str.begin(), msg->str.end());

        {
          std::lock_guard<std::mutex> queue_lock(queue_mutex_);
          incoming_queue_.push(std::move(req));
        }
        queue_cv_.notify_one();
      }
      // Binary messages from clients are ignored (server-to-client only)
    }
  });

  auto listen_result = server_->listen();
  if (!listen_result.first) {
    server_.reset();
    return tl::unexpected<std::string>(
        "Failed to listen on port " + std::to_string(port) + ": " + listen_result.second);
  }

  server_->start();
  initialized_ = true;

  return {};
}

void WebSocketMiddleware::shutdown() {
  std::unique_ptr<ix::WebSocketServer> server_to_stop;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!initialized_) {
      return;
    }

    initialized_ = false;

    // Clear callbacks before stopping so that Close events fired during
    // stop() don't invoke into potentially destroyed objects (BridgeServer).
    on_connect_ = nullptr;
    on_disconnect_ = nullptr;

    // Move server out — will be stopped outside the lock to avoid deadlock
    // with IO threads that acquire state_mutex_ in their Close callbacks.
    server_to_stop = std::move(server_);
  }

  // Wake any thread blocked in receive_request()
  queue_cv_.notify_all();

  // Stop the server WITHOUT holding state_mutex_.
  // IX IO threads fire Close callbacks that acquire state_mutex_;
  // releasing it first prevents the deadlock.
  if (server_to_stop) {
    server_to_stop->stop();
    server_to_stop.reset();
  }

  // Clear client map
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    clients_.clear();
  }

  // Clear incoming queue
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    std::queue<IncomingRequest> empty;
    std::swap(incoming_queue_, empty);
  }
}

bool WebSocketMiddleware::receive_request(std::vector<uint8_t>& data, std::string& client_identity) {
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (!initialized_) {
      return false;
    }
  }

  std::unique_lock<std::mutex> lock(queue_mutex_);

  if (!queue_cv_.wait_for(
          lock, std::chrono::milliseconds(kReceiveTimeoutMs), [this]() { return !incoming_queue_.empty(); })) {
    return false;  // Timeout
  }

  auto req = std::move(incoming_queue_.front());
  incoming_queue_.pop();

  data = std::move(req.data);
  client_identity = std::move(req.client_id);
  return true;
}

bool WebSocketMiddleware::send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) {
  std::lock_guard<std::mutex> lock(clients_mutex_);

  auto it = clients_.find(client_identity);
  if (it == clients_.end() || !it->second) {
    return false;  // Client not found or disconnected
  }

  std::string text_data(data.begin(), data.end());
  auto send_info = it->second->send(text_data);
  return send_info.success;
}

bool WebSocketMiddleware::send_binary(const std::string& client_identity, const std::vector<uint8_t>& data) {
  std::lock_guard<std::mutex> lock(clients_mutex_);

  auto it = clients_.find(client_identity);
  if (it == clients_.end() || !it->second) {
    return false;
  }

  std::string binary_data(reinterpret_cast<const char*>(data.data()), data.size());
  auto send_info = it->second->sendBinary(binary_data);
  return send_info.success;
}

bool WebSocketMiddleware::publish_data(const std::vector<uint8_t>& data) {
  std::lock_guard<std::mutex> lock(clients_mutex_);

  if (clients_.empty()) {
    return false;
  }

  std::string binary_data(reinterpret_cast<const char*>(data.data()), data.size());
  bool any_sent = false;

  for (auto& [client_id, ws] : clients_) {
    if (ws) {
      auto send_info = ws->sendBinary(binary_data);
      if (send_info.success) {
        any_sent = true;
      }
    }
  }

  return any_sent;
}

bool WebSocketMiddleware::is_ready() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return initialized_;
}

void WebSocketMiddleware::set_on_connect(ConnectionCallback callback) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  on_connect_ = std::move(callback);
}

void WebSocketMiddleware::set_on_disconnect(ConnectionCallback callback) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  on_disconnect_ = std::move(callback);
}

}  // namespace pj_ros_bridge
