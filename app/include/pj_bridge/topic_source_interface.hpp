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

#include <string>
#include <vector>

namespace pj_bridge {

/// A discovered topic with its fully-qualified name and message type.
struct TopicInfo {
  std::string name;  ///< e.g. "/sensor/imu"
  std::string type;  ///< e.g. "sensor_msgs/msg/Imu"
};

/// Abstract interface for discovering topics and retrieving their schemas.
///
/// Implementations wrap backend-specific discovery mechanisms:
///   - ROS2: `Ros2TopicSource` uses rclcpp topic enumeration + .msg file parsing
///   - RTI:  `RtiTopicSource` uses DDS participant discovery + OMG IDL type info
///
/// BridgeServer calls get_topics() to list available topics for clients, and
/// get_schema() before subscribing to provide the client with the schema needed
/// to deserialize incoming messages.
class TopicSourceInterface {
 public:
  virtual ~TopicSourceInterface() = default;

  /// Return all currently available topics.
  /// May be called repeatedly (e.g. on each client "get_topics" request).
  virtual std::vector<TopicInfo> get_topics() = 0;

  /// Return the full schema definition for a topic.
  /// @param topic_name  fully-qualified topic name
  /// @return the schema text (e.g. concatenated .msg definitions for ROS2,
  ///         OMG IDL for RTI). An empty string is a VALID schema — some types
  ///         have legitimately empty definitions (e.g. std_msgs/msg/Empty).
  /// @throws std::exception when the schema cannot be resolved.
  virtual std::string get_schema(const std::string& topic_name) = 0;

  /// Return the encoding identifier for schemas produced by this source.
  /// Used in the subscribe response so clients know how to parse the schema.
  /// @return "ros2msg" for ROS2, "omgidl" for RTI DDS.
  virtual std::string schema_encoding() const = 0;

  /// True when the DISCOVERY layer knows every publisher of `topic_name`
  /// offers TRANSIENT_LOCAL durability — usable BEFORE any subscription
  /// exists (unlike SubscriptionManagerInterface::is_transient_local, which is
  /// only meaningful while subscribed). Drives the `latched: true` badge on
  /// get_topics / topics_changed entries. Backends without discovery-time QoS
  /// knowledge return false: the badge is then simply absent (absent = "not
  /// latched or unknown"), never a false claim.
  virtual bool is_transient_local(const std::string& /*topic_name*/) const {
    return false;
  }
};

}  // namespace pj_bridge
