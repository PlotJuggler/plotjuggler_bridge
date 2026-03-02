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

#include <mutex>
#include <rclcpp/rclcpp.hpp>

#include "pj_bridge/subscription_manager_interface.hpp"
#include "pj_bridge_ros2/generic_subscription_manager.hpp"
#include "pj_bridge_ros2/message_stripper.hpp"

namespace pj_bridge {

/**
 * @brief ROS2 implementation of SubscriptionManagerInterface
 *
 * Wraps GenericSubscriptionManager to provide backend-agnostic subscription
 * management. Handles conversion from rclcpp::SerializedMessage to
 * shared_ptr<vector<byte>> and optional message stripping.
 */
class Ros2SubscriptionManager : public SubscriptionManagerInterface {
 public:
  explicit Ros2SubscriptionManager(rclcpp::Node::SharedPtr node, bool strip_large_messages = true);

  Ros2SubscriptionManager(const Ros2SubscriptionManager&) = delete;
  Ros2SubscriptionManager& operator=(const Ros2SubscriptionManager&) = delete;

  void set_message_callback(MessageCallback callback) override;
  bool subscribe(const std::string& topic_name, const std::string& topic_type) override;
  bool unsubscribe(const std::string& topic_name) override;
  void unsubscribe_all() override;

 private:
  rclcpp::Node::SharedPtr node_;
  GenericSubscriptionManager inner_manager_;
  bool strip_large_messages_;

  std::mutex callback_mutex_;
  MessageCallback callback_;
};

}  // namespace pj_bridge
