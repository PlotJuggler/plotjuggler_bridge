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

#pragma once

#include <rclcpp/rclcpp.hpp>
#include <string>

namespace pj_bridge {

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
 * Thread safety: Thread-safe, all methods are stateless
 */
class MessageStripper {
 public:
  static bool should_strip(const std::string& message_type);
  static rclcpp::SerializedMessage strip(const std::string& message_type, const rclcpp::SerializedMessage& input);
};

}  // namespace pj_bridge
