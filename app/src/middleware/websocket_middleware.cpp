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

#include "pj_bridge/middleware/websocket_middleware.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <thread>

namespace pj_bridge {

WebSocketMiddleware::WebSocketMiddleware() : initialized_(false) {}

WebSocketMiddleware::~WebSocketMiddleware() {
  shutdown();
}

tl::expected<void, std::string> WebSocketMiddleware::initialize(uint16_t port) {
  std::lock_guard<std::mutex> lock(state_mutex_);

  if (initialized_) {
    return tl::unexpected<std::string>("WebSocket middleware already initialized");
  }

  server_ = std::make_shared<ix::WebSocketServer>(port, "0.0.0.0");

  server_->setOnClientMessageCallback([this](
                                          std::shared_ptr<ix::ConnectionState> connection_state,
                                          ix::WebSocket& web_socket, const ix::WebSocketMessagePtr& msg) {
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      if (!initialized_) {
        return;
      }
    }

    std::string client_id = connection_state->getId();

    if (msg->type == ix::WebSocketMessageType::Open) {
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        for (const auto& client : server_->getClients()) {
          if (client.get() == &web_socket) {
            clients_[client_id] = client;
            break;
          }
        }
      }

      ConnectionCallback cb;
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        cb = on_connect_;
      }
      if (cb) {
        cb(client_id);
      }

    } else if (msg->type == ix::WebSocketMessageType::Close) {
      ConnectionCallback cb;
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        cb = on_disconnect_;
      }
      if (cb) {
        cb(client_id);
      }

      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        clients_.erase(client_id);
      }

    } else if (msg->type == ix::WebSocketMessageType::Message) {
      if (!msg->binary) {
        IncomingRequest req;
        req.client_id = client_id;
        req.data.assign(msg->str.begin(), msg->str.end());

        {
          std::lock_guard<std::mutex> queue_lock(queue_mutex_);
          if (incoming_queue_.size() >= kMaxIncomingQueueSize) {
            spdlog::warn(
                "Incoming queue full ({} messages), dropping message from client '{}'", kMaxIncomingQueueSize,
                client_id);
            return;
          }
          incoming_queue_.push(std::move(req));
        }
        queue_cv_.notify_one();
      }
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
  std::shared_ptr<ix::WebSocketServer> server_to_stop;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!initialized_) {
      return;
    }

    initialized_ = false;
    on_connect_ = nullptr;
    on_disconnect_ = nullptr;
    server_to_stop = std::move(server_);
  }

  queue_cv_.notify_all();

  if (server_to_stop) {
    // Replace the message callback with a no-op before stopping. The original
    // callback captures `this`, so it must not fire after the WebSocketMiddleware
    // instance is destroyed (which can happen if the stop thread is detached on timeout).
    server_to_stop->setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&, const ix::WebSocketMessagePtr&) {});

    // Initiate graceful close on all connected clients before calling stop().
    spdlog::debug("[shutdown] Pre-closing {} WebSocket client(s)...", server_to_stop->getClients().size());
    for (const auto& client : server_to_stop->getClients()) {
      client->close();
    }

    // Run stop() on a dedicated thread with a timeout.
    spdlog::debug("[shutdown] Starting server stop thread...");
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread stop_thread([server_to_stop, done]() {
      server_to_stop->stop();
      done->store(true);
    });

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::seconds(kShutdownTimeoutSeconds);
    while (!done->load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    if (done->load()) {
      stop_thread.join();
      spdlog::debug("[shutdown] Server stop completed (took {}ms)", elapsed_ms);
    } else {
      // Timeout reached. Detach the thread and let it finish in the background.
      // The thread captures server_to_stop by shared_ptr, so the server
      // will be destroyed when stop() finishes and the thread exits.
      // The message callback was already cleared above, so no use-after-free.
      spdlog::warn(
          "[shutdown] Server stop timed out after {}s ({}ms elapsed), detaching shutdown thread",
          kShutdownTimeoutSeconds, elapsed_ms);
      stop_thread.detach();
    }
  }

  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    clients_.clear();
  }

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
    return false;
  }

  auto req = std::move(incoming_queue_.front());
  incoming_queue_.pop();

  data = std::move(req.data);
  client_identity = std::move(req.client_id);
  return true;
}

bool WebSocketMiddleware::send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) {
  std::shared_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_identity);
    if (it == clients_.end() || !it->second) {
      return false;
    }
    ws = it->second;
  }

  std::string text_data(data.begin(), data.end());
  auto send_info = ws->send(text_data);
  return send_info.success;
}

bool WebSocketMiddleware::send_binary(const std::string& client_identity, const std::vector<uint8_t>& data) {
  std::shared_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_identity);
    if (it == clients_.end() || !it->second) {
      return false;
    }
    ws = it->second;
  }

  std::string binary_data(reinterpret_cast<const char*>(data.data()), data.size());
  auto send_info = ws->sendBinary(binary_data);
  return send_info.success;
}

bool WebSocketMiddleware::publish_data(const std::vector<uint8_t>& data) {
  std::vector<std::shared_ptr<ix::WebSocket>> clients_copy;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (clients_.empty()) {
      return false;
    }
    clients_copy.reserve(clients_.size());
    for (const auto& [client_id, ws] : clients_) {
      if (ws) {
        clients_copy.push_back(ws);
      }
    }
  }

  std::string binary_data(reinterpret_cast<const char*>(data.data()), data.size());
  bool any_sent = false;

  for (const auto& ws : clients_copy) {
    auto send_info = ws->sendBinary(binary_data);
    if (send_info.success) {
      any_sent = true;
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

}  // namespace pj_bridge
