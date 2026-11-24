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

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include <algorithm>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <thread>
#include <type_traits>

#ifdef PJ_BRIDGE_TLS
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif

namespace pj_bridge {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

#ifdef PJ_BRIDGE_TLS
using TlsStream = beast::ssl_stream<beast::tcp_stream>;
#endif

struct WebSocketMiddleware::Io {
#ifdef PJ_BRIDGE_TLS
  std::optional<asio::ssl::context> ssl;  // declared first: outlives every TLS stream
#endif
  asio::io_context ioc{1};
  tcp::acceptor acceptor{ioc};
  std::promise<void> done;  // set when ioc.run() returns
  std::thread thread;
};

/// One connection. All members are used on the io thread only, except the
/// immutable ones and `queue` (thread-safe), which the bridge's threads fill.
class WebSocketMiddleware::Session : public std::enable_shared_from_this<Session> {
 public:
  Session(WebSocketMiddleware& mw, std::string client_id, size_t backlog, int socket_fd, asio::any_io_executor ex)
      : id(std::move(client_id)), queue(backlog), fd(socket_fd), executor(std::move(ex)), mw_(mw) {}
  virtual ~Session() = default;
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  virtual void start() = 0;  ///< TLS handshake (if any), then the WebSocket handshake
  virtual void kick() = 0;   ///< start sending the next queued item, unless a write is in progress
  virtual void close() = 0;  ///< send a Close frame, then drop the connection
  virtual void abort() = 0;  ///< drop the connection now

  const std::string id;
  OutboundQueue queue;
  const int fd;
  const asio::any_io_executor executor;

 protected:
  WebSocketMiddleware& mw_;
};

template <class Stream>
class WebSocketMiddleware::SessionImpl final : public WebSocketMiddleware::Session {
 public:
  template <class... Args>
  SessionImpl(WebSocketMiddleware& mw, std::string client_id, size_t backlog, tcp::socket&& socket, Args&... args)
      : Session(mw, std::move(client_id), backlog, socket.native_handle(), socket.get_executor()),
        ws_(std::move(socket), args...) {}

  void start() override {
#ifdef PJ_BRIDGE_TLS
    if constexpr (std::is_same_v<Stream, TlsStream>) {
      beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));
      ws_.next_layer().async_handshake(asio::ssl::stream_base::server, [self = shared()](beast::error_code ec) {
        if (ec) {
          return self->fail(ec);
        }
        self->accept();
      });
      return;
    }
#endif
    accept();
  }

  void kick() override {
    if (!open_ || closed_ || writing_) {
      return;
    }
    auto item = queue.try_pop();
    if (!item) {
      return;
    }
    writing_ = true;
    out_ = std::move(item->data);
    ws_.binary(item->binary);
    ws_.async_write(asio::buffer(out_), [self = shared()](beast::error_code ec, std::size_t) {
      self->writing_ = false;
      std::string().swap(self->out_);  // release a large frame now, not at the next write
      if (ec) {
        return self->fail(ec);
      }
      self->kick();
    });
  }

  void close() override {
    if (closed_) {
      return;
    }
    if (!open_) {
      return abort();
    }
    // Waits behind a write in progress; shutdown() bounds the wait.
    ws_.async_close(websocket::close_code::going_away, [self = shared()](beast::error_code) { self->fail({}); });
  }

  void abort() override {
    fail({});
  }

 private:
  std::shared_ptr<SessionImpl> shared() {
    return std::static_pointer_cast<SessionImpl>(shared_from_this());
  }

  void accept() {
    beast::get_lowest_layer(ws_).expires_never();  // the WebSocket stream has its own timeouts
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::server);
    // No idle timeout: dead clients are found by BridgeServer's heartbeat timeout,
    // and a client on a slow link may have nothing to send for a long time.
    timeout.idle_timeout = websocket::stream_base::none();
    ws_.set_option(timeout);
    // Default options leave permessage-deflate disabled, so the server declines
    // it: binary frames are already zstd-compressed.
    ws_.set_option(websocket::permessage_deflate{});
    ws_.auto_fragment(false);  // one WebSocket frame per message
    ws_.async_accept([self = shared()](beast::error_code ec) {
      if (ec) {
        return self->fail(ec);
      }
      self->open_ = true;
      self->mw_.on_open(self);
      self->read();
      self->kick();
    });
  }

  void read() {
    ws_.async_read(buffer_, [self = shared()](beast::error_code ec, std::size_t) {
      if (ec) {
        return self->fail(ec);
      }
      if (self->ws_.got_text()) {
        self->mw_.on_message(self->id, beast::buffers_to_string(self->buffer_.data()));
      }
      self->buffer_.consume(self->buffer_.size());
      self->read();
    });
  }

  /// The connection is over: report it once, then close the socket, which
  /// completes every pending operation (each ends up here again).
  void fail(beast::error_code ec) {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (ec && ec != websocket::error::closed && ec != asio::error::operation_aborted) {
      spdlog::debug("WebSocket client '{}': {}", id, ec.message());
    }
    if (open_) {
      mw_.on_closed(id);
    }
    beast::get_lowest_layer(ws_).close();
  }

  websocket::stream<Stream> ws_;
  beast::flat_buffer buffer_;
  std::string out_;  // the item being written
  bool open_ = false;
  bool closed_ = false;
  bool writing_ = false;
};

WebSocketMiddleware::WebSocketMiddleware(size_t client_backlog_size, std::optional<TlsConfig> tls)
    : client_backlog_size_(client_backlog_size), tls_(std::move(tls)) {}

WebSocketMiddleware::~WebSocketMiddleware() {
  shutdown();
}

tl::expected<void, std::string> WebSocketMiddleware::initialize(uint16_t port) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (initialized_) {
    return tl::unexpected<std::string>("WebSocket middleware already initialized");
  }

  auto io = std::make_unique<Io>();
  if (tls_.has_value()) {
#ifndef PJ_BRIDGE_TLS
    return tl::unexpected<std::string>("TLS requested but pj_bridge was built without TLS support");
#else
    if (!std::ifstream(tls_->certfile).good()) {
      return tl::unexpected<std::string>("TLS certificate file not found or not readable: " + tls_->certfile);
    }
    if (!std::ifstream(tls_->keyfile).good()) {
      return tl::unexpected<std::string>("TLS key file not found or not readable: " + tls_->keyfile);
    }
    try {
      io->ssl.emplace(asio::ssl::context::tls_server);
      io->ssl->set_options(
          asio::ssl::context::default_workarounds | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
      io->ssl->use_certificate_chain_file(tls_->certfile);
      io->ssl->use_private_key_file(tls_->keyfile, asio::ssl::context::pem);
    } catch (const boost::system::system_error& e) {
      return tl::unexpected<std::string>(std::string("TLS setup failed: ") + e.what());
    }
#endif
  }

  beast::error_code ec;
  const tcp::endpoint endpoint(tcp::v4(), port);
  io->acceptor.open(endpoint.protocol(), ec);
  if (!ec) {
    io->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  }
  if (!ec) {
    io->acceptor.bind(endpoint, ec);
  }
  if (!ec) {
    io->acceptor.listen(asio::socket_base::max_listen_connections, ec);
  }
  if (ec) {
    return tl::unexpected<std::string>("Failed to listen on port " + std::to_string(port) + ": " + ec.message());
  }

  do_accept(*io);
  io->thread = std::thread([raw = io.get()] {
    try {
      raw->ioc.run();
    } catch (const std::exception& e) {
      spdlog::error("WebSocket io thread stopped: {}", e.what());
    }
    raw->done.set_value();
  });
  io_ = std::move(io);
  initialized_ = true;
  return {};
}

void WebSocketMiddleware::do_accept(Io& io) {
  io.acceptor.async_accept([this, &io](beast::error_code ec, tcp::socket socket) {
    if (!io.acceptor.is_open()) {
      return;  // shutdown() closed it
    }
    if (ec) {
      // ponytail: retries at once; a persistent error (EMFILE) spins until it clears.
      spdlog::warn("WebSocket accept failed: {}", ec.message());
      return do_accept(io);
    }
    beast::error_code ignored;
    socket.set_option(tcp::no_delay(true), ignored);
#ifdef TCP_NOTSENT_LOWAT
    const int max_unsent = kMaxUnsentBytes;
    if (setsockopt(socket.native_handle(), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &max_unsent, sizeof(max_unsent)) != 0) {
      spdlog::warn("Could not set TCP_NOTSENT_LOWAT; a slow client may see extra latency");
    }
#endif
    std::string id = std::to_string(next_client_id_++);
    std::shared_ptr<Session> session;
#ifdef PJ_BRIDGE_TLS
    if (io.ssl) {
      session = std::make_shared<SessionImpl<TlsStream>>(*this, id, client_backlog_size_, std::move(socket), *io.ssl);
    }
#endif
    if (!session) {
      session = std::make_shared<SessionImpl<beast::tcp_stream>>(*this, id, client_backlog_size_, std::move(socket));
    }
    session->start();
    do_accept(io);
  });
}

void WebSocketMiddleware::on_open(const std::shared_ptr<Session>& session) {
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_[session->id] = session;
  }
  ConnectionCallback cb;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    cb = on_connect_;
  }
  if (cb) {
    cb(session->id);
  }
}

void WebSocketMiddleware::on_message(const std::string& client_id, std::string text) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (incoming_queue_.size() >= kMaxIncomingQueueSize) {
    spdlog::warn(
        "Incoming queue full ({} messages), dropping message from client '{}'", kMaxIncomingQueueSize, client_id);
    return;
  }
  incoming_queue_.push_back(IncomingRequest{client_id, std::vector<uint8_t>(text.begin(), text.end())});
}

void WebSocketMiddleware::on_closed(const std::string& client_id) {
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
      auto& queue = it->second->queue;
      queue.close();  // first, so no producer adds to the counters read below
      dropped_from_departed_ += queue.dropped_total();
      shed_from_departed_ += queue.shed_total();
      clients_.erase(it);
    }
    // Requests the client sent before leaving would otherwise re-create its
    // session when the bridge gets to them.
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    incoming_queue_.erase(
        std::remove_if(
            incoming_queue_.begin(), incoming_queue_.end(),
            [&](const IncomingRequest& req) { return req.client_id == client_id; }),
        incoming_queue_.end());
  }
  ConnectionCallback cb;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    cb = on_disconnect_;
  }
  if (cb) {
    cb(client_id);
  }
}

void WebSocketMiddleware::shutdown() {
  std::unique_ptr<Io> io;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) {
      return;
    }
    initialized_ = false;
    on_connect_ = nullptr;
    on_disconnect_ = nullptr;
    io = std::move(io_);
  }

  // On the io thread: stop accepting and close every connection. The io
  // thread then runs out of work and returns, unless a client stops reading;
  // the deadline below covers that.
  asio::post(io->ioc, [this, raw = io.get()] {
    beast::error_code ignored;
    raw->acceptor.close(ignored);
    std::vector<std::shared_ptr<Session>> sessions;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      for (const auto& [id, session] : clients_) {
        sessions.push_back(session);
      }
    }
    for (const auto& session : sessions) {
      session->close();
    }
  });
  if (io->done.get_future().wait_for(std::chrono::seconds(kShutdownTimeoutSeconds)) != std::future_status::ready) {
    spdlog::warn("[shutdown] WebSocket clients did not close within {}s; dropping them", kShutdownTimeoutSeconds);
    io->ioc.stop();
  }
  io->thread.join();  // also waits for a callback still running

  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& [id, session] : clients_) {
      session->queue.close();
      dropped_from_departed_ += session->queue.dropped_total();
      shed_from_departed_ += session->queue.shed_total();
    }
    clients_.clear();  // before the io_context their sockets belong to
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    incoming_queue_.clear();
  }
  // io is destroyed here: the io_context destroys its pending handlers, and
  // with them the last sessions.
}

bool WebSocketMiddleware::receive_request(std::vector<uint8_t>& data, std::string& client_identity) {
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

template <class Push>
bool WebSocketMiddleware::push_to(const std::string& client_identity, Push&& push) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  auto it = clients_.find(client_identity);
  if (it == clients_.end()) {
    return false;
  }
  const auto& session = it->second;
  if (!push(session->queue)) {
    if (session->queue.reply_budget_exceeded()) {
      spdlog::warn("Client '{}' is not reading its replies; closing the connection", client_identity);
      asio::post(session->executor, [session] { session->abort(); });
    }
    return false;
  }
  asio::post(session->executor, [session] { session->kick(); });
  return true;
}

bool WebSocketMiddleware::send_reply(const std::string& client_identity, const std::vector<uint8_t>& data) {
  std::string text(data.begin(), data.end());
  return push_to(client_identity, [&](OutboundQueue& queue) { return queue.push_text(std::move(text)); });
}

bool WebSocketMiddleware::send_binary(
    const std::string& client_identity, const std::vector<uint8_t>& data, FramePriority priority,
    const std::string& key) {
  std::string frame(reinterpret_cast<const char*>(data.data()), data.size());
  return push_to(client_identity, [&](OutboundQueue& queue) {
    switch (priority) {
      case FramePriority::kHeavy:
        return queue.push_heavy(key, std::move(frame));
      case FramePriority::kLatched:
        return queue.push_kept(std::move(frame));
      case FramePriority::kNormal:
        break;
    }
    return queue.push_normal(std::move(frame));
  });
}

// Reads are never blocked by a write in the common case, but Beast's read
// does wait for a write in progress when it must answer a Ping (or Close), so
// a client that pings would have its heartbeats unread for as long as a large
// frame takes to cross a slow link. The kernel's receive time has no such gap.
bool WebSocketMiddleware::heard_from_client_within(
    const std::string& client_identity, std::chrono::milliseconds window) const {
#ifdef __linux__
  std::lock_guard<std::mutex> lock(clients_mutex_);
  auto it = clients_.find(client_identity);
  if (it == clients_.end()) {
    return false;
  }
  tcp_info info{};
  socklen_t len = sizeof(info);
  if (getsockopt(it->second->fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0) {
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
  std::lock_guard<std::mutex> lock(clients_mutex_);
  auto it = clients_.find(client_identity);
  if (it != clients_.end()) {
    it->second->queue.drop_frames();
  }
}

uint64_t WebSocketMiddleware::dropped_frame_count() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  uint64_t total = dropped_from_departed_;
  for (const auto& [id, session] : clients_) {
    total += session->queue.dropped_total();
  }
  return total;
}

uint64_t WebSocketMiddleware::heavy_shed_count() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  uint64_t total = shed_from_departed_;
  for (const auto& [id, session] : clients_) {
    total += session->queue.shed_total();
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
