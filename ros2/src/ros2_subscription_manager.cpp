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

#include "pj_bridge_ros2/ros2_subscription_manager.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <optional>

namespace pj_bridge {

Ros2SubscriptionManager::Ros2SubscriptionManager(rclcpp::Node::SharedPtr node, bool strip_large_messages)
    : inner_manager_(node), strip_large_messages_(strip_large_messages) {}

void Ros2SubscriptionManager::set_message_callback(MessageCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  callback_ = std::move(callback);
}

bool Ros2SubscriptionManager::subscribe(const std::string& topic_name, const std::string& topic_type) {
  bool needs_stripping = strip_large_messages_ && MessageStripper::should_strip(topic_type);

  Ros2MessageCallback ros2_callback =
      [this, topic_type, needs_stripping](
          const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg, uint64_t receive_time_ns) {
        const rclcpp::SerializedMessage* msg_to_use = msg.get();
        std::optional<rclcpp::SerializedMessage> stripped_msg;

        if (needs_stripping) {
          try {
            stripped_msg.emplace(MessageStripper::strip(topic_type, *msg));
            msg_to_use = &*stripped_msg;
          } catch (const std::exception& e) {
            spdlog::warn("Failed to strip message on topic '{}': {}. Forwarding original.", topic, e.what());
          }
        }

        const auto& rcl_msg = msg_to_use->get_rcl_serialized_message();
        auto data = std::make_shared<std::vector<std::byte>>(rcl_msg.buffer_length);
        std::memcpy(data->data(), rcl_msg.buffer, rcl_msg.buffer_length);

        MessageCallback cb;
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          cb = callback_;
        }
        if (cb) {
          cb(topic, std::move(data), receive_time_ns);
        }
      };

  return inner_manager_.subscribe(topic_name, topic_type, ros2_callback);
}

bool Ros2SubscriptionManager::unsubscribe(const std::string& topic_name) {
  return inner_manager_.unsubscribe(topic_name);
}

void Ros2SubscriptionManager::unsubscribe_all() {
  inner_manager_.unsubscribe_all();
}

}  // namespace pj_bridge
