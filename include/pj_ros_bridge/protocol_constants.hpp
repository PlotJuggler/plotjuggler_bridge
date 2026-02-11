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
