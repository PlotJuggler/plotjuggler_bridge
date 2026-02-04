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

#include "pj_ros_bridge/message_stripper.hpp"

#include <nav_msgs/msg/occupancy_grid.hpp>
#include <rclcpp/serialization.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <stdexcept>
#include <unordered_set>

namespace pj_ros_bridge {

namespace {

// Set of message types that should be stripped
const std::unordered_set<std::string> kStrippableTypes = {
    "sensor_msgs/msg/Image",     "sensor_msgs/msg/CompressedImage", "sensor_msgs/msg/PointCloud2",
    "sensor_msgs/msg/LaserScan", "nav_msgs/msg/OccupancyGrid",
};

// Strip function for sensor_msgs/msg/Image
rclcpp::SerializedMessage strip_image(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::Image> serializer;

  // Deserialize
  sensor_msgs::msg::Image msg;
  serializer.deserialize_message(&input, &msg);

  // Replace data with sentinel
  msg.data = {0};

  // Re-serialize
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

// Strip function for sensor_msgs/msg/CompressedImage
rclcpp::SerializedMessage strip_compressed_image(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::CompressedImage> serializer;
  sensor_msgs::msg::CompressedImage msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

// Strip function for sensor_msgs/msg/PointCloud2
rclcpp::SerializedMessage strip_pointcloud2(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::PointCloud2> serializer;
  sensor_msgs::msg::PointCloud2 msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}

// Strip function for sensor_msgs/msg/LaserScan
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

// Strip function for nav_msgs/msg/OccupancyGrid
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

}  // namespace pj_ros_bridge
