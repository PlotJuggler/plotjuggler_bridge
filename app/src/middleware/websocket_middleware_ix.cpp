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

#include "pj_bridge/middleware/websocket_middleware.hpp"

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

  /// Called on the connection's thread with the accepted socket, before the
  /// WebSocket upgrade. Set before listen().
  std::function<void(const std::string&, int)> on_connection_start;
  /// Called on the connection's thread once IXWebSocket no longer uses its
  /// WebSocket. Set before listen().
  std::function<void(const std::string&)> on_connection_end;

 private:
  void handleConnection(
      std::unique_ptr<ix::Socket> socket, std::shared_ptr<ix::ConnectionState> connection_state) override {
    if (on_connection_start) {
      on_connection_start(connection_state->getId(), SocketFdAccess::fd(*socket));
    }
#ifdef TCP_NOTSENT_LOWAT
    const int max_unsent = WebSocketMiddleware::kMaxUnsentBytes;
    if (setsockopt(SocketFdAccess::fd(*socket), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &max_unsent, sizeof(max_unsent)) != 0) {
      spdlog::warn("Could not set TCP_NOTSENT_LOWAT; a slow client may see extra latency");
    }
#endif
    handleUpgrade(std::move(socket), connection_state);
    if (on_connection_end) {
      on_connection_end(connection_state->getId());
    }
    connection_state->setTerminated();
  }
};

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

  auto server = std::make_shared<LowLatencyWebSocketServer>(port, "0.0.0.0");
  auto gate = std::make_shared<CallbackGate>();
  gate_ = gate;
  server->on_connection_start = [this, gate](const std::string& client_id, int fd) {
    std::shared_lock<std::shared_mutex> gate_lock(gate->mutex);
    if (gate->open) {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      connection_fds_[client_id] = fd;
    }
  };
  server->on_connection_end = [this, gate](const std::string& client_id) {
    std::shared_lock<std::shared_mutex> gate_lock(gate->mutex);
    if (gate->open) {
      mark_connection_ended(client_id);
    }
  };
  server_ = server;
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

  server_->setOnClientMessageCallback([this, gate](
                                          std::shared_ptr<ix::ConnectionState> connection_state,
                                          ix::WebSocket& web_socket, const ix::WebSocketMessagePtr& msg) {
    std::shared_lock<std::shared_mutex> gate_lock(gate->mutex);
    if (!gate->open) {
      return;
    }
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
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        if (!accepting_clients_) {
          return;  // shutdown() already collected the clients to join
        }
        Client client;
        client.id = client_id;
        client.ws = ws;
        client.queue = std::make_shared<OutboundQueue>(client_backlog_size_);
        client.sender_done = std::make_shared<std::atomic<bool>>(false);
        // The client's sender thread: the only caller of IXWebSocket's
        // blocking send for this client.
        // A raw pointer: the Client entry keeps the WebSocket alive until
        // this thread is joined.
        client.sender = std::thread([ws = client.ws.get(), queue = client.queue, done = client.sender_done] {
          while (auto item = queue->pop()) {
            if (!ws->send(item->data, item->binary).success) {
              break;  // the connection is closing; its Close event retires this client
            }
          }
          queue->close();
          *done = true;
        });
        clients_[client_id] = std::move(client);
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
      // Close may run on the client's sender thread (IXWebSocket reports a
      // failed write synchronously), so nothing here may join that thread.
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        auto it = clients_.find(client_id);
        if (it != clients_.end()) {
          retire_client(it->second);
          clients_.erase(it);
        }
        // Requests the client sent before leaving would otherwise re-create
        // its session (once per request) when the bridge gets to them. Under
        // clients_mutex_, so the Message branch cannot add one afterwards.
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
          // Lock order clients_mutex_ -> queue_mutex_, as in the Close branch:
          // a request is queued only while its client is still connected.
          std::lock_guard<std::mutex> clients_lock(clients_mutex_);
          if (clients_.find(client_id) == clients_.end()) {
            return;
          }
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

  // Close the callback gate first: no IXWebSocket callback runs into this
  // object (or, through on_connect_/on_disconnect_, its owner) after this,
  // including one that was already running. Callbacks take no lock that is
  // held here, and shutdown() is never called from a callback.
  std::shared_ptr<CallbackGate> gate;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    gate = gate_;
  }
  if (gate) {
    std::unique_lock<std::shared_mutex> gate_lock(gate->mutex);
    gate->open = false;
  }

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
    connection_fds_.clear();
  }

  if (server_to_stop) {
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
  client.queue->close();  // first, so no producer adds to the counters read below
  dropped_from_departed_ += client.queue->dropped_total();
  shed_from_departed_ += client.queue->shed_total();
  client.queue.reset();  // the sender keeps its own reference while it runs
  retired_.push_back(std::move(client));
}

void WebSocketMiddleware::mark_connection_ended(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  connection_fds_.erase(client_id);
  auto mark = [&](Client& client) {
    if (client.id == client_id) {
      client.connection_ended = true;
    }
  };
  for (auto& client : retired_) {
    mark(client);
  }
  if (auto it = clients_.find(client_id); it != clients_.end()) {
    mark(it->second);
  }
}

void WebSocketMiddleware::reap_finished_senders() {
  std::vector<Client> finished;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (retired_.empty()) {
      return;
    }
    finished = take_retired(false);
  }
  join_senders(finished);
}

std::vector<WebSocketMiddleware::Client> WebSocketMiddleware::take_retired(bool all) {
  auto keep = std::partition(retired_.begin(), retired_.end(), [all](const Client& client) {
    return !all && !(*client.sender_done && client.connection_ended);
  });
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

  // The bridge polls here continually, from its own thread: a good place to
  // join the sender threads of clients that left.
  reap_finished_senders();

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

WebSocketMiddleware::Client WebSocketMiddleware::find_client(const std::string& client_identity) const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  auto it = clients_.find(client_identity);
  if (it == clients_.end()) {
    return {};
  }
  Client copy;
  copy.ws = it->second.ws;
  copy.queue = it->second.queue;
  return copy;
}

bool WebSocketMiddleware::close_if_not_reading(const Client& client, const std::string& client_identity) {
  if (client.queue->reply_budget_exceeded()) {
    spdlog::warn("Client '{}' is not reading its replies; closing the connection", client_identity);
    client.ws->close();
  }
  return false;
}

bool WebSocketMiddleware::send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) {
  auto client = find_client(client_identity);
  if (!client.queue) {
    return false;
  }
  return client.queue->push_text(std::string(reinterpret_cast<const char*>(data.data()), data.size())) ||
         close_if_not_reading(client, client_identity);
}

bool WebSocketMiddleware::send_binary(
    const std::string& client_identity, const std::vector<uint8_t>& data, FramePriority priority,
    const std::string& key) {
  auto client = find_client(client_identity);
  if (!client.queue) {
    return false;
  }
  std::string frame(reinterpret_cast<const char*>(data.data()), data.size());
  switch (priority) {
    case FramePriority::kHeavy:
      return client.queue->push_heavy(key, std::move(frame));
    case FramePriority::kLatched:
      return client.queue->push_kept(std::move(frame)) || close_if_not_reading(client, client_identity);
    case FramePriority::kNormal:
      break;
  }
  return client.queue->push_normal(std::move(frame));
}

bool WebSocketMiddleware::heard_from_client_within(
    const std::string& client_identity, std::chrono::milliseconds window) const {
#ifdef __linux__
  std::lock_guard<std::mutex> lock(clients_mutex_);
  auto fd = connection_fds_.find(client_identity);
  if (fd == connection_fds_.end() || clients_.find(client_identity) == clients_.end()) {
    return false;
  }
  // The kernel's time since it last received data from the peer: it counts a
  // heartbeat that arrived even if IXWebSocket has not read it yet.
  tcp_info info{};
  socklen_t len = sizeof(info);
  if (getsockopt(fd->second, IPPROTO_TCP, TCP_INFO, &info, &len) != 0) {
    return false;
  }
  return std::chrono::milliseconds(info.tcpi_last_data_recv) <= window;
#else
  (void)client_identity;
  (void)window;
  return false;
#endif
}

void WebSocketMiddleware::drop_pending(const std::string& client_identity) {
  // The client's logical session was destroyed server-side (e.g. heartbeat
  // timeout) while its socket may still be open: discard its queued frames so
  // stale data can't flush into a future session on the same connection.
  if (auto queue = find_client(client_identity).queue) {
    queue->drop_frames();
  }
}

uint64_t WebSocketMiddleware::sum_counter(uint64_t departed, uint64_t (OutboundQueue::*counter)() const) const {
  uint64_t total = departed;
  for (const auto& [client_id, client] : clients_) {
    total += (client.queue.get()->*counter)();
  }
  return total;
}

uint64_t WebSocketMiddleware::dropped_frame_count() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  return sum_counter(dropped_from_departed_, &OutboundQueue::dropped_total);
}

uint64_t WebSocketMiddleware::heavy_shed_count() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  return sum_counter(shed_from_departed_, &OutboundQueue::shed_total);
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
