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

#include <gtest/gtest.h>

#include <nav_msgs/msg/occupancy_grid.hpp>
#include <rclcpp/serialization.hpp>
#include <sensor_msgs/msg/compressed_image.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

#include "pj_bridge_ros2/message_stripper.hpp"

using namespace pj_bridge;

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

// ============================================================================
// strip() tests
// ============================================================================

// Helper to serialize a ROS2 message
template <typename T>
rclcpp::SerializedMessage serialize_message(const T& msg) {
  rclcpp::Serialization<T> serializer;
  rclcpp::SerializedMessage serialized_msg;
  serializer.serialize_message(&msg, &serialized_msg);
  return serialized_msg;
}

// Helper to deserialize a ROS2 message
template <typename T>
T deserialize_message(const rclcpp::SerializedMessage& serialized_msg) {
  rclcpp::Serialization<T> serializer;
  T msg;
  serializer.deserialize_message(&serialized_msg, &msg);
  return msg;
}

TEST(MessageStripperTest, StripImageReplacesDataWithSentinel) {
  // Create an Image with large data
  sensor_msgs::msg::Image img;
  img.header.stamp.sec = 12345;
  img.header.stamp.nanosec = 67890;
  img.header.frame_id = "camera_frame";
  img.height = 480;
  img.width = 640;
  img.encoding = "rgb8";
  img.is_bigendian = 0;
  img.step = 640 * 3;
  img.data.resize(640 * 480 * 3, 0xAB);  // ~921KB of data

  // Serialize
  auto serialized = serialize_message(img);
  EXPECT_GT(serialized.size(), 900000);  // Should be large

  // Strip
  auto stripped = MessageStripper::strip("sensor_msgs/msg/Image", serialized);

  // Deserialize stripped message
  auto result = deserialize_message<sensor_msgs::msg::Image>(stripped);

  // Verify metadata preserved
  EXPECT_EQ(result.header.stamp.sec, 12345);
  EXPECT_EQ(result.header.stamp.nanosec, 67890);
  EXPECT_EQ(result.header.frame_id, "camera_frame");
  EXPECT_EQ(result.height, 480u);
  EXPECT_EQ(result.width, 640u);
  EXPECT_EQ(result.encoding, "rgb8");
  EXPECT_EQ(result.is_bigendian, 0u);
  EXPECT_EQ(result.step, 640u * 3);

  // Verify data stripped to sentinel
  ASSERT_EQ(result.data.size(), 1u);
  EXPECT_EQ(result.data[0], 0);

  // Verify serialized size is much smaller
  EXPECT_LT(stripped.size(), 1000);
}

TEST(MessageStripperTest, StripCompressedImageReplacesDataWithSentinel) {
  sensor_msgs::msg::CompressedImage img;
  img.header.stamp.sec = 11111;
  img.header.frame_id = "compressed_frame";
  img.format = "jpeg";
  img.data.resize(50000, 0xCD);  // 50KB JPEG data

  auto serialized = serialize_message(img);
  EXPECT_GT(serialized.size(), 49000);

  auto stripped = MessageStripper::strip("sensor_msgs/msg/CompressedImage", serialized);
  auto result = deserialize_message<sensor_msgs::msg::CompressedImage>(stripped);

  EXPECT_EQ(result.header.stamp.sec, 11111);
  EXPECT_EQ(result.header.frame_id, "compressed_frame");
  EXPECT_EQ(result.format, "jpeg");
  ASSERT_EQ(result.data.size(), 1u);
  EXPECT_EQ(result.data[0], 0);
  EXPECT_LT(stripped.size(), 500);
}

TEST(MessageStripperTest, StripPointCloud2ReplacesDataWithSentinel) {
  sensor_msgs::msg::PointCloud2 cloud;
  cloud.header.stamp.sec = 22222;
  cloud.header.frame_id = "lidar_frame";
  cloud.height = 1;
  cloud.width = 10000;
  cloud.is_bigendian = false;
  cloud.point_step = 16;
  cloud.row_step = 160000;
  cloud.is_dense = true;

  // Add a point field
  sensor_msgs::msg::PointField field;
  field.name = "x";
  field.offset = 0;
  field.datatype = sensor_msgs::msg::PointField::FLOAT32;
  field.count = 1;
  cloud.fields.push_back(field);

  cloud.data.resize(160000, 0xEF);  // 160KB of point data

  auto serialized = serialize_message(cloud);
  EXPECT_GT(serialized.size(), 159000);

  auto stripped = MessageStripper::strip("sensor_msgs/msg/PointCloud2", serialized);
  auto result = deserialize_message<sensor_msgs::msg::PointCloud2>(stripped);

  EXPECT_EQ(result.header.stamp.sec, 22222);
  EXPECT_EQ(result.header.frame_id, "lidar_frame");
  EXPECT_EQ(result.height, 1u);
  EXPECT_EQ(result.width, 10000u);
  EXPECT_EQ(result.point_step, 16u);
  EXPECT_EQ(result.row_step, 160000u);
  EXPECT_EQ(result.is_dense, true);
  ASSERT_EQ(result.fields.size(), 1u);
  EXPECT_EQ(result.fields[0].name, "x");
  ASSERT_EQ(result.data.size(), 1u);
  EXPECT_EQ(result.data[0], 0);
  EXPECT_LT(stripped.size(), 500);
}

TEST(MessageStripperTest, StripLaserScanReplacesRangesAndIntensitiesWithSentinel) {
  sensor_msgs::msg::LaserScan scan;
  scan.header.stamp.sec = 33333;
  scan.header.frame_id = "laser_frame";
  scan.angle_min = -1.57f;
  scan.angle_max = 1.57f;
  scan.angle_increment = 0.01f;
  scan.time_increment = 0.0001f;
  scan.scan_time = 0.1f;
  scan.range_min = 0.1f;
  scan.range_max = 30.0f;
  scan.ranges.resize(1000, 5.0f);         // 4KB
  scan.intensities.resize(1000, 100.0f);  // 4KB

  auto serialized = serialize_message(scan);
  EXPECT_GT(serialized.size(), 7000);

  auto stripped = MessageStripper::strip("sensor_msgs/msg/LaserScan", serialized);
  auto result = deserialize_message<sensor_msgs::msg::LaserScan>(stripped);

  EXPECT_EQ(result.header.stamp.sec, 33333);
  EXPECT_EQ(result.header.frame_id, "laser_frame");
  EXPECT_FLOAT_EQ(result.angle_min, -1.57f);
  EXPECT_FLOAT_EQ(result.angle_max, 1.57f);
  EXPECT_FLOAT_EQ(result.angle_increment, 0.01f);
  EXPECT_FLOAT_EQ(result.range_min, 0.1f);
  EXPECT_FLOAT_EQ(result.range_max, 30.0f);

  // Both ranges and intensities should be stripped
  ASSERT_EQ(result.ranges.size(), 1u);
  EXPECT_FLOAT_EQ(result.ranges[0], 0.0f);
  ASSERT_EQ(result.intensities.size(), 1u);
  EXPECT_FLOAT_EQ(result.intensities[0], 0.0f);

  EXPECT_LT(stripped.size(), 500);
}

TEST(MessageStripperTest, StripOccupancyGridReplacesDataWithSentinel) {
  nav_msgs::msg::OccupancyGrid grid;
  grid.header.stamp.sec = 44444;
  grid.header.frame_id = "map_frame";
  grid.info.resolution = 0.05f;
  grid.info.width = 1000;
  grid.info.height = 1000;
  grid.info.origin.position.x = -25.0;
  grid.info.origin.position.y = -25.0;
  grid.data.resize(1000000, 50);  // 1MB of map data

  auto serialized = serialize_message(grid);
  EXPECT_GT(serialized.size(), 999000);

  auto stripped = MessageStripper::strip("nav_msgs/msg/OccupancyGrid", serialized);
  auto result = deserialize_message<nav_msgs::msg::OccupancyGrid>(stripped);

  EXPECT_EQ(result.header.stamp.sec, 44444);
  EXPECT_EQ(result.header.frame_id, "map_frame");
  EXPECT_FLOAT_EQ(result.info.resolution, 0.05f);
  EXPECT_EQ(result.info.width, 1000u);
  EXPECT_EQ(result.info.height, 1000u);
  EXPECT_DOUBLE_EQ(result.info.origin.position.x, -25.0);

  ASSERT_EQ(result.data.size(), 1u);
  EXPECT_EQ(result.data[0], 0);

  EXPECT_LT(stripped.size(), 500);
}
