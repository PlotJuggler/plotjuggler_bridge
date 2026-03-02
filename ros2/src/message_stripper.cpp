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

rclcpp::SerializedMessage strip_image(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::Image> serializer;
  sensor_msgs::msg::Image msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

rclcpp::SerializedMessage strip_compressed_image(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::CompressedImage> serializer;
  sensor_msgs::msg::CompressedImage msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

rclcpp::SerializedMessage strip_pointcloud2(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::PointCloud2> serializer;
  sensor_msgs::msg::PointCloud2 msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

rclcpp::SerializedMessage strip_laser_scan(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::LaserScan> serializer;
  sensor_msgs::msg::LaserScan msg;
  serializer.deserialize_message(&input, &msg);
  msg.ranges = {0.0f};
  msg.intensities = {0.0f};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

rclcpp::SerializedMessage strip_occupancy_grid(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<nav_msgs::msg::OccupancyGrid> serializer;
  nav_msgs::msg::OccupancyGrid msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
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
    return strip_image(input);
  }
  if (message_type == "sensor_msgs/msg/CompressedImage") {
    return strip_compressed_image(input);
  }
  if (message_type == "sensor_msgs/msg/PointCloud2") {
    return strip_pointcloud2(input);
  }
  if (message_type == "sensor_msgs/msg/LaserScan") {
    return strip_laser_scan(input);
  }
  if (message_type == "nav_msgs/msg/OccupancyGrid") {
    return strip_occupancy_grid(input);
  }

  throw std::runtime_error("strip() not implemented for type: " + message_type);
}

}  // namespace pj_bridge
