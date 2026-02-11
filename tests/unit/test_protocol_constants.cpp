/*
 * Copyright (C) 2026 Davide Faconti
 *
 * This file is part of pj_ros_bridge.
 *
 * pj_ros_bridge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pj_ros_bridge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with pj_ros_bridge. If not, see <https://www.gnu.org/licenses/>.
 */

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
