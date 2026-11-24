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

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXSocket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace pj_bridge {

namespace {

// IXWebSocket has no accessor for a connection's file descriptor; this reads
// ix::Socket's protected member through a derived-class member pointer.
struct SocketFdAccess : ix::Socket {
  static int fd(const ix::Socket& socket) {
    return (socket.*(&SocketFdAccess::_sockfd)).load();
  }
};

// WebSocketServer that sets TCP_NOTSENT_LOWAT on each accepted socket before
// the WebSocket upgrade. The body otherwise repeats
// ix::WebSocketServer::handleConnection (11.x and 12.x): handleUpgrade, then
// mark the connection terminated.
class LowLatencyWebSocketServer : public ix::WebSocketServer {
 public:
  using ix::WebSocketServer::WebSocketServer;

 private:
  void handleConnection(
      std::unique_ptr<ix::Socket> socket, std::shared_ptr<ix::ConnectionState> connection_state) override {
#ifdef TCP_NOTSENT_LOWAT
    const int max_unsent = WebSocketMiddleware::kMaxUnsentBytes;
    if (setsockopt(SocketFdAccess::fd(*socket), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &max_unsent, sizeof(max_unsent)) != 0) {
      spdlog::warn("Could not set TCP_NOTSENT_LOWAT; a slow client may see extra latency");
    }
#endif
    handleUpgrade(std::move(socket), connection_state);
    connection_state->setTerminated();
  }
};

// Body of a client's sender thread: the only place that calls IXWebSocket's
// blocking send for this client.
void run_sender(
    const std::shared_ptr<ix::WebSocket>& ws, const std::shared_ptr<OutboundQueue>& queue,
    const std::shared_ptr<std::atomic<bool>>& done) {
  while (auto item = queue->pop()) {
    if (!ws->send(item->data, item->binary).success) {
      break;  // the connection is closing; its Close event retires this client
    }
  }
  queue->close();
  done->store(true);
}

}  // namespace

WebSocketMiddleware::WebSocketMiddleware(size_t client_backlog_size, std::optional<TlsConfig> tls)
    : client_backlog_size_(client_backlog_size), tls_(std::move(tls)), initialized_(false) {}

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

  if (tls_.has_value()) {
#ifndef IXWEBSOCKET_USE_TLS
    return tl::unexpected<std::string>("TLS requested but IXWebSocket was built without TLS support");
#else
    std::ifstream cert_stream(tls_->certfile);
    if (!cert_stream.good()) {
      return tl::unexpected<std::string>("TLS certificate file not found or not readable: " + tls_->certfile);
    }
    std::ifstream key_stream(tls_->keyfile);
    if (!key_stream.good()) {
      return tl::unexpected<std::string>("TLS key file not found or not readable: " + tls_->keyfile);
    }
#endif
  }

  server_ = std::make_shared<LowLatencyWebSocketServer>(port, "0.0.0.0");
  // Binary frames are already zstd-compressed; deflating them again is wasted work.
  server_->disablePerMessageDeflate();

#ifdef IXWEBSOCKET_USE_TLS
  if (tls_.has_value()) {
    ix::SocketTLSOptions tls_options;
    tls_options.tls = true;
    tls_options.certFile = tls_->certfile;
    tls_options.keyFile = tls_->keyfile;
    tls_options.caFile = "NONE";
    server_->setTLSOptions(tls_options);
  }
#endif

  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    accepting_clients_ = true;
  }

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
      std::shared_ptr<ix::WebSocket> ws;
      for (const auto& client : server->getClients()) {
        if (client.get() == &web_socket) {
          ws = client;
          break;
        }
      }
      if (!ws) {
        return;
      }
      std::vector<Client> finished;  // departed clients' finished senders, joined below
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        if (!accepting_clients_) {
          return;  // shutdown() already collected the clients to join
        }
        finished = take_retired(false);
        Client client;
        client.ws = ws;
        client.queue = std::make_shared<OutboundQueue>(client_backlog_size_);
        client.sender_done = std::make_shared<std::atomic<bool>>(false);
        client.sender = std::thread(run_sender, client.ws, client.queue, client.sender_done);
        clients_[client_id] = std::move(client);
      }
      join_senders(finished);

      ConnectionCallback cb;
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        cb = on_connect_;
      }
      if (cb) {
        cb(client_id);
      }

    } else if (msg->type == ix::WebSocketMessageType::Close) {
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
          retire_client(it->second);
          clients_.erase(it);
        }
      }
      // Requests the client sent before leaving would otherwise re-create its
      // session (once per request) when the bridge gets to them.
      {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        incoming_queue_.erase(
            std::remove_if(
                incoming_queue_.begin(), incoming_queue_.end(),
                [&](const IncomingRequest& req) { return req.client_id == client_id; }),
            incoming_queue_.end());
      }

      ConnectionCallback cb;
      {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        cb = on_disconnect_;
      }
      if (cb) {
        cb(client_id);
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
          incoming_queue_.push_back(std::move(req));
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

  // Stop every sender before closing the sockets, so none starts a new send.
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    accepting_clients_ = false;
    for (auto& [client_id, client] : clients_) {
      retire_client(client);
    }
    clients_.clear();
  }

  if (server_to_stop) {
    // Replace the message callback with a no-op before stopping. The original
    // callback captures `this`, so it must not fire while the stop thread
    // outlives this shutdown call (it is joined in the destructor on timeout).
    server_to_stop->setOnClientMessageCallback(
        [](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&, const ix::WebSocketMessagePtr&) {});

    // Initiate graceful close on all connected clients before calling stop().
    // This also cancels a sender thread blocked in a send to a stalled client.
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

  // Sockets are closed now, so a sender blocked in a send has returned.
  std::vector<Client> senders;
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    senders = take_retired(true);
  }
  join_senders(senders);

  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    incoming_queue_.clear();
  }
}

void WebSocketMiddleware::retire_client(Client& client) {
  dropped_from_departed_ += client.queue->dropped_total();
  shed_from_departed_ += client.queue->shed_total();
  client.queue->close();
  retired_.push_back(std::move(client));
}

std::vector<WebSocketMiddleware::Client> WebSocketMiddleware::take_retired(bool all) {
  auto keep = std::partition(
      retired_.begin(), retired_.end(), [all](const Client& client) { return !all && !client.sender_done->load(); });
  std::vector<Client> taken(std::make_move_iterator(keep), std::make_move_iterator(retired_.end()));
  retired_.erase(keep, retired_.end());
  return taken;
}

void WebSocketMiddleware::join_senders(std::vector<Client>& clients) {
  for (auto& client : clients) {
    if (client.sender.joinable()) {
      client.sender.join();
    }
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
  incoming_queue_.pop_front();

  data = std::move(req.data);
  client_identity = std::move(req.client_id);
  return true;
}

std::shared_ptr<OutboundQueue> WebSocketMiddleware::find_queue(const std::string& client_identity) const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  auto it = clients_.find(client_identity);
  return it == clients_.end() ? nullptr : it->second.queue;
}

bool WebSocketMiddleware::send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) {
  auto queue = find_queue(client_identity);
  return queue && queue->push_text(std::string(data.begin(), data.end()));
}

bool WebSocketMiddleware::send_binary(
    const std::string& client_identity, const std::vector<uint8_t>& data, FramePriority priority,
    const std::string& key) {
  auto queue = find_queue(client_identity);
  if (!queue) {
    return false;
  }
  std::string frame(reinterpret_cast<const char*>(data.data()), data.size());
  return priority == FramePriority::kHeavy ? queue->push_heavy(key, std::move(frame))
                                           : queue->push_normal(std::move(frame));
}

void WebSocketMiddleware::drop_pending(const std::string& client_identity) {
  // The client's logical session was destroyed server-side (e.g. heartbeat
  // timeout) while its socket may still be open: discard its queued frames so
  // stale data can't flush into a future session on the same connection.
  if (auto queue = find_queue(client_identity)) {
    queue->drop_frames();
  }
}

uint64_t WebSocketMiddleware::dropped_frame_count() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  uint64_t total = dropped_from_departed_;
  for (const auto& [client_id, client] : clients_) {
    total += client.queue->dropped_total();
  }
  return total;
}

uint64_t WebSocketMiddleware::heavy_shed_count() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  uint64_t total = shed_from_departed_;
  for (const auto& [client_id, client] : clients_) {
    total += client.queue->shed_total();
  }
  return total;
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
