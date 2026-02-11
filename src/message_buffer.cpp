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

#include "pj_ros_bridge/message_buffer.hpp"

#include "pj_ros_bridge/time_utils.hpp"

namespace pj_ros_bridge {

MessageBuffer::MessageBuffer() : last_read_timestamp_ns_(get_current_time_ns()) {}

void MessageBuffer::add_message(
    const std::string& topic_name, uint64_t timestamp_ns, std::shared_ptr<rclcpp::SerializedMessage> serialized_msg) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Cleanup old messages first
  cleanup_old_messages();

  // Add the new message
  BufferedMessage msg;
  msg.timestamp_ns = timestamp_ns;
  msg.msg = std::move(serialized_msg);

  topic_buffers_[topic_name].push_back(std::move(msg));
}

void MessageBuffer::move_messages(std::unordered_map<std::string, std::deque<BufferedMessage>>& out_messages) {
  std::lock_guard<std::mutex> lock(mutex_);
  out_messages = std::move(topic_buffers_);
  topic_buffers_.clear();
  last_read_timestamp_ns_ = get_current_time_ns();
}

void MessageBuffer::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  topic_buffers_.clear();
  last_read_timestamp_ns_ = get_current_time_ns();
}

size_t MessageBuffer::size() const {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t total = 0;
  for (const auto& [topic, buffer] : topic_buffers_) {
    total += buffer.size();
  }

  return total;
}

void MessageBuffer::cleanup_old_messages() {
  uint64_t current_time = get_current_time_ns();

  // Remove old messages from all topic buffers
  for (auto& [topic, buffer] : topic_buffers_) {
    // Remove messages older than kMaxMessageAgeNs
    while (!buffer.empty()) {
      const auto& oldest_msg = buffer.front();
      if (current_time - oldest_msg.timestamp_ns > kMaxMessageAgeNs) {
        buffer.pop_front();
      } else {
        // Messages are ordered by time, so we can stop here
        break;
      }
    }
  }

  // Remove empty topic buffers
  for (auto it = topic_buffers_.begin(); it != topic_buffers_.end();) {
    if (it->second.empty()) {
      it = topic_buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace pj_ros_bridge
