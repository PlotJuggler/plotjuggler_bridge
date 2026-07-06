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
#include <optional>
#include <thread>

namespace pj_bridge {

WebSocketMiddleware::WebSocketMiddleware(size_t client_backlog_size)
    : client_backlog_size_(client_backlog_size), initialized_(false) {}

WebSocketMiddleware::~WebSocketMiddleware() {
  shutdown();

  // Join any stop thread that outlived its shutdown timeout. Blocking here
  // is deliberate: letting it run detached past main() races static
  // destruction.
  std::thread pending;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    pending = std::move(pending_stop_thread_);
  }
  if (pending.joinable()) {
    pending.join();
  }
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
    // Copy the server pointer under the same lock as the initialized_ check:
    // shutdown() concurrently moves server_ away, so touching the member
    // after releasing the lock would dereference a null shared_ptr.
    std::shared_ptr<ix::WebSocketServer> server;
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      if (!initialized_ || !server_) {
        return;
      }
      server = server_;
    }

    std::string client_id = connection_state->getId();

    if (msg->type == ix::WebSocketMessageType::Open) {
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        for (const auto& client : server->getClients()) {
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
        auto pending_it = pending_frames_.find(client_id);
        if (pending_it != pending_frames_.end()) {
          dropped_from_disconnected_ += pending_it->second.dropped_total();
          pending_frames_.erase(pending_it);
        }
        last_drop_warn_.erase(client_id);
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

  if (server_to_stop) {
    // Replace the message callback with a no-op before stopping. The original
    // callback captures `this`, so it must not fire while the stop thread
    // outlives this shutdown call (it is joined in the destructor on timeout).
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
      // Timeout reached. Keep the thread joinable and hand it to the
      // destructor: a detached thread still running IXWebSocket teardown
      // during static destruction (after main() returns) is undefined
      // behavior. shutdown() itself stays bounded; only final destruction
      // waits for a hung stop() to complete.
      spdlog::warn(
          "[shutdown] Server stop timed out after {}s ({}ms elapsed), deferring join to destructor",
          kShutdownTimeoutSeconds, elapsed_ms);
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (pending_stop_thread_.joinable()) {
        pending_stop_thread_.join();
      }
      pending_stop_thread_ = std::move(stop_thread);
    }
  }

  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    clients_.clear();
    for (const auto& entry : pending_frames_) {
      dropped_from_disconnected_ += entry.second.dropped_total();
    }
    pending_frames_.clear();
    last_drop_warn_.clear();
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

  // Truly non-blocking: callers poll from timers/event loops, so waiting on
  // the condition variable here would park the caller's thread (up to 10 ms
  // per empty poll) and delay everything else it drives.
  std::lock_guard<std::mutex> lock(queue_mutex_);

  if (incoming_queue_.empty()) {
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
  // Lossy-send policy adapted from foxglove_bridge (MIT License, Copyright
  // (c) Foxglove Technologies Inc): a slow client never blocks the publisher
  // and is never disconnected for falling behind. Instead, once its socket's
  // outgoing buffer crosses kSocketBufferHighWatermark, new frames are queued
  // per-client (dropping the oldest queued frame on overflow) and flushed
  // once the buffer drains below the watermark again.
  //
  // IMPORTANT: clients_mutex_ is only ever held to look up state (the client
  // handle, pending_frames_, last_drop_warn_) — never while calling into
  // ix::WebSocket (bufferedAmount()/sendBinary()). Those calls take
  // IXWebSocket's own internal locks, and the Open-connection handler above
  // takes clients_mutex_ from an IXWebSocket-owned thread that already holds
  // some of those internal locks; holding clients_mutex_ across a socket call
  // here would form a lock-order cycle with that path (observed as a TSAN
  // lock-order-inversion during development).
  std::shared_ptr<ix::WebSocket> ws;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_identity);
    if (it == clients_.end() || !it->second) {
      return false;
    }
    ws = it->second;
  }

  if (ws->bufferedAmount() < kSocketBufferHighWatermark) {
    // Flush any backlog first, in FIFO order, bounded to the number of
    // frames present when this call started so a fast producer can't spin
    // forever. Each frame is popped under the lock and sent outside it.
    size_t to_flush = 0;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      auto pending_it = pending_frames_.find(client_identity);
      if (pending_it != pending_frames_.end()) {
        to_flush = pending_it->second.size();
      }
    }

    for (size_t i = 0; i < to_flush; ++i) {
      std::optional<std::vector<uint8_t>> frame;
      {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto pending_it = pending_frames_.find(client_identity);
        if (pending_it == pending_frames_.end()) {
          break;
        }
        frame = pending_it->second.pop_front();
      }
      if (!frame) {
        break;
      }
      std::string frame_data(reinterpret_cast<const char*>(frame->data()), frame->size());
      auto send_info = ws->sendBinary(frame_data);
      if (!send_info.success) {
        return false;
      }
    }

    std::string binary_data(reinterpret_cast<const char*>(data.data()), data.size());
    auto send_info = ws->sendBinary(binary_data);
    return send_info.success;
  }

  // Socket buffer is over the high watermark: queue the frame instead of
  // sending it now.
  size_t dropped;
  uint64_t dropped_total_now;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto [pending_it, inserted] = pending_frames_.try_emplace(client_identity, BoundedFrameQueue(client_backlog_size_));
    (void)inserted;
    dropped = pending_it->second.push(data);
    dropped_total_now = pending_it->second.dropped_total();
  }

  if (dropped > 0) {
    auto now = std::chrono::steady_clock::now();
    bool should_warn;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      auto warn_it = last_drop_warn_.find(client_identity);
      should_warn =
          warn_it == last_drop_warn_.end() || now - warn_it->second >= std::chrono::seconds(kDropWarnIntervalSeconds);
      if (should_warn) {
        last_drop_warn_[client_identity] = now;
      }
    }
    if (should_warn) {
      spdlog::warn(
          "Slow client '{}': dropping oldest queued frame(s) ({} dropped total)", client_identity, dropped_total_now);
    }
  }

  // The frame was accepted for later delivery, not a failure.
  return true;
}

uint64_t WebSocketMiddleware::dropped_frame_count() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  uint64_t total = dropped_from_disconnected_;
  for (const auto& entry : pending_frames_) {
    total += entry.second.dropped_total();
  }
  return total;
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
