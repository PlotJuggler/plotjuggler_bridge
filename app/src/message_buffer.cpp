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

#include "pj_bridge/message_buffer.hpp"

#include "pj_bridge/time_utils.hpp"

namespace pj_bridge {

MessageBuffer::MessageBuffer(uint64_t max_message_age_ns) : max_message_age_ns_(max_message_age_ns) {}

void MessageBuffer::add_message(
    const std::string& topic_name, uint64_t timestamp_ns, std::shared_ptr<std::vector<std::byte>> data) {
  std::lock_guard<std::mutex> lock(mutex_);

  cleanup_old_messages();

  BufferedMessage msg;
  msg.timestamp_ns = timestamp_ns;
  msg.received_at_ns = get_current_time_ns();
  msg.data = std::move(data);

  topic_buffers_[topic_name].push_back(std::move(msg));
}

void MessageBuffer::move_messages(std::unordered_map<std::string, std::deque<BufferedMessage>>& out_messages) {
  std::lock_guard<std::mutex> lock(mutex_);
  out_messages = std::move(topic_buffers_);
  topic_buffers_.clear();
}

void MessageBuffer::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  topic_buffers_.clear();
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

  for (auto& [topic, buffer] : topic_buffers_) {
    while (!buffer.empty()) {
      const auto& oldest_msg = buffer.front();
      if (current_time - oldest_msg.received_at_ns > max_message_age_ns_) {
        buffer.pop_front();
      } else {
        break;
      }
    }
  }

  for (auto it = topic_buffers_.begin(); it != topic_buffers_.end();) {
    if (it->second.empty()) {
      it = topic_buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace pj_bridge
