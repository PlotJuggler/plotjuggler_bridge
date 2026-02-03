// Copyright 2025
// ROS2 Bridge - Protocol Constants Tests

#include <gtest/gtest.h>

#include <cstring>

#include "pj_ros_bridge/protocol_constants.hpp"

namespace pj_ros_bridge {
namespace {

TEST(ProtocolConstantsTest, ProtocolVersionIsOne) {
  EXPECT_EQ(kProtocolVersion, 1);
}

TEST(ProtocolConstantsTest, BinaryFrameMagicIsPJRB) {
  // "PJRB" as little-endian uint32_t: 'P'=0x50, 'J'=0x4A, 'R'=0x52, 'B'=0x42
  // Little-endian: 0x42524A50
  EXPECT_EQ(kBinaryFrameMagic, 0x42524A50);

  // Verify it spells "PJRB" when read as bytes
  const char* magic_bytes = reinterpret_cast<const char*>(&kBinaryFrameMagic);
  EXPECT_EQ(magic_bytes[0], 'P');
  EXPECT_EQ(magic_bytes[1], 'J');
  EXPECT_EQ(magic_bytes[2], 'R');
  EXPECT_EQ(magic_bytes[3], 'B');
}

TEST(ProtocolConstantsTest, BinaryHeaderSizeIs16) {
  EXPECT_EQ(kBinaryHeaderSize, 16u);
}

TEST(ProtocolConstantsTest, SchemaEncodingIsRos2Msg) {
  EXPECT_STREQ(kSchemaEncodingRos2Msg, "ros2msg");
}

}  // namespace
}  // namespace pj_ros_bridge
