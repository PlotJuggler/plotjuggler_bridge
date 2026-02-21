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

#include "pj_ros_bridge/topic_discovery.hpp"

namespace pj_ros_bridge {

TopicDiscovery::TopicDiscovery(rclcpp::Node::SharedPtr node) : node_(node) {}

std::vector<TopicInfo> TopicDiscovery::discover_topics() {
  topics_.clear();

  auto topic_names_and_types = node_->get_topic_names_and_types();

  for (const auto& [topic_name, topic_types] : topic_names_and_types) {
    // Filter system topics
    if (should_filter_topic(topic_name)) {
      continue;
    }

    // A topic can have multiple types in some cases, we'll use the first one
    if (!topic_types.empty()) {
      TopicInfo info;
      info.name = topic_name;
      info.type = topic_types[0];
      topics_.push_back(info);
    }
  }

  return topics_;
}

std::vector<TopicInfo> TopicDiscovery::get_topics() const {
  return topics_;
}

bool TopicDiscovery::refresh() {
  try {
    discover_topics();
    return true;
  } catch (const std::exception& e) {
    RCLCPP_ERROR(node_->get_logger(), "Topic discovery refresh failed: %s", e.what());
    return false;
  }
}

bool TopicDiscovery::should_filter_topic(const std::string& topic_name) const {
  // Filter out ROS2 system topics
  static const std::vector<std::string> kSystemTopics = {"/rosout", "/parameter_events", "/robot_description"};

  for (const auto& system_topic : kSystemTopics) {
    if (topic_name == system_topic) {
      return true;
    }
  }

  // Filter topics that start with system prefixes
  if (topic_name.find("/_") == 0) {
    return true;
  }

  return false;
}

}  // namespace pj_ros_bridge
