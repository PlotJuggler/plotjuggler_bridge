// Copyright 2025 Davide Faconti
//
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
 * @brief Structure to hold a buffered message
 */
struct BufferedMessage {
  std::string topic_name;
  uint64_t publish_timestamp_ns;  // Nanoseconds since epoch
  uint64_t receive_timestamp_ns;  // Nanoseconds since epoch
  std::vector<uint8_t> data;
};

/**
 * @brief Thread-safe message buffer with automatic cleanup
 *
 * Stores messages per topic with automatic removal of messages
 * older than 1 second to prevent unbounded memory growth.
 *
 * Thread safety: All public methods are thread-safe
 */
class MessageBuffer {
 public:
  MessageBuffer();

  /**
   * @brief Add a message to the buffer
   *
   * Automatically cleans up old messages before adding.
   *
   * @param topic_name Topic name
   * @param publish_timestamp_ns Publish timestamp in nanoseconds since epoch
   * @param receive_timestamp_ns Receive timestamp in nanoseconds since epoch
   * @param data Serialized message data
   */
  void add_message(
      const std::string& topic_name, uint64_t publish_timestamp_ns, uint64_t receive_timestamp_ns,
      const std::vector<uint8_t>& data);

  /**
   * @brief Get all messages received since last read
   *
   * @return Vector of messages received since last get_new_messages() call
   */
  std::vector<BufferedMessage> get_new_messages();

  /**
   * @brief Get new messages for a specific topic
   *
   * @param topic_name Topic name to filter by
   * @return Vector of messages for the specified topic
   */
  std::vector<BufferedMessage> get_new_messages(const std::string& topic_name);

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

  /**
   * @brief Get current time in nanoseconds since epoch
   */
  static uint64_t get_current_time_ns();
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MESSAGE_BUFFER_HPP_
