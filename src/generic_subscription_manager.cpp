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

#include "pj_ros_bridge/generic_subscription_manager.hpp"

#include <chrono>

namespace pj_ros_bridge {

GenericSubscriptionManager::GenericSubscriptionManager(rclcpp::Node::SharedPtr node) : node_(node) {}

bool GenericSubscriptionManager::subscribe(
    const std::string& topic_name, const std::string& topic_type, MessageCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);

  if (it != subscriptions_.end()) {
    // Already subscribed - increment reference count
    it->second.reference_count++;
    // Update callback to the new one
    it->second.callback = callback;
    return true;
  }

  // Create new subscription
  try {
    auto sub_callback = [topic_name, callback](std::shared_ptr<rclcpp::SerializedMessage> msg) {
      uint64_t receive_time = get_current_time_ns();
      callback(topic_name, msg, receive_time);
    };

    auto subscription = node_->create_generic_subscription(topic_name, topic_type, rclcpp::QoS(10), sub_callback);

    SubscriptionInfo info;
    info.subscription = subscription;
    info.topic_type = topic_type;
    info.callback = callback;
    info.reference_count = 1;

    subscriptions_[topic_name] = std::move(info);

    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool GenericSubscriptionManager::unsubscribe(const std::string& topic_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);
  if (it == subscriptions_.end()) {
    return false;  // Not subscribed
  }

  // Decrement reference count
  it->second.reference_count--;

  // Remove subscription if no more references
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

uint64_t GenericSubscriptionManager::get_current_time_ns() {
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

}  // namespace pj_ros_bridge
