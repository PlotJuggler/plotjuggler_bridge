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

}  // namespace

bool MessageStripper::should_strip(const std::string& message_type) {
  return kStrippableTypes.find(message_type) != kStrippableTypes.end();
}

rclcpp::SerializedMessage MessageStripper::strip(
    const std::string& message_type, const rclcpp::SerializedMessage& input) {
  // TODO: Implement strip functions for each message type
  (void)message_type;
  (void)input;
  throw std::runtime_error("strip() not yet implemented");
}

}  // namespace pj_ros_bridge
