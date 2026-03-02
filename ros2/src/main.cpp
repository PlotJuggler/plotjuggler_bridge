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
#include <rclcpp/rclcpp.hpp>

#include "pj_bridge/bridge_server.hpp"
#include "pj_bridge/middleware/websocket_middleware.hpp"
#include "pj_bridge_ros2/ros2_subscription_manager.hpp"
#include "pj_bridge_ros2/ros2_topic_source.hpp"

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);

  auto node = std::make_shared<rclcpp::Node>("pj_bridge");

  RCLCPP_INFO(node->get_logger(), "Starting pj_bridge (ROS2 backend)...");

  // Declare and get parameters
  node->declare_parameter<int>("port", 8080);
  node->declare_parameter<double>("publish_rate", 50.0);
  node->declare_parameter<double>("session_timeout", 10.0);
  node->declare_parameter<bool>("strip_large_messages", true);

  int port = node->get_parameter("port").as_int();
  double publish_rate = node->get_parameter("publish_rate").as_double();
  double session_timeout = node->get_parameter("session_timeout").as_double();
  bool strip_large_messages = node->get_parameter("strip_large_messages").as_bool();

  RCLCPP_INFO(
      node->get_logger(),
      "Configuration: port=%d, publish_rate=%.1f Hz, session_timeout=%.1f s, strip_large_messages=%s", port,
      publish_rate, session_timeout, strip_large_messages ? "true" : "false");

  try {
    // Create backend components
    auto topic_source = std::make_shared<pj_bridge::Ros2TopicSource>(node);
    auto sub_manager = std::make_shared<pj_bridge::Ros2SubscriptionManager>(node, strip_large_messages);
    auto middleware = std::make_shared<pj_bridge::WebSocketMiddleware>();

    // Create bridge server
    pj_bridge::BridgeServer server(topic_source, sub_manager, middleware, port, session_timeout, publish_rate);

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

    // Spin until shutdown
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);

    while (rclcpp::ok()) {
      executor.spin_some(100ms);
    }

    // Graceful shutdown
    RCLCPP_INFO(node->get_logger(), "Shutting down bridge server...");

    request_timer->cancel();
    publish_timer->cancel();
    timeout_timer->cancel();
    executor.remove_node(node);

    // Clear the subscription manager callback before server destruction
    sub_manager->set_message_callback(nullptr);
    sub_manager->unsubscribe_all();
    middleware->shutdown();

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
