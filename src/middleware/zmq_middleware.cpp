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

#include "pj_ros_bridge/middleware/zmq_middleware.hpp"

#include <sstream>
#include <stdexcept>

namespace pj_ros_bridge {

ZmqMiddleware::ZmqMiddleware() : initialized_(false), req_port_(0), pub_port_(0) {}

ZmqMiddleware::~ZmqMiddleware() {
  shutdown();
}

tl::expected<void, std::string> ZmqMiddleware::initialize(uint16_t req_port, uint16_t pub_port) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (initialized_) {
    return tl::unexpected("Middleware already initialized");
  }

  try {
    // Create ZMQ context
    context_ = std::make_unique<zmq::context_t>(1);

    // Create and bind REP socket for API requests
    rep_socket_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::rep);

    // Set socket options
    int linger = 0;
    rep_socket_->set(zmq::sockopt::linger, linger);
    rep_socket_->set(zmq::sockopt::rcvtimeo, kZmqReceiveTimeout);

    std::string rep_address = "tcp://*:" + std::to_string(req_port);
    try {
      rep_socket_->bind(rep_address);
    } catch (const zmq::error_t& e) {
      pub_socket_.reset();
      rep_socket_.reset();
      context_.reset();
      return tl::unexpected(
          "Failed to bind REP socket to port " + std::to_string(req_port) + ": " + std::string(e.what()) + " (errno " +
          std::to_string(e.num()) + ")");
    }

    // Create and bind PUB socket for data streaming
    pub_socket_ = std::make_unique<zmq::socket_t>(*context_, zmq::socket_type::pub);
    pub_socket_->set(zmq::sockopt::linger, linger);

    std::string pub_address = "tcp://*:" + std::to_string(pub_port);
    try {
      pub_socket_->bind(pub_address);
    } catch (const zmq::error_t& e) {
      pub_socket_.reset();
      rep_socket_.reset();
      context_.reset();
      return tl::unexpected(
          "Failed to bind PUB socket to port " + std::to_string(pub_port) + ": " + std::string(e.what()) + " (errno " +
          std::to_string(e.num()) + ")");
    }

    req_port_ = req_port;
    pub_port_ = pub_port;
    initialized_ = true;

    return {};  // Success
  } catch (const zmq::error_t& e) {
    // Cleanup on error
    pub_socket_.reset();
    rep_socket_.reset();
    context_.reset();
    initialized_ = false;
    return tl::unexpected(
        "ZMQ initialization failed: " + std::string(e.what()) + " (errno " + std::to_string(e.num()) + ")");
  } catch (const std::exception& e) {
    pub_socket_.reset();
    rep_socket_.reset();
    context_.reset();
    initialized_ = false;
    return tl::unexpected("Unexpected error during initialization: " + std::string(e.what()));
  }
}

void ZmqMiddleware::shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!initialized_) {
    return;
  }

  try {
    if (pub_socket_) {
      pub_socket_->close();
      pub_socket_.reset();
    }

    if (rep_socket_) {
      rep_socket_->close();
      rep_socket_.reset();
    }

    if (context_) {
      context_->close();
      context_.reset();
    }

    initialized_ = false;
    last_client_identity_.clear();
  } catch (const zmq::error_t&) {
    // Suppress errors during shutdown
  }
}

bool ZmqMiddleware::receive_request(std::vector<uint8_t>& data, std::string& client_identity) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!initialized_ || !rep_socket_) {
    return false;
  }

  try {
    zmq::message_t message;
    auto result = rep_socket_->recv(message, zmq::recv_flags::none);

    if (!result) {
      return false;  // Timeout or error
    }

    // Extract data
    data.resize(message.size());
    std::memcpy(data.data(), message.data(), message.size());

    // For ZeroMQ REP socket, we use the routing ID if available
    // For now, we'll generate a simple identity based on connection
    // In a full implementation, we could use ZMQ_IDENTITY or router patterns
    client_identity =
        "client_" +
        std::to_string(std::hash<std::string>{}(std::string(static_cast<const char*>(message.data()), message.size())));

    last_client_identity_ = client_identity;

    return true;
  } catch (const zmq::error_t&) {
    return false;
  }
}

bool ZmqMiddleware::send_reply(const std::vector<uint8_t>& data) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!initialized_ || !rep_socket_) {
    return false;
  }

  try {
    zmq::message_t message(data.size());
    std::memcpy(message.data(), data.data(), data.size());

    auto result = rep_socket_->send(message, zmq::send_flags::none);
    return result.has_value();
  } catch (const zmq::error_t&) {
    return false;
  }
}

bool ZmqMiddleware::publish_data(const std::vector<uint8_t>& data) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!initialized_ || !pub_socket_) {
    return false;
  }

  try {
    zmq::message_t message(data.size());
    std::memcpy(message.data(), data.data(), data.size());

    auto result = pub_socket_->send(message, zmq::send_flags::dontwait);
    return result.has_value();
  } catch (const zmq::error_t&) {
    return false;
  }
}

std::string ZmqMiddleware::get_client_identity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_client_identity_;
}

bool ZmqMiddleware::is_ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

}  // namespace pj_ros_bridge
