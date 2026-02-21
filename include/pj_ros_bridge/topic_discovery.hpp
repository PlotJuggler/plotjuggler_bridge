/*
 * Copyright (C) 2026 Davide Faconti
 *
 * This file is part of pj_ros_bridge.
 *
 * pj_ros_bridge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pj_ros_bridge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with pj_ros_bridge. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef PJ_ROS_BRIDGE__TOPIC_DISCOVERY_HPP_
#define PJ_ROS_BRIDGE__TOPIC_DISCOVERY_HPP_

#include <map>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <vector>

namespace pj_ros_bridge {

/**
 * @brief Structure to hold topic information
 */
struct TopicInfo {
  std::string name;
  std::string type;
};

/**
 * @brief Discovers and manages ROS2 topics
 *
 * This class uses the ROS2 API to discover available topics and filter
 * system topics that should not be exposed to clients.
 *
 * Thread safety: Not thread-safe. External synchronization required.
 */
class TopicDiscovery {
 public:
  explicit TopicDiscovery(rclcpp::Node::SharedPtr node);

  /**
   * @brief Discover all available ROS2 topics
   *
   * @return Vector of discovered topics (excluding system topics)
   */
  std::vector<TopicInfo> discover_topics();

  /**
   * @brief Get cached topic information
   *
   * @return Vector of previously discovered topics
   */
  std::vector<TopicInfo> get_topics() const;

  /**
   * @brief Refresh the topic list
   *
   * @return true if successful, false otherwise
   */
  bool refresh();

 private:
  rclcpp::Node::SharedPtr node_;
  std::vector<TopicInfo> topics_;

  /**
   * @brief Check if a topic should be filtered out
   *
   * @param topic_name Topic name to check
   * @return true if topic should be filtered, false otherwise
   */
  bool should_filter_topic(const std::string& topic_name) const;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__TOPIC_DISCOVERY_HPP_
