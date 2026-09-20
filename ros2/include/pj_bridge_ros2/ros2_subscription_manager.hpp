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

#include <memory>
#include <mutex>
#include <rclcpp/rclcpp.hpp>

#include "pj_bridge/subscription_manager_interface.hpp"
#include "pj_bridge/transform_set.hpp"
#include "pj_bridge_ros2/generic_subscription_manager.hpp"

namespace pj_bridge {

/**
 * @brief ROS2 implementation of SubscriptionManagerInterface
 *
 * Wraps GenericSubscriptionManager to provide backend-agnostic subscription
 * management. Converts rclcpp::SerializedMessage to shared_ptr<vector<byte>>
 * and applies the topic's transform, if any, on the rcl buffer before copying.
 */
class Ros2SubscriptionManager : public SubscriptionManagerInterface {
 public:
  explicit Ros2SubscriptionManager(
      rclcpp::Node::SharedPtr node, std::shared_ptr<TransformSet> transforms = nullptr, size_t min_qos_depth = 1,
      size_t max_qos_depth = 100);

  Ros2SubscriptionManager(const Ros2SubscriptionManager&) = delete;
  Ros2SubscriptionManager& operator=(const Ros2SubscriptionManager&) = delete;

  void set_message_callback(MessageCallback callback) override;
  bool subscribe(const std::string& topic_name, const std::string& topic_type) override;
  bool unsubscribe(const std::string& topic_name) override;
  void unsubscribe_all() override;
  bool is_transient_local(const std::string& topic_name) const override;
  bool is_subscribed(const std::string& topic_name) const override;
  size_t subscription_count() const;

 private:
  GenericSubscriptionManager inner_manager_;
  std::shared_ptr<TransformSet> transforms_;

  std::mutex callback_mutex_;
  MessageCallback callback_;
};

}  // namespace pj_bridge
