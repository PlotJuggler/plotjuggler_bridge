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

#include <gtest/gtest.h>

#include <nav_msgs/msg/occupancy_grid.hpp>
#include <rclcpp/serialization.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

#include "pj_ros_bridge/message_stripper.hpp"

using namespace pj_ros_bridge;

// ============================================================================
// should_strip() tests
// ============================================================================

TEST(MessageStripperTest, ShouldStripReturnsTrueForImage) {
  EXPECT_TRUE(MessageStripper::should_strip("sensor_msgs/msg/Image"));
}

TEST(MessageStripperTest, ShouldStripReturnsTrueForCompressedImage) {
  EXPECT_TRUE(MessageStripper::should_strip("sensor_msgs/msg/CompressedImage"));
}

TEST(MessageStripperTest, ShouldStripReturnsTrueForPointCloud2) {
  EXPECT_TRUE(MessageStripper::should_strip("sensor_msgs/msg/PointCloud2"));
}

TEST(MessageStripperTest, ShouldStripReturnsTrueForLaserScan) {
  EXPECT_TRUE(MessageStripper::should_strip("sensor_msgs/msg/LaserScan"));
}

TEST(MessageStripperTest, ShouldStripReturnsTrueForOccupancyGrid) {
  EXPECT_TRUE(MessageStripper::should_strip("nav_msgs/msg/OccupancyGrid"));
}

TEST(MessageStripperTest, ShouldStripReturnsFalseForUnknownType) {
  EXPECT_FALSE(MessageStripper::should_strip("std_msgs/msg/String"));
  EXPECT_FALSE(MessageStripper::should_strip("geometry_msgs/msg/Pose"));
  EXPECT_FALSE(MessageStripper::should_strip("sensor_msgs/msg/Imu"));
  EXPECT_FALSE(MessageStripper::should_strip("unknown/msg/Type"));
}
