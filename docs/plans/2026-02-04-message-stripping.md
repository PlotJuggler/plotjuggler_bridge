# Message Stripping Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Strip large array fields (data, ranges, intensities) from Image, PointCloud2, LaserScan, and similar message types before buffering to reduce memory and bandwidth.

**Architecture:** New `MessageStripper` class with static methods that deserialize known large message types using ROS2 type support, replace bulk arrays with sentinel byte `[0]`, and re-serialize. Called from subscription callback before buffering. Controlled by `strip_large_messages` ROS2 parameter (default: true).

**Tech Stack:** C++17, ROS2 Humble, sensor_msgs, nav_msgs, rclcpp serialization

---

## Task 1: Add nav_msgs Test Dependency

**Files:**
- Modify: `package.xml:19-22`

**Step 1: Add nav_msgs to package.xml**

Add `nav_msgs` to test dependencies after `geometry_msgs`:

```xml
  <test_depend>ament_cmake_gtest</test_depend>
  <test_depend>sensor_msgs</test_depend>
  <test_depend>geometry_msgs</test_depend>
  <test_depend>nav_msgs</test_depend>
```

**Step 2: Verify package.xml is valid**

Run: `cd ~/ws_plotjuggler/src/pj_ros_bridge && xmllint --noout package.xml && echo "XML valid"`
Expected: `XML valid`

**Step 3: Commit**

```bash
git add package.xml
git commit -m "build: Add nav_msgs test dependency for MessageStripper"
```

---

## Task 2: Update CMakeLists.txt for New Dependencies

**Files:**
- Modify: `CMakeLists.txt`

**Step 1: Add sensor_msgs and nav_msgs find_package in test block**

After line 134 (`find_package(ament_cmake_gtest REQUIRED)`), add:

```cmake
  find_package(sensor_msgs REQUIRED)
  find_package(nav_msgs REQUIRED)
```

**Step 2: Add message_stripper.cpp to library sources**

In the `add_library` block (around line 72), add `src/message_stripper.cpp`:

```cmake
add_library(${PROJECT_NAME}_lib
  src/middleware/websocket_middleware.cpp
  src/topic_discovery.cpp
  src/schema_extractor.cpp
  src/message_buffer.cpp
  src/generic_subscription_manager.cpp
  src/session_manager.cpp
  src/message_serializer.cpp
  src/message_stripper.cpp
  src/bridge_server.cpp
)
```

**Step 3: Add sensor_msgs and nav_msgs as library dependencies**

Add to `ament_target_dependencies(${PROJECT_NAME}_lib ...)`:

```cmake
ament_target_dependencies(${PROJECT_NAME}_lib
  ament_index_cpp
  rclcpp
  sensor_msgs
  nav_msgs
)
```

**Step 4: Add test_message_stripper.cpp to test sources**

In the `ament_add_gtest` block (around line 137), add:

```cmake
  ament_add_gtest(${PROJECT_NAME}_tests
    tests/unit/test_websocket_middleware.cpp
    tests/unit/test_topic_discovery.cpp
    tests/unit/test_schema_extractor.cpp
    tests/unit/test_message_buffer.cpp
    tests/unit/test_generic_subscription_manager.cpp
    tests/unit/test_session_manager.cpp
    tests/unit/test_message_serializer.cpp
    tests/unit/test_message_stripper.cpp
    tests/unit/test_bridge_server.cpp
    tests/unit/test_protocol_constants.cpp
  )
```

**Step 5: Add sensor_msgs and nav_msgs to test dependencies**

Add to `ament_target_dependencies(${PROJECT_NAME}_tests ...)`:

```cmake
  ament_target_dependencies(${PROJECT_NAME}_tests
    rclcpp
    sensor_msgs
    nav_msgs
  )
```

**Step 6: Verify CMake configuration**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release 2>&1 | head -50`
Expected: Build should start (may fail due to missing source files, that's expected)

**Step 7: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: Add sensor_msgs/nav_msgs deps and message_stripper sources"
```

---

## Task 3: Create MessageStripper Header

**Files:**
- Create: `include/pj_ros_bridge/message_stripper.hpp`

**Step 1: Write the header file**

```cpp
// Copyright 2025 Davide Faconti
//
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
  static rclcpp::SerializedMessage strip(
      const std::string& message_type, const rclcpp::SerializedMessage& input);
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MESSAGE_STRIPPER_HPP_
```

**Step 2: Verify header compiles**

Run: `cd ~/ws_plotjuggler/src/pj_ros_bridge && source /opt/ros/humble/setup.bash && g++ -std=c++17 -I include -I /opt/ros/humble/include/rclcpp -I /opt/ros/humble/include/rcl -I /opt/ros/humble/include/rcutils -I /opt/ros/humble/include/rmw -I /opt/ros/humble/include/rcl_yaml_param_parser -I /opt/ros/humble/include/rosidl_runtime_c -I /opt/ros/humble/include/rosidl_typesupport_interface -I /opt/ros/humble/include/rcpputils -I /opt/ros/humble/include/builtin_interfaces -I /opt/ros/humble/include/rosidl_runtime_cpp -I /opt/ros/humble/include/tracetools -I /opt/ros/humble/include/statistics_msgs -I /opt/ros/humble/include/libstatistics_collector -I /opt/ros/humble/include/ament_index_cpp -fsyntax-only include/pj_ros_bridge/message_stripper.hpp && echo "Header OK"`
Expected: `Header OK`

**Step 3: Commit**

```bash
git add include/pj_ros_bridge/message_stripper.hpp
git commit -m "feat: Add MessageStripper header"
```

---

## Task 4: Write Failing Tests for should_strip()

**Files:**
- Create: `tests/unit/test_message_stripper.cpp`

**Step 1: Write the test file with should_strip tests**

```cpp
// Copyright 2025 Davide Faconti
//
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
```

**Step 2: Commit test file**

```bash
git add tests/unit/test_message_stripper.cpp
git commit -m "test: Add failing tests for MessageStripper::should_strip"
```

---

## Task 5: Implement should_strip() Method

**Files:**
- Create: `src/message_stripper.cpp`

**Step 1: Create implementation file with should_strip**

```cpp
// Copyright 2025 Davide Faconti
//
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
    "sensor_msgs/msg/Image",
    "sensor_msgs/msg/CompressedImage",
    "sensor_msgs/msg/PointCloud2",
    "sensor_msgs/msg/LaserScan",
    "nav_msgs/msg/OccupancyGrid",
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
```

**Step 2: Build and run tests**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release && colcon test --packages-select pj_ros_bridge --ctest-args -R MessageStripper && colcon test-result --verbose`
Expected: All `ShouldStrip*` tests PASS

**Step 3: Commit**

```bash
git add src/message_stripper.cpp
git commit -m "feat: Implement MessageStripper::should_strip"
```

---

## Task 6: Write Failing Tests for strip() - Image

**Files:**
- Modify: `tests/unit/test_message_stripper.cpp`

**Step 1: Add strip test for Image**

Append to test file:

```cpp
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
```

**Step 2: Verify test fails**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge --ctest-args -R StripImage && colcon test-result --verbose`
Expected: FAIL with "strip() not yet implemented"

**Step 3: Commit**

```bash
git add tests/unit/test_message_stripper.cpp
git commit -m "test: Add failing test for MessageStripper::strip(Image)"
```

---

## Task 7: Implement strip() for Image

**Files:**
- Modify: `src/message_stripper.cpp`

**Step 1: Add strip_image helper function**

Replace the `strip()` implementation with:

```cpp
namespace {

// ... existing kStrippableTypes ...

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

}  // namespace

// ... should_strip() unchanged ...

rclcpp::SerializedMessage MessageStripper::strip(
    const std::string& message_type, const rclcpp::SerializedMessage& input) {
  if (message_type == "sensor_msgs/msg/Image") {
    return strip_image(input);
  }

  throw std::runtime_error("strip() not implemented for type: " + message_type);
}
```

**Step 2: Build and run test**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge --ctest-args -R StripImage && colcon test-result --verbose`
Expected: PASS

**Step 3: Commit**

```bash
git add src/message_stripper.cpp
git commit -m "feat: Implement MessageStripper::strip for Image"
```

---

## Task 8: Add strip() Tests and Implementation for CompressedImage

**Files:**
- Modify: `tests/unit/test_message_stripper.cpp`
- Modify: `src/message_stripper.cpp`

**Step 1: Add test for CompressedImage**

Append to test file:

```cpp
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
```

**Step 2: Add strip_compressed_image function and update strip()**

In `src/message_stripper.cpp`, add:

```cpp
rclcpp::SerializedMessage strip_compressed_image(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::CompressedImage> serializer;
  sensor_msgs::msg::CompressedImage msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}
```

Update `strip()`:

```cpp
rclcpp::SerializedMessage MessageStripper::strip(
    const std::string& message_type, const rclcpp::SerializedMessage& input) {
  if (message_type == "sensor_msgs/msg/Image") {
    return strip_image(input);
  }
  if (message_type == "sensor_msgs/msg/CompressedImage") {
    return strip_compressed_image(input);
  }

  throw std::runtime_error("strip() not implemented for type: " + message_type);
}
```

**Step 3: Build and run tests**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge --ctest-args -R MessageStripper && colcon test-result --verbose`
Expected: All tests PASS

**Step 4: Commit**

```bash
git add tests/unit/test_message_stripper.cpp src/message_stripper.cpp
git commit -m "feat: Implement MessageStripper::strip for CompressedImage"
```

---

## Task 9: Add strip() Tests and Implementation for PointCloud2

**Files:**
- Modify: `tests/unit/test_message_stripper.cpp`
- Modify: `src/message_stripper.cpp`

**Step 1: Add test for PointCloud2**

Append to test file:

```cpp
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
```

**Step 2: Add strip_pointcloud2 function and update strip()**

```cpp
rclcpp::SerializedMessage strip_pointcloud2(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<sensor_msgs::msg::PointCloud2> serializer;
  sensor_msgs::msg::PointCloud2 msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}
```

Update `strip()` to add:

```cpp
  if (message_type == "sensor_msgs/msg/PointCloud2") {
    return strip_pointcloud2(input);
  }
```

**Step 3: Build and run tests**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge --ctest-args -R MessageStripper && colcon test-result --verbose`
Expected: All tests PASS

**Step 4: Commit**

```bash
git add tests/unit/test_message_stripper.cpp src/message_stripper.cpp
git commit -m "feat: Implement MessageStripper::strip for PointCloud2"
```

---

## Task 10: Add strip() Tests and Implementation for LaserScan

**Files:**
- Modify: `tests/unit/test_message_stripper.cpp`
- Modify: `src/message_stripper.cpp`

**Step 1: Add test for LaserScan**

Append to test file:

```cpp
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
  scan.ranges.resize(1000, 5.0f);        // 4KB
  scan.intensities.resize(1000, 100.0f); // 4KB

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
```

**Step 2: Add strip_laser_scan function and update strip()**

```cpp
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
```

Update `strip()` to add:

```cpp
  if (message_type == "sensor_msgs/msg/LaserScan") {
    return strip_laser_scan(input);
  }
```

**Step 3: Build and run tests**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge --ctest-args -R MessageStripper && colcon test-result --verbose`
Expected: All tests PASS

**Step 4: Commit**

```bash
git add tests/unit/test_message_stripper.cpp src/message_stripper.cpp
git commit -m "feat: Implement MessageStripper::strip for LaserScan"
```

---

## Task 11: Add strip() Tests and Implementation for OccupancyGrid

**Files:**
- Modify: `tests/unit/test_message_stripper.cpp`
- Modify: `src/message_stripper.cpp`

**Step 1: Add test for OccupancyGrid**

Append to test file:

```cpp
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
```

**Step 2: Add strip_occupancy_grid function and update strip()**

```cpp
rclcpp::SerializedMessage strip_occupancy_grid(const rclcpp::SerializedMessage& input) {
  rclcpp::Serialization<nav_msgs::msg::OccupancyGrid> serializer;
  nav_msgs::msg::OccupancyGrid msg;
  serializer.deserialize_message(&input, &msg);
  msg.data = {0};
  rclcpp::SerializedMessage output;
  serializer.serialize_message(&msg, &output);
  return output;
}
```

Update `strip()` to add:

```cpp
  if (message_type == "nav_msgs/msg/OccupancyGrid") {
    return strip_occupancy_grid(input);
  }
```

**Step 3: Build and run tests**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge --ctest-args -R MessageStripper && colcon test-result --verbose`
Expected: All tests PASS

**Step 4: Commit**

```bash
git add tests/unit/test_message_stripper.cpp src/message_stripper.cpp
git commit -m "feat: Implement MessageStripper::strip for OccupancyGrid"
```

---

## Task 12: Integrate MessageStripper into BridgeServer

**Files:**
- Modify: `include/pj_ros_bridge/bridge_server.hpp`
- Modify: `src/bridge_server.cpp`

**Step 1: Add strip_large_messages_ member to bridge_server.hpp**

After line 121 (`mutable std::mutex stats_mutex_;`), add:

```cpp
  // Message stripping configuration
  bool strip_large_messages_;
```

**Step 2: Add include for message_stripper.hpp in bridge_server.cpp**

After line 10 (`#include "pj_ros_bridge/message_serializer.hpp"`), add:

```cpp
#include "pj_ros_bridge/message_stripper.hpp"
```

**Step 3: Initialize strip_large_messages_ in constructor**

In constructor initializer list (around line 26), add after `total_bytes_published_(0)`:

```cpp
      total_bytes_published_(0),
      strip_large_messages_(true) {}
```

**Step 4: Update GenericSubscriptionManager callback in handle_subscribe**

In `handle_subscribe()`, the callback is created around line 256. Modify it to include stripping.

Find this code block (lines 253-258):

```cpp
    // Create callback to add messages to buffer
    auto callback = [this](
                        const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg,
                        uint64_t receive_time_ns) { message_buffer_->add_message(topic, receive_time_ns, msg); };
```

Replace with:

```cpp
    // Create callback to add messages to buffer (with optional stripping)
    auto callback = [this, topic_type](
                        const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg,
                        uint64_t receive_time_ns) {
      if (strip_large_messages_ && MessageStripper::should_strip(topic_type)) {
        auto stripped = MessageStripper::strip(topic_type, *msg);
        auto stripped_ptr = std::make_shared<rclcpp::SerializedMessage>(std::move(stripped));
        message_buffer_->add_message(topic, receive_time_ns, stripped_ptr);
      } else {
        message_buffer_->add_message(topic, receive_time_ns, msg);
      }
    };
```

**Step 5: Update callback in handle_resume (same pattern)**

In `handle_resume()`, find the callback around line 493:

```cpp
  // Create callback for message buffer (same as in handle_subscribe)
  auto callback = [this](
                      const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg,
                      uint64_t receive_time_ns) { message_buffer_->add_message(topic, receive_time_ns, msg); };
```

This callback doesn't have `topic_type` in scope, so we need a different approach. Since the type is looked up inside the loop, modify the loop:

Replace lines 497-510:

```cpp
  auto subs = session_manager_->get_subscriptions(client_id);
  for (const auto& [topic, rate] : subs) {
    auto type_it = topic_types.find(topic);
    if (type_it != topic_types.end()) {
      subscription_manager_->subscribe(topic, type_it->second, callback);
```

With:

```cpp
  auto subs = session_manager_->get_subscriptions(client_id);
  for (const auto& [topic, rate] : subs) {
    auto type_it = topic_types.find(topic);
    if (type_it != topic_types.end()) {
      std::string topic_type = type_it->second;
      auto callback = [this, topic_type](
                          const std::string& t, const std::shared_ptr<rclcpp::SerializedMessage>& msg,
                          uint64_t receive_time_ns) {
        if (strip_large_messages_ && MessageStripper::should_strip(topic_type)) {
          auto stripped = MessageStripper::strip(topic_type, *msg);
          auto stripped_ptr = std::make_shared<rclcpp::SerializedMessage>(std::move(stripped));
          message_buffer_->add_message(t, receive_time_ns, stripped_ptr);
        } else {
          message_buffer_->add_message(t, receive_time_ns, msg);
        }
      };
      subscription_manager_->subscribe(topic, topic_type, callback);
```

And remove the old callback definition from before the loop.

**Step 6: Build and run all tests**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 7: Commit**

```bash
git add include/pj_ros_bridge/bridge_server.hpp src/bridge_server.cpp
git commit -m "feat: Integrate MessageStripper into BridgeServer subscription callback"
```

---

## Task 13: Add ROS2 Parameter for strip_large_messages

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/bridge_server.cpp`
- Modify: `include/pj_ros_bridge/bridge_server.hpp`

**Step 1: Read main.cpp to understand parameter handling**

Check how other parameters are declared and passed.

**Step 2: Add strip_large_messages parameter to BridgeServer constructor**

In `bridge_server.hpp`, update constructor signature (around line 43):

```cpp
  explicit BridgeServer(
      std::shared_ptr<rclcpp::Node> node, std::shared_ptr<MiddlewareInterface> middleware, int port = 8080,
      double session_timeout = 10.0, double publish_rate = 50.0, bool strip_large_messages = true);
```

In `bridge_server.cpp`, update constructor (around line 17):

```cpp
BridgeServer::BridgeServer(
    std::shared_ptr<rclcpp::Node> node, std::shared_ptr<MiddlewareInterface> middleware, int port,
    double session_timeout, double publish_rate, bool strip_large_messages)
    : node_(node),
      middleware_(middleware),
      port_(port),
      session_timeout_(session_timeout),
      publish_rate_(publish_rate),
      initialized_(false),
      total_messages_published_(0),
      total_bytes_published_(0),
      strip_large_messages_(strip_large_messages) {}
```

**Step 3: Update main.cpp to declare and pass the parameter**

Read main.cpp first, then add parameter declaration and pass to BridgeServer.

**Step 4: Build and test**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests PASS

**Step 5: Commit**

```bash
git add include/pj_ros_bridge/bridge_server.hpp src/bridge_server.cpp src/main.cpp
git commit -m "feat: Add strip_large_messages ROS2 parameter (default: true)"
```

---

## Task 14: Run Full Test Suite and Format Code

**Files:**
- All modified files

**Step 1: Format all code**

Run: `cd ~/ws_plotjuggler/src/pj_ros_bridge && pre-commit run -a`
Expected: All checks pass (or files reformatted)

**Step 2: Build with ASAN to check for memory issues**

Run: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge --build-base build_asan --install-base install_asan --cmake-args -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=ON && ASAN_OPTIONS="new_delete_type_mismatch=0" LSAN_OPTIONS="suppressions=src/pj_ros_bridge/asan_suppressions.txt" build_asan/pj_ros_bridge/pj_ros_bridge_tests`
Expected: All tests pass, no ASAN errors

**Step 3: Run regular tests**

Run: `cd ~/ws_plotjuggler && colcon build --packages-select pj_ros_bridge && colcon test --packages-select pj_ros_bridge && colcon test-result --verbose`
Expected: All tests pass

**Step 4: Commit any formatting changes**

```bash
git add -A
git commit -m "style: Format code with pre-commit"
```

---

## Task 15: Update Documentation

**Files:**
- Modify: `CLAUDE.md`

**Step 1: Update CLAUDE.md with new feature**

Add to "Recent changes" section:

```markdown
- Added MessageStripper to strip large array fields from Image, PointCloud2, LaserScan, OccupancyGrid
- New ROS2 parameter `strip_large_messages` (default: true)
```

Update test count if changed.

**Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: Document message stripping feature"
```

---

## Summary

**Total Tasks**: 15

**New Files**:
- `include/pj_ros_bridge/message_stripper.hpp`
- `src/message_stripper.cpp`
- `tests/unit/test_message_stripper.cpp`

**Modified Files**:
- `package.xml`
- `CMakeLists.txt`
- `include/pj_ros_bridge/bridge_server.hpp`
- `src/bridge_server.cpp`
- `src/main.cpp`
- `CLAUDE.md`

**Dependencies Added**:
- `sensor_msgs` (runtime + test)
- `nav_msgs` (runtime + test)
