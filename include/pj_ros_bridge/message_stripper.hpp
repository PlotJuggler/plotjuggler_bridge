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

#ifndef PJ_ROS_BRIDGE__MESSAGE_STRIPPER_HPP_
#define PJ_ROS_BRIDGE__MESSAGE_STRIPPER_HPP_

#include <rclcpp/rclcpp.hpp>
#include <string>

namespace pj_ros_bridge {

/**
 * @brief Strips large array fields from known message types to reduce bandwidth
 *
 * Supported message types:
 * - sensor_msgs/msg/Image: strips `data` field
 * - sensor_msgs/msg/CompressedImage: strips `data` field
 * - sensor_msgs/msg/PointCloud2: strips `data` field
 * - sensor_msgs/msg/LaserScan: strips `ranges` and `intensities` fields
 * - nav_msgs/msg/OccupancyGrid: strips `data` field
 *
 * Stripped fields are replaced with a single sentinel byte [0] to indicate
 * the field was intentionally removed.
 *
 * Thread safety: Thread-safe, all methods are stateless
 */
class MessageStripper {
 public:
  /**
   * @brief Check if a message type should be stripped
   * @param message_type Full type name (e.g., "sensor_msgs/msg/Image")
   * @return true if this type has fields that should be stripped
   */
  static bool should_strip(const std::string& message_type);

  /**
   * @brief Strip large fields from a serialized message
   *
   * Deserializes the message, replaces large array fields with [0] sentinel,
   * and re-serializes to CDR format.
   *
   * @param message_type Full type name (e.g., "sensor_msgs/msg/Image")
   * @param input CDR-serialized message from ROS2
   * @return New SerializedMessage with large fields replaced by [0] sentinel
   * @throws std::runtime_error if deserialization or serialization fails
   */
  static rclcpp::SerializedMessage strip(const std::string& message_type, const rclcpp::SerializedMessage& input);
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MESSAGE_STRIPPER_HPP_
