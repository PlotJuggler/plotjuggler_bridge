// Copyright 2025
// ROS2 Bridge - Protocol Constants

#pragma once

#include <cstddef>
#include <cstdint>

namespace pj_ros_bridge {

/// Protocol version number, included in all JSON responses
static constexpr int kProtocolVersion = 1;

/// Magic bytes for binary frame header: "PJRB" in little-endian
static constexpr uint32_t kBinaryFrameMagic = 0x42524A50;

/// Size of the binary frame header in bytes
static constexpr size_t kBinaryHeaderSize = 16;

/// Schema encoding identifier for ROS2 message definitions
static constexpr const char* kSchemaEncodingRos2Msg = "ros2msg";

}  // namespace pj_ros_bridge
