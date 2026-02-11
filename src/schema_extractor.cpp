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

#include "pj_ros_bridge/schema_extractor.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <fstream>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <vector>

namespace pj_ros_bridge {

std::string SchemaExtractor::get_message_definition(const std::string& message_type) {
  // Level 1 cache: Check if complete definition is cached
  {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    auto it = definition_cache_.find(message_type);
    if (it != definition_cache_.end()) {
      return it->second;
    }
  }

  // Cache miss - build the definition
  std::string package_name;
  std::string type_name;

  if (!parse_message_type(message_type, package_name, type_name)) {
    return "";
  }

  // Build the message definition by reading .msg files from ROS2 share directory
  // and recursively expanding nested types
  std::ostringstream result;
  std::set<std::string> processed_types;

  if (!build_message_definition_recursive(package_name, type_name, result, processed_types, true)) {
    return "";
  }

  std::string definition = result.str();

  // Store in cache
  {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    definition_cache_[message_type] = definition;
  }

  return definition;
}

bool SchemaExtractor::build_message_definition_recursive(
    const std::string& package_name, const std::string& type_name, std::ostringstream& output,
    std::set<std::string>& processed_types, bool is_root) const {
  const std::string full_type = package_name + "/" + type_name;

  // Level 2 cache: Check if .msg file content is cached
  std::string msg_content;
  {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    auto it = msg_file_cache_.find(full_type);
    if (it != msg_file_cache_.end()) {
      msg_content = it->second;
    }
  }

  // Cache miss - read the .msg file
  if (msg_content.empty()) {
    // Get package share directory
    std::string package_share_dir;
    try {
      package_share_dir = ament_index_cpp::get_package_share_directory(package_name);
    } catch (const std::exception& e) {
      return false;
    }

    // Construct path to .msg file
    std::string msg_file_path = package_share_dir + "/msg/" + type_name + ".msg";

    std::ifstream msg_file(msg_file_path);
    if (!msg_file.is_open()) {
      return false;
    }

    // Read entire file into string
    std::ostringstream content_stream;
    std::string line;
    while (std::getline(msg_file, line)) {
      content_stream << line << "\n";
    }
    msg_content = content_stream.str();

    // Store in cache
    {
      std::unique_lock<std::shared_mutex> lock(cache_mutex_);
      msg_file_cache_[full_type] = msg_content;
    }
  }

  // Add separator for nested types (not for root type)
  if (!is_root) {
    output << "\n================================================================================\n";
    output << "MSG: " << full_type << "\n";
  }

  // Process the .msg file content line by line
  std::istringstream msg_stream(msg_content);
  std::string line;
  std::vector<std::string> nested_types;

  while (std::getline(msg_stream, line)) {
    output << line << "\n";

    // Check if line contains a nested message type
    // Format: "package_name/MessageType field_name" or "MessageType field_name"
    std::istringstream line_stream(line);
    std::string first_word;
    line_stream >> first_word;

    // Skip comments and empty lines
    if (first_word.empty() || first_word[0] == '#') {
      continue;
    }

    // Check if this is a nested message type
    std::string nested_package;
    std::string nested_type;

    if (first_word.find('/') != std::string::npos) {
      // Format: package_name/MessageType
      size_t slash_pos = first_word.find('/');
      nested_package = first_word.substr(0, slash_pos);
      nested_type = first_word.substr(slash_pos + 1);

      // Remove array brackets if present
      size_t bracket_pos = nested_type.find('[');
      if (bracket_pos != std::string::npos) {
        nested_type = nested_type.substr(0, bracket_pos);
      }
    } else {
      // Check if it's a built-in type or a local type
      bool is_builtin =
          (first_word == "bool" || first_word == "byte" || first_word == "char" || first_word == "float32" ||
           first_word == "float64" || first_word == "int8" || first_word == "uint8" || first_word == "int16" ||
           first_word == "uint16" || first_word == "int32" || first_word == "uint32" || first_word == "int64" ||
           first_word == "uint64" || first_word == "string" || first_word == "wstring");

      if (!is_builtin) {
        // It's a local type in the same package
        nested_package = package_name;
        nested_type = first_word;

        // Remove array brackets if present
        size_t bracket_pos = nested_type.find('[');
        if (bracket_pos != std::string::npos) {
          nested_type = nested_type.substr(0, bracket_pos);
        }
      }
    }

    // If we found a nested type, add it to the list
    if (!nested_type.empty()) {
      std::string full_nested_type = nested_package + "/" + nested_type;
      if (processed_types.find(full_nested_type) == processed_types.end()) {
        processed_types.insert(full_nested_type);
        nested_types.push_back(full_nested_type);
      }
    }
  }
  // std_msgs/Header always goes last
  if (nested_types.size() >= 2) {
    if (nested_types.at(0) == "std_msgs/Header") {
      nested_types.push_back(nested_types.at(0));
      nested_types.erase(nested_types.begin());
    }
  }

  // Recursively process nested types in the order they appear
  for (const auto& nested_full_type : nested_types) {
    size_t slash_pos = nested_full_type.find('/');
    std::string nested_pkg = nested_full_type.substr(0, slash_pos);
    std::string nested_typ = nested_full_type.substr(slash_pos + 1);

    build_message_definition_recursive(nested_pkg, nested_typ, output, processed_types, false);
  }

  return true;
}

bool SchemaExtractor::parse_message_type(
    const std::string& message_type, std::string& library_name, std::string& type_name) const {
  // Expected format: "package_name/msg/MessageType"
  size_t first_slash = message_type.find('/');
  if (first_slash == std::string::npos) {
    return false;
  }

  size_t second_slash = message_type.find('/', first_slash + 1);
  if (second_slash == std::string::npos) {
    return false;
  }

  library_name = message_type.substr(0, first_slash);
  type_name = message_type.substr(second_slash + 1);

  return true;
}

std::string remove_comments_from_schema(const std::string& schema) {
  std::istringstream input(schema);
  std::ostringstream output;
  std::string line;

  while (std::getline(input, line)) {
    // Find the position of '#' character
    size_t comment_pos = line.find('#');

    if (comment_pos != std::string::npos) {
      // Keep everything before the '#', trim trailing whitespace
      std::string before_comment = line.substr(0, comment_pos);

      // Trim trailing whitespace
      size_t end = before_comment.find_last_not_of(" \t\r\n");
      if (end != std::string::npos) {
        before_comment = before_comment.substr(0, end + 1);
      } else {
        before_comment = "";
      }

      // Only add non-empty lines
      if (!before_comment.empty()) {
        output << before_comment << "\n";
      }
    } else {
      // No comment, keep the whole line
      output << line << "\n";
    }
  }

  return output.str();
}

}  // namespace pj_ros_bridge
