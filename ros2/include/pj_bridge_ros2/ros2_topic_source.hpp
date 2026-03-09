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
#include <unordered_map>

#include "pj_bridge/topic_source_interface.hpp"
#include "pj_bridge_ros2/schema_extractor.hpp"
#include "pj_bridge_ros2/topic_discovery.hpp"

namespace pj_bridge {

/**
 * @brief ROS2 implementation of TopicSourceInterface
 *
 * Wraps TopicDiscovery and SchemaExtractor to provide backend-agnostic
 * topic discovery and schema extraction.
 *
 * Thread safety: NOT thread-safe. All calls must be serialized externally
 * (BridgeServer calls get_topics() and get_schema() from the same event-loop thread).
 */
class Ros2TopicSource : public TopicSourceInterface {
 public:
  explicit Ros2TopicSource(rclcpp::Node::SharedPtr node);

  Ros2TopicSource(const Ros2TopicSource&) = delete;
  Ros2TopicSource& operator=(const Ros2TopicSource&) = delete;

  std::vector<TopicInfo> get_topics() override;
  std::string get_schema(const std::string& topic_name) override;
  std::string schema_encoding() const override;

 private:
  TopicDiscovery topic_discovery_;
  SchemaExtractor schema_extractor_;

  // Cached name → type mapping from last get_topics() call
  std::unordered_map<std::string, std::string> topic_type_cache_;
};

}  // namespace pj_bridge
