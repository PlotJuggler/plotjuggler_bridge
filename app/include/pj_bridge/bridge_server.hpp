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
#include "pj_bridge/protocol_constants.hpp"
#include "pj_bridge/session_manager.hpp"
#include "pj_bridge/subscription_manager_interface.hpp"
#include "pj_bridge/topic_source_interface.hpp"
#include "pj_bridge/whitelist_filter.hpp"

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

/// Tunable configuration for BridgeServer (backend-agnostic). Bundled into one
/// struct so entry points construct the server with a single named aggregate
/// rather than a long positional argument list.
struct BridgeServerConfig {
  int port = 9090;                 ///< WebSocket port
  double session_timeout = 10.0;   ///< client session timeout, seconds
  double publish_rate = 50.0;      ///< message aggregation/publish rate, Hz
  WhitelistFilter whitelist = {};  ///< topic whitelist (default: matches everything)
  /// Per-message byte size at or above which a topic's message is isolated into
  /// its own size-class ("heavy") frame instead of being aggregated with light
  /// topics. 0 disables splitting (single aggregated frame). Default: 256 KiB.
  size_t heavy_frame_threshold_bytes = kDefaultHeavyFrameThresholdBytes;
};

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
   * @param config Tunable server configuration (see BridgeServerConfig)
   */
  explicit BridgeServer(
      std::shared_ptr<TopicSourceInterface> topic_source,
      std::shared_ptr<SubscriptionManagerInterface> subscription_manager,
      std::shared_ptr<MiddlewareInterface> middleware, BridgeServerConfig config = {});

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
   * Non-blocking call that drains pending requests (bounded per call),
   * processes them, and sends responses.
   *
   * @return true if at least one request was processed, false if none pending
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
   * @brief Check whether the (whitelist-filtered) topic set has changed and
   * notify opted-in clients
   *
   * Called externally by the event loop (e.g. every topic_poll_interval
   * seconds). The first call after construction only takes a snapshot and
   * sends nothing; subsequent calls diff against that snapshot and send a
   * `topics_changed` notification (added/removed) to every session that
   * called `subscribe_topic_updates`, but only when the diff is non-empty.
   */
  void check_topic_changes();

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
  void process_single_request(const std::vector<uint8_t>& request_data, const std::string& client_id);
  std::string handle_get_topics(const std::string& client_id, const nlohmann::json& request);
  std::string handle_subscribe(const std::string& client_id, const nlohmann::json& request);
  std::string handle_unsubscribe(const std::string& client_id, const nlohmann::json& request);
  std::string handle_heartbeat(const std::string& client_id, const nlohmann::json& request);
  std::string handle_pause(const std::string& client_id, const nlohmann::json& request);
  std::string handle_resume(const std::string& client_id, const nlohmann::json& request);
  std::string handle_subscribe_topic_updates(const std::string& client_id, const nlohmann::json& request);
  std::string handle_unsubscribe_topic_updates(const std::string& client_id, const nlohmann::json& request);
  std::string create_error_response(
      const std::string& error_code, const std::string& message, const nlohmann::json& request) const;
  void inject_response_fields(nlohmann::json& response, const nlohmann::json& request) const;
  void cleanup_session(const std::string& client_id);

  /// Extract a topic's schema, turning get_schema()'s throw-on-failure
  /// contract into a bool. On success @p schema_out holds the definition
  /// (which may legitimately be empty, e.g. std_msgs/msg/Empty); on failure
  /// returns false with the exception's message in @p error_out. The caller
  /// decides what a failure means — the subscribe path fails that one
  /// subscription, while the get_topics / topics_changed paths keep the topic
  /// listed without schema fields. Shared to keep both paths' extraction and
  /// failure handling identical.
  bool extract_schema(const std::string& topic_name, std::string& schema_out, std::string& error_out) const;

  /// Defensive optional-bool read: false unless @p key is present AND boolean
  /// (nlohmann's value() would throw on a wrong-typed value). Shared by every
  /// handler reading an opt-in flag off the wire.
  static bool optional_bool(const nlohmann::json& request, const char* key);

  /// Stamp `latched: true` on a topic-entry when the topic is transient-local;
  /// leave the key absent otherwise. Shared by get_topics and topics_changed.
  void attach_latched_badge(nlohmann::json& topic_entry, const std::string& topic_name) const;

  /// Add `encoding` + `definition` schema fields to a topic list entry when a
  /// client opts into up-front schemas (get_topics / topics_changed
  /// `include_schemas`). A per-topic extraction failure is swallowed: the
  /// entry is left as name+type only and a warning is logged, so one bad
  /// schema never drops a topic from the list — the same principle the
  /// subscribe path follows for the topic list. Uses the same `encoding` /
  /// `definition` field names as the subscribe response.
  void attach_schema_fields(nlohmann::json& topic_entry, const std::string& topic_name) const;

  /// Release one middleware subscription ref for @p topic_name. When the
  /// last ref is gone (subscription destroyed), also clears the topic's
  /// latched state in the message buffer: a stale retained sample must not
  /// be replayed to the next fresh subscriber, who receives the current
  /// sample from DDS transient_local redelivery instead. Call with
  /// cleanup_mutex_ held (same requirement as unsubscribe itself).
  void release_subscription_ref(const std::string& topic_name);

  /// Latched (transient_local) replay bookkeeping for a topic whose
  /// middleware subscription ref was just (re)acquired for @p client_id —
  /// shared by handle_subscribe and handle_resume. Records the topic's
  /// latched flag in the message buffer and, when a retained sample is
  /// available for replay, serializes it into a single-message binary frame
  /// queued in pending_replays_ (flushed by process_single_request right
  /// after the handler's response is sent). Call with cleanup_mutex_ held;
  /// acquires only the leaf replays_mutex_ and makes no middleware calls.
  void collect_latched_replay(const std::string& client_id, const std::string& topic_name);

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
  WhitelistFilter whitelist_;
  // Per-message byte size at or above which a topic is isolated into its own
  // size-class ("heavy") frame; 0 disables splitting. See publish_aggregated_messages().
  size_t heavy_frame_threshold_bytes_;

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
  // topics_mutex_ is a LEAF lock: it is held only to compute the
  // added/removed diff and swap in the new known_topics_ snapshot. Never
  // call middleware_ or session_manager_ (or acquire any other lock in this
  // class) while holding topics_mutex_ — release it before sending
  // notifications.
  // replays_mutex_ is likewise a LEAF lock: it only guards pending_replays_
  // map accesses. Never hold it while acquiring any other lock or calling
  // middleware_ — move the frames out, release, then send.
  std::mutex cleanup_mutex_;

  // Per-client per-topic last-sent timestamp (nanoseconds) for rate limiting
  std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> last_sent_times_;
  std::mutex last_sent_mutex_;

  // Whitelist-filtered topic name -> type snapshot, used by check_topic_changes()
  // to detect additions/removals/type-changes. LEAF lock (see comment above).
  std::unordered_map<std::string, std::string> known_topics_;
  bool topics_snapshot_taken_{false};
  std::mutex topics_mutex_;

  // Latched (transient_local) replay frames queued by handle_subscribe and
  // handle_resume (via collect_latched_replay), flushed by
  // process_single_request right AFTER the handler's response is sent — the
  // client must receive the response (which carries the topic's schema on
  // subscribe) before the binary frame that needs it. LEAF lock (see
  // comment above).
  std::unordered_map<std::string, std::vector<std::vector<uint8_t>>> pending_replays_;
  std::mutex replays_mutex_;
};

}  // namespace pj_bridge
