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

#include <functional>
#include <memory>
#include <mutex>
#include <rclcpp/generic_subscription.hpp>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <unordered_map>

namespace pj_bridge {

/**
 * @brief ROS2-specific callback for received serialized messages
 *
 * Parameters: topic_name, serialized_data, receive_timestamp_ns
 */
using Ros2MessageCallback =
    std::function<void(const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t)>;

/**
 * @brief Manages generic subscriptions to ROS2 topics
 *
 * Handles subscription lifecycle with reference counting to support
 * multiple clients subscribing to the same topic.
 *
 * Thread safety: All public methods are thread-safe
 */
class GenericSubscriptionManager {
 public:
  explicit GenericSubscriptionManager(rclcpp::Node::SharedPtr node);

  bool subscribe(const std::string& topic_name, const std::string& topic_type, Ros2MessageCallback callback);
  bool unsubscribe(const std::string& topic_name);
  bool is_subscribed(const std::string& topic_name) const;
  size_t get_reference_count(const std::string& topic_name) const;
  void unsubscribe_all();

 private:
  struct SubscriptionInfo {
    std::shared_ptr<rclcpp::GenericSubscription> subscription;
    size_t reference_count;
  };

  rclcpp::Node::SharedPtr node_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, SubscriptionInfo> subscriptions_;
};

}  // namespace pj_bridge
