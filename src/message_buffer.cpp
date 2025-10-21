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

#include "pj_ros_bridge/message_buffer.hpp"

namespace pj_ros_bridge {

MessageBuffer::MessageBuffer() : last_read_timestamp_ns_(get_current_time_ns()) {}

void MessageBuffer::add_message(
    const std::string& topic_name, uint64_t publish_timestamp_ns, uint64_t receive_timestamp_ns,
    const std::vector<uint8_t>& data) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Cleanup old messages first
  cleanup_old_messages();

  // Add the new message
  BufferedMessage msg;
  msg.topic_name = topic_name;
  msg.publish_timestamp_ns = publish_timestamp_ns;
  msg.receive_timestamp_ns = receive_timestamp_ns;
  msg.data = data;

  topic_buffers_[topic_name].push_back(std::move(msg));
}

std::vector<BufferedMessage> MessageBuffer::get_new_messages() {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<BufferedMessage> new_messages;

  // Collect messages received after last read
  for (auto& [topic, buffer] : topic_buffers_) {
    for (const auto& msg : buffer) {
      if (msg.receive_timestamp_ns > last_read_timestamp_ns_) {
        new_messages.push_back(msg);
      }
    }
  }

  // Update last read timestamp
  last_read_timestamp_ns_ = get_current_time_ns();

  return new_messages;
}

std::vector<BufferedMessage> MessageBuffer::get_new_messages(const std::string& topic_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<BufferedMessage> new_messages;

  auto it = topic_buffers_.find(topic_name);
  if (it != topic_buffers_.end()) {
    for (const auto& msg : it->second) {
      if (msg.receive_timestamp_ns > last_read_timestamp_ns_) {
        new_messages.push_back(msg);
      }
    }
  }

  // Update last read timestamp
  last_read_timestamp_ns_ = get_current_time_ns();

  return new_messages;
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
      if (current_time - oldest_msg.receive_timestamp_ns > kMaxMessageAgeNs) {
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

uint64_t MessageBuffer::get_current_time_ns() {
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

}  // namespace pj_ros_bridge
