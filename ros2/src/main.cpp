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

#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <vector>

#include "pj_bridge/bridge_server.hpp"
#include "pj_bridge/middleware/websocket_middleware.hpp"
#include "pj_bridge/whitelist_filter.hpp"
#include "pj_bridge_ros2/ros2_subscription_manager.hpp"
#include "pj_bridge_ros2/ros2_topic_source.hpp"

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);

  auto node = std::make_shared<rclcpp::Node>("pj_bridge");

  RCLCPP_INFO(node->get_logger(), "Starting pj_bridge (ROS2 backend)...");

  // Declare and get parameters
  node->declare_parameter<int>("port", 9090);
  node->declare_parameter<double>("publish_rate", 50.0);
  node->declare_parameter<double>("session_timeout", 10.0);
  node->declare_parameter<bool>("strip_large_messages", false);
  node->declare_parameter<std::vector<std::string>>("topic_whitelist", {".*"});
  node->declare_parameter<int>("min_qos_depth", 1);
  node->declare_parameter<int>("max_qos_depth", 100);
  node->declare_parameter<double>("topic_poll_interval", 1.0);
  node->declare_parameter<int>("client_backlog_size", 100);
  node->declare_parameter<int>("heavy_frame_threshold_bytes", 262144);
  node->declare_parameter<bool>("tls", false);
  node->declare_parameter<std::string>("certfile", "");
  node->declare_parameter<std::string>("keyfile", "");

  int port = node->get_parameter("port").as_int();
  double publish_rate = node->get_parameter("publish_rate").as_double();
  double session_timeout = node->get_parameter("session_timeout").as_double();
  bool strip_large_messages = node->get_parameter("strip_large_messages").as_bool();
  std::vector<std::string> topic_whitelist = node->get_parameter("topic_whitelist").as_string_array();
  int64_t min_qos_depth = node->get_parameter("min_qos_depth").as_int();
  int64_t max_qos_depth = node->get_parameter("max_qos_depth").as_int();
  double topic_poll_interval = node->get_parameter("topic_poll_interval").as_double();
  int64_t client_backlog_size = node->get_parameter("client_backlog_size").as_int();
  int64_t heavy_frame_threshold_bytes = node->get_parameter("heavy_frame_threshold_bytes").as_int();
  bool tls_enabled = node->get_parameter("tls").as_bool();
  std::string certfile = node->get_parameter("certfile").as_string();
  std::string keyfile = node->get_parameter("keyfile").as_string();

  RCLCPP_INFO(
      node->get_logger(),
      "Configuration: port=%d, publish_rate=%.1f Hz, session_timeout=%.1f s, strip_large_messages=%s, "
      "min_qos_depth=%ld, max_qos_depth=%ld, topic_poll_interval=%.1f s, client_backlog_size=%ld, tls=%s",
      port, publish_rate, session_timeout, strip_large_messages ? "true" : "false", min_qos_depth, max_qos_depth,
      topic_poll_interval, client_backlog_size, tls_enabled ? "true" : "false");

  if (tls_enabled && (certfile.empty() || keyfile.empty())) {
    RCLCPP_ERROR(node->get_logger(), "tls=true requires both 'certfile' and 'keyfile' parameters to be set");
    rclcpp::shutdown();
    return 1;
  }

  if (topic_poll_interval < 0.0) {
    RCLCPP_ERROR(
        node->get_logger(), "Invalid topic_poll_interval: %.1f (must be >= 0; 0 disables polling)",
        topic_poll_interval);
    rclcpp::shutdown();
    return 1;
  }

  if (client_backlog_size <= 0) {
    RCLCPP_ERROR(node->get_logger(), "Invalid client_backlog_size: %ld (must be > 0)", client_backlog_size);
    rclcpp::shutdown();
    return 1;
  }

  if (heavy_frame_threshold_bytes < 0) {
    RCLCPP_ERROR(
        node->get_logger(), "Invalid heavy_frame_threshold_bytes: %ld (must be >= 0; 0 disables splitting)",
        heavy_frame_threshold_bytes);
    rclcpp::shutdown();
    return 1;
  }
  RCLCPP_INFO(node->get_logger(), "heavy_frame_threshold_bytes=%ld", heavy_frame_threshold_bytes);

  auto whitelist_result = pj_bridge::WhitelistFilter::create(topic_whitelist);
  if (!whitelist_result) {
    RCLCPP_ERROR(node->get_logger(), "Invalid topic_whitelist: %s", whitelist_result.error().c_str());
    rclcpp::shutdown();
    return 1;
  }

  if (min_qos_depth < 0 || max_qos_depth < 0 || min_qos_depth > max_qos_depth) {
    RCLCPP_ERROR(
        node->get_logger(),
        "Invalid QoS depth configuration: min_qos_depth=%ld, max_qos_depth=%ld (both must be >= 0 and "
        "min_qos_depth <= max_qos_depth)",
        min_qos_depth, max_qos_depth);
    rclcpp::shutdown();
    return 1;
  }

  try {
    // Create backend components
    auto topic_source = std::make_shared<pj_bridge::Ros2TopicSource>(node);
    auto sub_manager = std::make_shared<pj_bridge::Ros2SubscriptionManager>(
        node, strip_large_messages, static_cast<size_t>(min_qos_depth), static_cast<size_t>(max_qos_depth));
    std::optional<pj_bridge::TlsConfig> tls_config;
    if (tls_enabled) {
      tls_config = pj_bridge::TlsConfig{certfile, keyfile};
    }
    auto middleware =
        std::make_shared<pj_bridge::WebSocketMiddleware>(static_cast<size_t>(client_backlog_size), tls_config);

    // Create bridge server
    pj_bridge::BridgeServer server(
        topic_source, sub_manager, middleware, port, session_timeout, publish_rate, std::move(whitelist_result.value()),
        static_cast<size_t>(heavy_frame_threshold_bytes));

    if (!server.initialize()) {
      RCLCPP_ERROR(node->get_logger(), "Failed to initialize bridge server");
      rclcpp::shutdown();
      return 1;
    }

    RCLCPP_INFO(node->get_logger(), "Bridge server initialized successfully");
    RCLCPP_INFO(node->get_logger(), "Ready to accept WebSocket connections on port %d", port);

    // ROS2 timers drive the event loop
    using namespace std::chrono_literals;

    auto request_timer = node->create_wall_timer(10ms, [&server]() { server.process_requests(); });

    auto publish_period = std::chrono::duration<double>(1.0 / publish_rate);
    auto publish_timer = node->create_wall_timer(
        std::chrono::duration_cast<std::chrono::nanoseconds>(publish_period),
        [&server]() { server.publish_aggregated_messages(); });

    auto timeout_timer = node->create_wall_timer(1s, [&server]() { server.check_session_timeouts(); });

    // topic_poll_interval == 0 disables the pushed topic-advertisement poll.
    rclcpp::TimerBase::SharedPtr topic_poll_timer;
    if (topic_poll_interval > 0.0) {
      // Take the silent baseline snapshot now, before any client can connect,
      // so topics appearing before the first timer tick are notified rather
      // than folded into the baseline.
      server.check_topic_changes();
      topic_poll_timer = node->create_wall_timer(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(topic_poll_interval)),
          [&server]() { server.check_topic_changes(); });
    }

    // Spin until shutdown. spin() blocks waiting for work and returns when
    // rclcpp::shutdown() runs (e.g. on SIGINT) — unlike spin_some() in a
    // loop, which returns immediately when idle and busy-spins a full core.
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);

    executor.spin();

    // Graceful shutdown
    RCLCPP_INFO(node->get_logger(), "Shutting down bridge server...");

    request_timer->cancel();
    publish_timer->cancel();
    timeout_timer->cancel();
    if (topic_poll_timer) {
      topic_poll_timer->cancel();
    }
    executor.remove_node(node);

    // Clear the subscription manager callback before server destruction
    sub_manager->set_message_callback(nullptr);
    sub_manager->unsubscribe_all();
    // middleware->shutdown() is handled by BridgeServer destructor

    auto [total_messages, total_bytes] = server.get_publish_stats();
    RCLCPP_INFO(
        node->get_logger(), "Final statistics: %lu messages published, %lu bytes transmitted", total_messages,
        total_bytes);

    RCLCPP_INFO(node->get_logger(), "Bridge server shutdown complete");

  } catch (const std::exception& e) {
    RCLCPP_ERROR(node->get_logger(), "Exception in main: %s", e.what());
    rclcpp::shutdown();
    return 1;
  }

  rclcpp::shutdown();
  return 0;
}
