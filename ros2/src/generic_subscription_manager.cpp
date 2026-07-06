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

#include "pj_bridge_ros2/generic_subscription_manager.hpp"

#include <algorithm>

#include "pj_bridge/time_utils.hpp"

namespace pj_bridge {

GenericSubscriptionManager::GenericSubscriptionManager(
    rclcpp::Node::SharedPtr node, size_t min_qos_depth, size_t max_qos_depth)
    : node_(node), min_qos_depth_(min_qos_depth), max_qos_depth_(max_qos_depth) {}

rclcpp::QoS GenericSubscriptionManager::adapt_qos(const std::string& topic_name) const {
  // Match the QoS the publishers actually offer (same policy as rosbag2):
  // a RELIABLE subscription never matches a BEST_EFFORT publisher (sensor
  // topics!) and a VOLATILE one misses latched (TRANSIENT_LOCAL) samples.
  rclcpp::QoS qos(100);

  auto publishers = node_->get_publishers_info_by_topic(topic_name);
  if (publishers.empty()) {
    qos.keep_last(std::min<size_t>(100, max_qos_depth_));
    return qos;
  }

  bool any_best_effort = false;
  bool all_transient_local = true;
  // Depth aggregation adapted from foxglove_bridge (MIT License,
  // Copyright (c) Foxglove Technologies Inc):
  // sum the publishers' history depths so a burst from every publisher
  // still fits, then clamp to [min_qos_depth, max_qos_depth].
  size_t total_depth = 0;
  for (const auto& info : publishers) {
    const auto& profile = info.qos_profile();
    if (profile.reliability() == rclcpp::ReliabilityPolicy::BestEffort) {
      any_best_effort = true;
    }
    if (profile.durability() != rclcpp::DurabilityPolicy::TransientLocal) {
      all_transient_local = false;
    }
    total_depth += profile.depth();
  }
  qos.keep_last(std::clamp(total_depth, min_qos_depth_, max_qos_depth_));

  // BEST_EFFORT matches both kinds of publisher; TRANSIENT_LOCAL only
  // matches if every publisher offers it.
  if (any_best_effort) {
    qos.best_effort();
  }
  if (all_transient_local) {
    qos.transient_local();
  }

  return qos;
}

bool GenericSubscriptionManager::subscribe(
    const std::string& topic_name, const std::string& topic_type, Ros2MessageCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);

  if (it != subscriptions_.end()) {
    it->second.reference_count++;
    return true;
  }

  try {
    auto sub_callback = [topic_name, callback](std::shared_ptr<rclcpp::SerializedMessage> msg) {
      uint64_t receive_time = get_current_time_ns();
      callback(topic_name, msg, receive_time);
    };

    auto subscription = node_->create_generic_subscription(topic_name, topic_type, adapt_qos(topic_name), sub_callback);

    subscriptions_[topic_name] = SubscriptionInfo{subscription, 1};

    return true;
  } catch (const std::exception& e) {
    RCLCPP_ERROR(
        node_->get_logger(), "Failed to create subscription for topic '%s' (type '%s'): %s", topic_name.c_str(),
        topic_type.c_str(), e.what());
    return false;
  }
}

bool GenericSubscriptionManager::unsubscribe(const std::string& topic_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);
  if (it == subscriptions_.end()) {
    return false;
  }

  if (it->second.reference_count == 0) {
    return false;
  }

  it->second.reference_count--;

  if (it->second.reference_count == 0) {
    subscriptions_.erase(it);
  }

  return true;
}

bool GenericSubscriptionManager::is_subscribed(const std::string& topic_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subscriptions_.find(topic_name) != subscriptions_.end();
}

size_t GenericSubscriptionManager::get_reference_count(const std::string& topic_name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);
  if (it != subscriptions_.end()) {
    return it->second.reference_count;
  }

  return 0;
}

void GenericSubscriptionManager::unsubscribe_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  subscriptions_.clear();
}

}  // namespace pj_bridge
