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

#ifndef PJ_ROS_BRIDGE__SCHEMA_EXTRACTOR_HPP_
#define PJ_ROS_BRIDGE__SCHEMA_EXTRACTOR_HPP_

#include <memory>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace pj_ros_bridge {

/**
 * @brief Extracts message definitions in ROS2 interface text format
 *
 * This class reads .msg files from ROS2 packages and recursively expands
 * nested message types to produce the full message definition text.
 *
 * Implements two-level caching:
 * - Level 1: Complete message definitions (final output)
 * - Level 2: Raw .msg file contents
 *
 * Thread safety: Thread-safe for concurrent access using shared_mutex.
 */
class SchemaExtractor {
 public:
  SchemaExtractor() = default;

  /**
   * @brief Get message definition text
   *
   * Returns the message definition in ROS2 interface format (same as ros2 interface show).
   * Results are cached for subsequent calls.
   *
   * @param message_type Full message type name (e.g., "std_msgs/msg/String")
   * @return Message definition text, or empty string on failure
   */
  std::string get_message_definition(const std::string& message_type);

 private:
  /**
   * @brief Convert ROS2 type string to library and type name
   *
   * Converts "pkg_name/msg/MsgType" to library name and type name
   * for use with rosidl type support lookup.
   *
   * @param message_type Full message type string
   * @param library_name Output: library name
   * @param type_name Output: type name
   * @return true if conversion successful, false otherwise
   */
  bool parse_message_type(const std::string& message_type, std::string& library_name, std::string& type_name) const;

  /**
   * @brief Recursively build message definition text from .msg files
   *
   * Reads .msg files from /opt/ros/DISTRO/share/{package}/msg/ and recursively
   * expands nested message types with ================MSG: separators.
   *
   * @param package_name Package name (e.g., "sensor_msgs")
   * @param type_name Type name (e.g., "PointCloud2")
   * @param output Output stream to write the definition to
   * @param processed_types Set of already processed types to avoid duplicates
   * @param is_root Whether this is the root type (no separator)
   * @return true if successful, false otherwise
   */
  bool build_message_definition_recursive(
      const std::string& package_name, const std::string& type_name, std::ostringstream& output,
      std::set<std::string>& processed_types, bool is_root) const;

  // Level 1 cache: Complete message definitions (key: "package/msg/Type")
  mutable std::unordered_map<std::string, std::string> definition_cache_;

  // Level 2 cache: Raw .msg file contents (key: "package/Type")
  mutable std::unordered_map<std::string, std::string> msg_file_cache_;

  // Thread safety: shared_mutex allows multiple readers or one writer
  mutable std::shared_mutex cache_mutex_;
};

std::string remove_comments_from_schema(const std::string& schema);

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__SCHEMA_EXTRACTOR_HPP_
