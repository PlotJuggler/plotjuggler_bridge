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

#ifndef PJ_ROS_BRIDGE__SCHEMA_EXTRACTOR_HPP_
#define PJ_ROS_BRIDGE__SCHEMA_EXTRACTOR_HPP_

#include <memory>
#include <set>
#include <sstream>
#include <string>

namespace pj_ros_bridge {

/**
 * @brief Extracts message definitions in ROS2 interface text format
 *
 * This class reads .msg files from ROS2 packages and recursively expands
 * nested message types to produce the full message definition text.
 *
 * Thread safety: Thread-safe for concurrent reads of different types.
 */
class SchemaExtractor {
 public:
  SchemaExtractor() = default;

  /**
   * @brief Get message definition text
   *
   * Returns the message definition in ROS2 interface format (same as ros2 interface show).
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
};

std::string remove_comments_from_schema(const std::string& schema);

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__SCHEMA_EXTRACTOR_HPP_
