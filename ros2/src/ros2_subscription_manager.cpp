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

namespace pj_bridge {

Ros2SubscriptionManager::Ros2SubscriptionManager(
    rclcpp::Node::SharedPtr node, std::shared_ptr<TransformSet> transforms, size_t min_qos_depth, size_t max_qos_depth)
    : inner_manager_(node, min_qos_depth, max_qos_depth), transforms_(std::move(transforms)) {}

void Ros2SubscriptionManager::set_message_callback(MessageCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  callback_ = std::move(callback);
}

bool Ros2SubscriptionManager::subscribe(const std::string& topic_name, const std::string& topic_type) {
  // A transformed topic is advertised with its output type; subscribe with the real one.
  std::shared_ptr<BoundTransform> bound = transforms_ ? transforms_->find(topic_name) : nullptr;
  const std::string& source_type = bound ? bound->source_type : topic_type;

  Ros2MessageCallback ros2_callback =
      [this, bound](
          const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg, uint64_t receive_time_ns) {
        const auto& rcl_msg = msg->get_rcl_serialized_message();
        const auto* bytes = reinterpret_cast<const std::byte*>(rcl_msg.buffer);

        std::shared_ptr<std::vector<std::byte>> data;
        if (bound) {
          data = std::make_shared<std::vector<std::byte>>();
          if (!bound->run(topic, {bytes, rcl_msg.buffer_length}, *data)) {
            return;  // dropped and counted; never forward the untransformed bytes
          }
        } else {
          data = std::make_shared<std::vector<std::byte>>(bytes, bytes + rcl_msg.buffer_length);
        }

        MessageCallback cb;
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          cb = callback_;
        }
        if (cb) {
          cb(topic, std::move(data), receive_time_ns);
        }
      };

  return inner_manager_.subscribe(topic_name, source_type, ros2_callback);
}

bool Ros2SubscriptionManager::unsubscribe(const std::string& topic_name) {
  return inner_manager_.unsubscribe(topic_name);
}

size_t Ros2SubscriptionManager::subscription_count() const {
  return inner_manager_.subscription_count();
}

void Ros2SubscriptionManager::unsubscribe_all() {
  inner_manager_.unsubscribe_all();
}

bool Ros2SubscriptionManager::is_transient_local(const std::string& topic_name) const {
  return inner_manager_.is_transient_local(topic_name);
}

bool Ros2SubscriptionManager::is_subscribed(const std::string& topic_name) const {
  return inner_manager_.is_subscribed(topic_name);
}

}  // namespace pj_bridge
