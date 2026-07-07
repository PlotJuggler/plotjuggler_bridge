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

#include <rclcpp/rclcpp.hpp>
#include <string>
#include <vector>

#include "pj_bridge/topic_source_interface.hpp"

namespace pj_bridge {

/**
 * @brief Discovers and manages ROS2 topics
 *
 * Uses the ROS2 API to discover available topics and filter
 * system topics that should not be exposed to clients.
 *
 * Thread safety: Not thread-safe. External synchronization required.
 */
class TopicDiscovery {
 public:
  explicit TopicDiscovery(rclcpp::Node::SharedPtr node);

  /**
   * @brief Discover all available ROS2 topics
   * @return Vector of discovered topics (excluding system topics)
   */
  std::vector<TopicInfo> discover_topics();

  /**
   * @brief Whether every discovered publisher of the topic offers
   * TRANSIENT_LOCAL durability (the same rule the subscription manager's
   * adapt_qos applies at subscribe time, evaluated here WITHOUT subscribing —
   * a live graph query, so it works for topics no client has requested yet).
   * False when the topic has no publishers.
   */
  bool is_transient_local(const std::string& topic_name) const;

 private:
  rclcpp::Node::SharedPtr node_;

  bool should_filter_topic(const std::string& topic_name) const;
};

}  // namespace pj_bridge
