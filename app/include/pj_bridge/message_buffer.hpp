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

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pj_bridge {

/// A single message held in the buffer, waiting to be aggregated and sent.
struct BufferedMessage {
  uint64_t timestamp_ns;    ///< Source-clock timestamp from the middleware
  uint64_t received_at_ns;  ///< Wall-clock time when add_message() was called (used for TTL cleanup)
  std::shared_ptr<std::vector<std::byte>> data;  ///< CDR-encoded message payload
};

/// Thread-safe per-topic message buffer with time-based auto-cleanup.
///
/// Messages arrive via add_message() (called from the SubscriptionManager callback)
/// and are drained by move_messages() (called from BridgeServer::publish_aggregated_messages()).
///
/// Stale messages older than max_message_age_ns are automatically removed on each
/// add_message() call. Cleanup uses received_at_ns (wall-clock), not timestamp_ns,
/// so sim-time offsets don't cause premature eviction or unbounded growth.
///
/// Thread safety: all public methods are mutex-protected. add_message() may be
/// called concurrently with move_messages() from different threads.
class MessageBuffer {
 public:
  static constexpr uint64_t kDefaultMaxMessageAgeNs = 1'000'000'000;  ///< 1 second

  /// @param max_message_age_ns  TTL for buffered messages (default 1 second).
  explicit MessageBuffer(uint64_t max_message_age_ns = kDefaultMaxMessageAgeNs);

  /// Add a message to the buffer for the given topic.
  /// Triggers cleanup of stale messages before inserting.
  /// @param topic_name    fully-qualified topic name
  /// @param timestamp_ns  source-clock timestamp from the middleware
  /// @param data          CDR-encoded message payload (shared ownership)
  void add_message(const std::string& topic_name, uint64_t timestamp_ns, std::shared_ptr<std::vector<std::byte>> data);

  /// Atomically drain all buffered messages into @p out_messages.
  /// After this call the internal buffer is empty. Messages are appended to any
  /// existing entries in out_messages (keyed by topic name).
  void move_messages(std::unordered_map<std::string, std::deque<BufferedMessage>>& out_messages);

  /// Discard all buffered messages across all topics.
  void clear();

  /// Return the total number of messages across all topics.
  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::deque<BufferedMessage>> topic_buffers_;
  uint64_t max_message_age_ns_;

  void cleanup_old_messages();
};

}  // namespace pj_bridge
