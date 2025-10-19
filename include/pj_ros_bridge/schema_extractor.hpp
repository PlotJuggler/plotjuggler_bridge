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

#include <nlohmann/json.hpp>
#include <string>
#include <memory>

namespace pj_ros_bridge
{

/**
 * @brief Extracts message schemas using runtime introspection
 *
 * This class uses rosidl_typesupport_introspection_cpp to extract
 * message schemas at runtime without compile-time knowledge of types.
 *
 * Thread safety: Thread-safe for concurrent reads of different types.
 */
class SchemaExtractor
{
public:
  SchemaExtractor() = default;

  /**
   * @brief Extract schema for a given message type
   *
   * @param message_type Full message type name (e.g., "std_msgs/msg/String")
   * @return JSON object containing the schema, or null on failure
   */
  nlohmann::json extract_schema(const std::string& message_type);

  /**
   * @brief Get schema as JSON string
   *
   * @param message_type Full message type name
   * @return JSON string representation of schema, or empty string on failure
   */
  std::string get_schema_json(const std::string& message_type);

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
  bool parse_message_type(
    const std::string& message_type,
    std::string& library_name,
    std::string& type_name) const;

  /**
   * @brief Build JSON schema from introspection data
   *
   * @param type_support Pointer to type support introspection data
   * @return JSON object containing the schema
   */
  nlohmann::json build_schema_from_introspection(const void* type_support);
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__SCHEMA_EXTRACTOR_HPP_
