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
