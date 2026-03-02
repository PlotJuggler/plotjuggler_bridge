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

#include "pj_bridge_ros2/message_stripper.hpp"

#include <nav_msgs/msg/occupancy_grid.hpp>
#include <rclcpp/serialization.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <stdexcept>
#include <unordered_set>

namespace pj_bridge {

namespace {

const std::unordered_set<std::string> kStrippableTypes = {
    "sensor_msgs/msg/Image",     "sensor_msgs/msg/CompressedImage", "sensor_msgs/msg/PointCloud2",
    "sensor_msgs/msg/LaserScan", "nav_msgs/msg/OccupancyGrid",
};

template <typename MsgT, typename StripFn>
rclcpp::SerializedMessage strip_and_reserialize(const rclcpp::SerializedMessage& input, StripFn strip_fn) {
  rclcpp::Serialization<MsgT> serializer;
  MsgT msg;
  serializer.deserialize_message(&input, &msg);
  strip_fn(msg);
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

}  // namespace

bool MessageStripper::should_strip(const std::string& message_type) {
  return kStrippableTypes.find(message_type) != kStrippableTypes.end();
}

rclcpp::SerializedMessage MessageStripper::strip(
    const std::string& message_type, const rclcpp::SerializedMessage& input) {
  if (message_type == "sensor_msgs/msg/Image") {
    return strip_and_reserialize<sensor_msgs::msg::Image>(input, [](auto& m) { m.data = {0}; });
  }
  if (message_type == "sensor_msgs/msg/CompressedImage") {
    return strip_and_reserialize<sensor_msgs::msg::CompressedImage>(input, [](auto& m) { m.data = {0}; });
  }
  if (message_type == "sensor_msgs/msg/PointCloud2") {
    return strip_and_reserialize<sensor_msgs::msg::PointCloud2>(input, [](auto& m) { m.data = {0}; });
  }
  if (message_type == "sensor_msgs/msg/LaserScan") {
    return strip_and_reserialize<sensor_msgs::msg::LaserScan>(input, [](auto& m) {
      m.ranges = {0.0f};
      m.intensities = {0.0f};
    });
  }
  if (message_type == "nav_msgs/msg/OccupancyGrid") {
    return strip_and_reserialize<nav_msgs::msg::OccupancyGrid>(input, [](auto& m) { m.data = {0}; });
  }

  throw std::runtime_error("strip() not implemented for type: " + message_type);
}

}  // namespace pj_bridge
