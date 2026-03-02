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

#include "pj_bridge_ros2/ros2_topic_source.hpp"

#include "pj_bridge/protocol_constants.hpp"

namespace pj_bridge {

Ros2TopicSource::Ros2TopicSource(rclcpp::Node::SharedPtr node) : topic_discovery_(node) {}

std::vector<TopicInfo> Ros2TopicSource::get_topics() {
  auto topics = topic_discovery_.discover_topics();

  // Update the name → type cache
  topic_type_cache_.clear();
  for (const auto& topic : topics) {
    topic_type_cache_[topic.name] = topic.type;
  }

  return topics;
}

std::string Ros2TopicSource::get_schema(const std::string& topic_name) {
  auto it = topic_type_cache_.find(topic_name);
  if (it == topic_type_cache_.end()) {
    return "";
  }

  return schema_extractor_.get_message_definition(it->second);
}

std::string Ros2TopicSource::schema_encoding() const {
  return kSchemaEncodingRos2Msg;
}

}  // namespace pj_bridge
