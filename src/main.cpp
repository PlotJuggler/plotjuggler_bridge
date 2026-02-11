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

#include <memory>
#include <rclcpp/rclcpp.hpp>

#include "pj_ros_bridge/bridge_server.hpp"
#include "pj_ros_bridge/middleware/websocket_middleware.hpp"

int main(int argc, char** argv) {
  // Initialize ROS2
  rclcpp::init(argc, argv);

  // Create node
  auto node = std::make_shared<rclcpp::Node>("pj_ros_bridge");

  RCLCPP_INFO(node->get_logger(), "Starting pj_ros_bridge server...");

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
    // Create middleware
    auto middleware = std::make_shared<pj_ros_bridge::WebSocketMiddleware>();

    // Create bridge server
    auto bridge_server = std::make_shared<pj_ros_bridge::BridgeServer>(
        node, middleware, port, session_timeout, publish_rate, strip_large_messages);

    // Initialize server
    if (!bridge_server->initialize()) {
      RCLCPP_ERROR(node->get_logger(), "Failed to initialize bridge server");
      rclcpp::shutdown();
      return 1;
    }

    RCLCPP_INFO(node->get_logger(), "Bridge server initialized successfully");
    RCLCPP_INFO(node->get_logger(), "Ready to accept WebSocket connections on port %d", port);

    // Create ROS timer to process API requests at 100 Hz
    auto request_timer = node->create_wall_timer(
        std::chrono::milliseconds(10),  // 100 Hz
        [&bridge_server]() { bridge_server->process_requests(); });

    // Create single-threaded executor and add node
    rclcpp::executors::SingleThreadedExecutor executor;
    executor.add_node(node);

    // Spin until shutdown is requested
    while (rclcpp::ok()) {
      executor.spin_some(std::chrono::milliseconds(100));
    }

    // Graceful shutdown
    RCLCPP_INFO(node->get_logger(), "Shutting down bridge server...");

    // Cancel timer and remove node from executor
    request_timer->cancel();
    executor.remove_node(node);

    // Get final statistics
    auto [total_messages, total_bytes] = bridge_server->get_publish_stats();
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
