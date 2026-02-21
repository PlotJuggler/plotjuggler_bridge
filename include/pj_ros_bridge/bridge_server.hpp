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

#ifndef PJ_ROS_BRIDGE__BRIDGE_SERVER_HPP_
#define PJ_ROS_BRIDGE__BRIDGE_SERVER_HPP_

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <string>

#include "pj_ros_bridge/generic_subscription_manager.hpp"
#include "pj_ros_bridge/message_buffer.hpp"
#include "pj_ros_bridge/middleware/middleware_interface.hpp"
#include "pj_ros_bridge/schema_extractor.hpp"
#include "pj_ros_bridge/session_manager.hpp"
#include "pj_ros_bridge/topic_discovery.hpp"

namespace pj_ros_bridge {

/**
 * @brief Main bridge server that coordinates all components
 *
 * Orchestrates the ROS2 bridge by managing:
 * - Client sessions and heartbeats
 * - Topic discovery and subscriptions
 * - Message buffering and aggregation
 * - API request/response handling
 *
 * Thread-safe for concurrent client connections.
 */
class BridgeServer {
 public:
  /**
   * @brief Constructor
   * @param node ROS2 node for communication
   * @param middleware Middleware interface for network communication
   * @param port Server port (default: 8080)
   * @param session_timeout Session timeout in seconds (default: 10.0)
   * @param publish_rate Message aggregation publish rate in Hz (default: 50.0)
   */
  explicit BridgeServer(
      std::shared_ptr<rclcpp::Node> node, std::shared_ptr<MiddlewareInterface> middleware, int port = 8080,
      double session_timeout = 10.0, double publish_rate = 50.0, bool strip_large_messages = true);

  /// Shuts down middleware before members are destroyed, preventing
  /// disconnect callbacks from firing into a partially destroyed object.
  ~BridgeServer();

  /**
   * @brief Initialize the bridge server
   * @return true if initialization successful, false otherwise
   */
  bool initialize();

  /**
   * @brief Process incoming API requests
   *
   * Non-blocking call that checks for pending requests,
   * processes them, and sends responses.
   *
   * @return true if a request was processed, false if no requests pending
   */
  bool process_requests();

  /**
   * @brief Get the number of active client sessions
   * @return Number of active sessions
   */
  size_t get_active_session_count() const;

  /**
   * @brief Get statistics about published messages
   * @return Pair of (total_messages_published, total_bytes_published)
   */
  std::pair<uint64_t, uint64_t> get_publish_stats() const;

 private:
  std::string handle_get_topics(const std::string& client_id, const nlohmann::json& request);
  std::string handle_subscribe(const std::string& client_id, const nlohmann::json& request);
  std::string handle_unsubscribe(const std::string& client_id, const nlohmann::json& request);
  std::string handle_heartbeat(const std::string& client_id, const nlohmann::json& request);

  /// Handle pause command - pauses binary frame delivery for client
  std::string handle_pause(const std::string& client_id, const nlohmann::json& request);

  /// Handle resume command - resumes binary frame delivery for client
  std::string handle_resume(const std::string& client_id, const nlohmann::json& request);
  std::string create_error_response(
      const std::string& error_code, const std::string& message, const nlohmann::json& request) const;
  void check_session_timeouts();

  /// Inject standard response fields (protocol_version, optional id)
  void inject_response_fields(nlohmann::json& response, const nlohmann::json& request) const;
  void cleanup_session(const std::string& client_id);
  void publish_aggregated_messages();

  /// Create a message callback that buffers messages with optional stripping.
  /// If stripping fails (e.g., corrupted CDR data), the original message
  /// is forwarded instead of crashing the callback.
  MessageCallback make_buffer_callback(const std::string& topic_type);

  // ROS2 components
  std::shared_ptr<rclcpp::Node> node_;

  // Core components
  std::shared_ptr<MiddlewareInterface> middleware_;
  std::unique_ptr<TopicDiscovery> topic_discovery_;
  std::unique_ptr<SchemaExtractor> schema_extractor_;
  std::unique_ptr<GenericSubscriptionManager> subscription_manager_;
  std::shared_ptr<MessageBuffer> message_buffer_;
  std::unique_ptr<SessionManager> session_manager_;

  // Timers
  rclcpp::TimerBase::SharedPtr session_timeout_timer_;
  rclcpp::TimerBase::SharedPtr publish_timer_;

  // Configuration
  int port_;
  double session_timeout_;
  double publish_rate_;

  // State
  bool initialized_;

  // Statistics
  uint64_t total_messages_published_;
  uint64_t total_bytes_published_;
  mutable std::mutex stats_mutex_;

  // Message stripping configuration
  bool strip_large_messages_;

  // Protects cleanup_session from concurrent calls (disconnect + timeout)
  std::mutex cleanup_mutex_;

  // Per-client per-topic last-sent timestamp (nanoseconds) for rate limiting
  std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> last_sent_times_;
  std::mutex last_sent_mutex_;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__BRIDGE_SERVER_HPP_
