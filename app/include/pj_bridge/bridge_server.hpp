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

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "pj_bridge/message_buffer.hpp"
#include "pj_bridge/middleware/middleware_interface.hpp"
#include "pj_bridge/session_manager.hpp"
#include "pj_bridge/subscription_manager_interface.hpp"
#include "pj_bridge/topic_source_interface.hpp"

namespace pj_bridge {

/**
 * @brief Main bridge server that coordinates all components
 *
 * Orchestrates the bridge by managing:
 * - Client sessions and heartbeats
 * - Topic discovery and subscriptions (via abstract interfaces)
 * - Message buffering and aggregation
 * - API request/response handling
 *
 * Thread-safe for concurrent client connections.
 * Event loop is driven externally (no internal timers).
 */
class BridgeServer {
 public:
  struct StatsSnapshot {
    std::unordered_map<std::string, uint64_t> topic_receive_counts;
    std::unordered_map<std::string, uint64_t> topic_forward_counts;
    uint64_t publish_cycles;
    uint64_t total_bytes_published;
  };

  /**
   * @brief Constructor
   * @param topic_source Backend-specific topic discovery and schema provider
   * @param subscription_manager Backend-specific subscription manager
   * @param middleware Middleware interface for network communication
   * @param port Server port (default: 9090)
   * @param session_timeout Session timeout in seconds (default: 10.0)
   * @param publish_rate Message aggregation publish rate in Hz (default: 50.0)
   */
  explicit BridgeServer(
      std::shared_ptr<TopicSourceInterface> topic_source,
      std::shared_ptr<SubscriptionManagerInterface> subscription_manager,
      std::shared_ptr<MiddlewareInterface> middleware, int port = 9090, double session_timeout = 10.0,
      double publish_rate = 50.0);

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
   * @brief Publish aggregated messages to connected clients
   *
   * Called externally by the event loop (e.g. at 50 Hz).
   * Moves messages from the buffer, serializes per subscription group,
   * compresses with ZSTD, and sends binary frames.
   */
  void publish_aggregated_messages();

  /**
   * @brief Check for timed-out sessions and clean them up
   *
   * Called externally by the event loop (e.g. every 1 second).
   */
  void check_session_timeouts();

  /**
   * @brief Get the number of active client sessions
   */
  size_t get_active_session_count() const;

  /**
   * @brief Get statistics about published messages
   * @return Pair of (total_messages_published, total_bytes_published)
   */
  std::pair<uint64_t, uint64_t> get_publish_stats() const;

  /**
   * @brief Snapshot and reset statistics counters
   */
  StatsSnapshot snapshot_and_reset_stats();

 private:
  std::string handle_get_topics(const std::string& client_id, const nlohmann::json& request);
  std::string handle_subscribe(const std::string& client_id, const nlohmann::json& request);
  std::string handle_unsubscribe(const std::string& client_id, const nlohmann::json& request);
  std::string handle_heartbeat(const std::string& client_id, const nlohmann::json& request);
  std::string handle_pause(const std::string& client_id, const nlohmann::json& request);
  std::string handle_resume(const std::string& client_id, const nlohmann::json& request);
  std::string create_error_response(
      const std::string& error_code, const std::string& message, const nlohmann::json& request) const;
  void inject_response_fields(nlohmann::json& response, const nlohmann::json& request) const;
  void cleanup_session(const std::string& client_id);

  // Backend interfaces (owned)
  std::shared_ptr<TopicSourceInterface> topic_source_;
  std::shared_ptr<SubscriptionManagerInterface> subscription_manager_;

  // Core components
  std::shared_ptr<MiddlewareInterface> middleware_;
  std::shared_ptr<MessageBuffer> message_buffer_;
  std::unique_ptr<SessionManager> session_manager_;

  // Configuration
  int port_;
  double session_timeout_;
  double publish_rate_;

  // State
  std::atomic<bool> initialized_;

  // Statistics
  uint64_t total_messages_published_;
  uint64_t total_bytes_published_;
  uint64_t publish_cycles_;
  std::unordered_map<std::string, uint64_t> topic_receive_counts_;
  std::unordered_map<std::string, uint64_t> topic_forward_counts_;
  mutable std::mutex stats_mutex_;

  // Lock ordering (to prevent deadlock):
  //   cleanup_mutex_ > last_sent_mutex_ > stats_mutex_
  // SessionManager::mutex_ may be acquired while holding any of these.
  // Never acquire a higher-order lock while holding a lower-order one.
  std::mutex cleanup_mutex_;

  // Per-client per-topic last-sent timestamp (nanoseconds) for rate limiting
  std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> last_sent_times_;
  std::mutex last_sent_mutex_;
};

}  // namespace pj_bridge
