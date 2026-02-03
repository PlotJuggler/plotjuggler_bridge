// Copyright 2025
// ROS2 Bridge - Session Manager

#ifndef PJ_ROS_BRIDGE__SESSION_MANAGER_HPP_
#define PJ_ROS_BRIDGE__SESSION_MANAGER_HPP_

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pj_ros_bridge {

/**
 * @brief Represents a client session
 */
struct Session {
  /// Unique client identifier (from WebSocket connection)
  std::string client_id;

  /// Map of topic names to max rate in Hz (0.0 = unlimited)
  std::unordered_map<std::string, double> subscribed_topics;

  /// Timestamp of last heartbeat received
  std::chrono::steady_clock::time_point last_heartbeat;

  /// Session creation timestamp
  std::chrono::steady_clock::time_point created_at;
};

/**
 * @brief Manages client sessions and tracks subscriptions
 *
 * Thread-safe class that manages active client sessions, tracks
 * per-client subscriptions, monitors heartbeats, and handles
 * session timeouts.
 */
class SessionManager {
 public:
  /**
   * @brief Constructor
   * @param timeout_seconds Timeout duration for client sessions (default: 10 seconds)
   */
  explicit SessionManager(double timeout_seconds = 10.0);

  /**
   * @brief Create a new session for a client
   * @param client_id Unique client identifier
   * @return true if session created, false if already exists
   */
  bool create_session(const std::string& client_id);

  /**
   * @brief Update heartbeat timestamp for a client session
   * @param client_id Client identifier
   * @return true if session exists and was updated, false otherwise
   */
  bool update_heartbeat(const std::string& client_id);

  /**
   * @brief Get a session by client ID
   * @param client_id Client identifier
   * @param session Output parameter for session data
   * @return true if session exists, false otherwise
   */
  bool get_session(const std::string& client_id, Session& session) const;

  /**
   * @brief Update the subscribed topics for a client session
   * @param client_id Client identifier
   * @param topics Set of topic names
   * @return true if session exists and was updated, false otherwise
   */
  bool update_subscriptions(const std::string& client_id, const std::unordered_map<std::string, double>& topics);

  /**
   * @brief Get the subscribed topics for a client
   * @param client_id Client identifier
   * @return Set of subscribed topic names (empty if session doesn't exist)
   */
  std::unordered_map<std::string, double> get_subscriptions(const std::string& client_id) const;

  /**
   * @brief Remove a session
   * @param client_id Client identifier
   * @return true if session was removed, false if it didn't exist
   */
  bool remove_session(const std::string& client_id);

  /**
   * @brief Check for timed-out sessions and return their IDs
   * @return Vector of client IDs that have timed out
   */
  std::vector<std::string> get_timed_out_sessions();

  /**
   * @brief Get all active session IDs
   * @return Vector of active client IDs
   */
  std::vector<std::string> get_active_sessions() const;

  /**
   * @brief Get the number of active sessions
   * @return Number of active sessions
   */
  size_t session_count() const;

  /**
   * @brief Check if a session exists
   * @param client_id Client identifier
   * @return true if session exists, false otherwise
   */
  bool session_exists(const std::string& client_id) const;

 private:
  /// Map of client ID to session data
  std::unordered_map<std::string, Session> sessions_;

  /// Mutex for thread-safe access
  mutable std::mutex mutex_;

  /// Session timeout duration
  std::chrono::duration<double> timeout_duration_;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__SESSION_MANAGER_HPP_
