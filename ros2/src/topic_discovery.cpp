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

#include "pj_bridge_ros2/topic_discovery.hpp"

namespace pj_bridge {

TopicDiscovery::TopicDiscovery(rclcpp::Node::SharedPtr node) : node_(node) {}

std::vector<TopicInfo> TopicDiscovery::discover_topics() {
  std::vector<TopicInfo> topics;
  auto topic_names_and_types = node_->get_topic_names_and_types();

  for (const auto& [topic_name, topic_types] : topic_names_and_types) {
    if (should_filter_topic(topic_name)) {
      continue;
    }

    if (!topic_types.empty()) {
      TopicInfo info;
      info.name = topic_name;
      info.type = topic_types[0];
      topics.push_back(info);
    }
  }

  return topics;
}

bool TopicDiscovery::should_filter_topic(const std::string& topic_name) const {
  static const std::vector<std::string> kSystemTopics = {"/rosout", "/parameter_events", "/robot_description"};

  for (const auto& system_topic : kSystemTopics) {
    if (topic_name == system_topic) {
      return true;
    }
  }

  if (topic_name.find("/_") == 0) {
    return true;
  }

  return false;
}

}  // namespace pj_bridge
