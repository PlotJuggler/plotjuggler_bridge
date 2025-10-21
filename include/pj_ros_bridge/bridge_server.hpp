// Copyright 2025
// ROS2 Bridge - Bridge Server

#ifndef PJ_ROS_BRIDGE__BRIDGE_SERVER_HPP_
#define PJ_ROS_BRIDGE__BRIDGE_SERVER_HPP_

#include <memory>
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
 * Orchestrates the ROS2 to ZMQ bridge by managing:
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
   * @param req_port REQ-REP socket port (default: 5555)
   * @param pub_port PUB socket port (default: 5556)
   * @param session_timeout Session timeout in seconds (default: 10.0)
   */
  explicit BridgeServer(
      std::shared_ptr<rclcpp::Node> node, std::shared_ptr<MiddlewareInterface> middleware, int req_port = 5555,
      int pub_port = 5556, double session_timeout = 10.0);

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

 private:
  /**
   * @brief Handle "get_topics" API command
   * @param client_id Client identifier
   * @return JSON response string
   */
  std::string handle_get_topics(const std::string& client_id);

  /**
   * @brief Handle "subscribe" API command
   * @param client_id Client identifier
   * @param request_json JSON request containing topics to subscribe to
   * @return JSON response string
   */
  std::string handle_subscribe(const std::string& client_id, const std::string& request_json);

  /**
   * @brief Handle "heartbeat" API command
   * @param client_id Client identifier
   * @return JSON response string
   */
  std::string handle_heartbeat(const std::string& client_id);

  /**
   * @brief Create error response JSON
   * @param error_code Error code string
   * @param message Error message
   * @return JSON error response string
   */
  std::string create_error_response(const std::string& error_code, const std::string& message) const;

  /**
   * @brief Timer callback to check for timed-out sessions
   */
  void check_session_timeouts();

  /**
   * @brief Cleanup a timed-out session
   * @param client_id Client identifier
   */
  void cleanup_session(const std::string& client_id);

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

  // Configuration
  int req_port_;
  int pub_port_;
  double session_timeout_;

  // State
  bool initialized_;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__BRIDGE_SERVER_HPP_
