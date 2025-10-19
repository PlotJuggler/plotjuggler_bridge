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

#ifndef PJ_ROS_BRIDGE__GENERIC_SUBSCRIPTION_MANAGER_HPP_
#define PJ_ROS_BRIDGE__GENERIC_SUBSCRIPTION_MANAGER_HPP_

#include <rclcpp/rclcpp.hpp>
#include <rclcpp/generic_subscription.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pj_ros_bridge
{

/**
 * @brief Callback function for received messages
 *
 * Parameters: topic_name, serialized_data, receive_timestamp_ns
 */
using MessageCallback = std::function<void(
  const std::string&,
  const std::shared_ptr<rclcpp::SerializedMessage>&,
  uint64_t)>;

/**
 * @brief Manages generic subscriptions to ROS2 topics
 *
 * Handles subscription lifecycle with reference counting to support
 * multiple clients subscribing to the same topic.
 *
 * Thread safety: All public methods are thread-safe
 */
class GenericSubscriptionManager
{
public:
  explicit GenericSubscriptionManager(rclcpp::Node::SharedPtr node);

  /**
   * @brief Subscribe to a topic
   *
   * If already subscribed, increments reference count.
   *
   * @param topic_name Topic name to subscribe to
   * @param topic_type Full topic type (e.g., "std_msgs/msg/String")
   * @param callback Callback function for received messages
   * @return true if subscription created/incremented, false on error
   */
  bool subscribe(
    const std::string& topic_name,
    const std::string& topic_type,
    MessageCallback callback);

  /**
   * @brief Unsubscribe from a topic
   *
   * Decrements reference count. Removes subscription when count reaches zero.
   *
   * @param topic_name Topic name to unsubscribe from
   * @return true if unsubscribed, false if not subscribed
   */
  bool unsubscribe(const std::string& topic_name);

  /**
   * @brief Check if subscribed to a topic
   *
   * @param topic_name Topic name to check
   * @return true if subscribed, false otherwise
   */
  bool is_subscribed(const std::string& topic_name) const;

  /**
   * @brief Get reference count for a topic
   *
   * @param topic_name Topic name
   * @return Reference count, or 0 if not subscribed
   */
  size_t get_reference_count(const std::string& topic_name) const;

  /**
   * @brief Unsubscribe from all topics
   */
  void unsubscribe_all();

private:
  struct SubscriptionInfo
  {
    std::shared_ptr<rclcpp::GenericSubscription> subscription;
    std::string topic_type;
    MessageCallback callback;
    size_t reference_count;
  };

  rclcpp::Node::SharedPtr node_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, SubscriptionInfo> subscriptions_;

  /**
   * @brief Get current time in nanoseconds since epoch
   */
  static uint64_t get_current_time_ns();
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__GENERIC_SUBSCRIPTION_MANAGER_HPP_
