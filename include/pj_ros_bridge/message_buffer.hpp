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

#ifndef PJ_ROS_BRIDGE__MESSAGE_BUFFER_HPP_
#define PJ_ROS_BRIDGE__MESSAGE_BUFFER_HPP_

#include <chrono>
#include <deque>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace pj_ros_bridge {

/**
 * @brief Structure to hold a buffered message with zero-copy semantics
 *
 * Uses shared_ptr to avoid copying message data. The same SerializedMessage
 * can be referenced by multiple BufferedMessage instances without duplication.
 */
struct BufferedMessage {
  uint64_t timestamp_ns;                           ///< Receive timestamp in nanoseconds since epoch
  std::shared_ptr<rclcpp::SerializedMessage> msg;  ///< Shared pointer to serialized ROS2 message (zero-copy)
};

/**
 * @brief Thread-safe message buffer with zero-copy and move semantics
 *
 * Design principles:
 * - Zero-copy: Messages stored as shared_ptr to avoid data duplication
 * - Move semantics: Buffer ownership transferred via std::swap() in move_messages()
 * - Auto-cleanup: Messages older than 1 second are automatically removed
 * - Thread-safe: All public methods use mutex protection
 *
 * The buffer stores messages per topic and uses std::swap() for efficient
 * ownership transfer, avoiding any message copying during read operations.
 */
class MessageBuffer {
 public:
  MessageBuffer();

  /**
   * @brief Add a message to the buffer (zero-copy)
   *
   * The message is stored as a shared_ptr, avoiding any data copying.
   * Automatically cleans up old messages before adding.
   *
   * @param topic_name Topic name
   * @param timestamp_ns Receive timestamp in nanoseconds since epoch
   * @param serialized_msg Shared pointer to serialized ROS2 message (ownership shared, not transferred)
   */
  void add_message(
      const std::string& topic_name, uint64_t timestamp_ns, std::shared_ptr<rclcpp::SerializedMessage> serialized_msg);

  /**
   * @brief Move entire buffer out atomically (zero-copy via std::swap)
   *
   * This method uses std::swap() to transfer buffer ownership without copying.
   * After the call:
   * - out_messages contains all buffered messages grouped by topic
   * - Internal buffer is cleared
   * - last_read_timestamp is updated to current time
   *
   * @param out_messages Output parameter that receives the buffer contents via swap
   */
  void move_messages(std::unordered_map<std::string, std::deque<BufferedMessage>>& out_messages);

  /**
   * @brief Clear all buffered messages
   */
  void clear();

  /**
   * @brief Get the number of buffered messages
   *
   * @return Total number of messages currently in buffer
   */
  size_t size() const;

 private:
  mutable std::mutex mutex_;

  // Buffer per topic
  std::unordered_map<std::string, std::deque<BufferedMessage>> topic_buffers_;

  // Timestamp of last read operation
  uint64_t last_read_timestamp_ns_;

  // Maximum age of messages in nanoseconds (1 second)
  static constexpr uint64_t kMaxMessageAgeNs = 1'000'000'000;

  /**
   * @brief Remove messages older than kMaxMessageAgeNs
   *
   * Must be called with mutex_ locked.
   */
  void cleanup_old_messages();
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MESSAGE_BUFFER_HPP_
