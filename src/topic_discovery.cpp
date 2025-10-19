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

#include "pj_ros_bridge/topic_discovery.hpp"

namespace pj_ros_bridge
{

TopicDiscovery::TopicDiscovery(rclcpp::Node::SharedPtr node)
: node_(node)
{
}

std::vector<TopicInfo> TopicDiscovery::discover_topics()
{
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

std::vector<TopicInfo> TopicDiscovery::get_topics() const
{
  return topics_;
}

bool TopicDiscovery::refresh()
{
  try {
    discover_topics();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool TopicDiscovery::should_filter_topic(const std::string& topic_name) const
{
  // Filter out ROS2 system topics
  static const std::vector<std::string> kSystemTopics = {
    "/rosout",
    "/parameter_events",
    "/robot_description"
  };

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
