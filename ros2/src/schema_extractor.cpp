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

#include "pj_bridge_ros2/schema_extractor.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace pj_bridge {

std::string SchemaExtractor::get_message_definition(const std::string& message_type) {
  {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    auto it = definition_cache_.find(message_type);
    if (it != definition_cache_.end()) {
      return it->second;
    }
  }

  std::string package_name;
  std::string type_name;

  if (!parse_message_type(message_type, package_name, type_name)) {
    return "";
  }

  std::ostringstream result;
  std::unordered_set<std::string> processed_types;

  if (!build_message_definition_recursive(package_name, type_name, result, processed_types, true)) {
    return "";
  }

  std::string definition = result.str();

  {
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    definition_cache_[message_type] = definition;
  }

  return definition;
}

bool SchemaExtractor::build_message_definition_recursive(
    const std::string& package_name, const std::string& type_name, std::ostringstream& output,
    std::unordered_set<std::string>& processed_types, bool is_root) const {
  const std::string full_type = package_name + "/" + type_name;

  std::string msg_content;
  {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    auto it = msg_file_cache_.find(full_type);
    if (it != msg_file_cache_.end()) {
      msg_content = it->second;
    }
  }

  if (msg_content.empty()) {
    std::string package_share_dir;
    try {
      package_share_dir = ament_index_cpp::get_package_share_directory(package_name);
    } catch (const std::exception& e) {
      return false;
    }

    std::string msg_file_path = package_share_dir + "/msg/" + type_name + ".msg";

    std::ifstream msg_file(msg_file_path);
    if (!msg_file.is_open()) {
      return false;
    }

    std::ostringstream content_stream;
    std::string line;
    while (std::getline(msg_file, line)) {
      content_stream << line << "\n";
    }
    msg_content = content_stream.str();

    {
      std::unique_lock<std::shared_mutex> lock(cache_mutex_);
      msg_file_cache_[full_type] = msg_content;
    }
  }

  if (!is_root) {
    output << "\n================================================================================\n";
    output << "MSG: " << full_type << "\n";
  }

  std::istringstream msg_stream(msg_content);
  std::string line;
  std::vector<std::string> nested_types;

  while (std::getline(msg_stream, line)) {
    output << line << "\n";

    std::istringstream line_stream(line);
    std::string first_word;
    line_stream >> first_word;

    if (first_word.empty() || first_word[0] == '#') {
      continue;
    }

    std::string nested_package;
    std::string nested_type;

    if (first_word.find('/') != std::string::npos) {
      size_t slash_pos = first_word.find('/');
      nested_package = first_word.substr(0, slash_pos);
      nested_type = first_word.substr(slash_pos + 1);

      size_t bracket_pos = nested_type.find('[');
      if (bracket_pos != std::string::npos) {
        nested_type = nested_type.substr(0, bracket_pos);
      }
    } else {
      std::string base_type = first_word;
      size_t bracket_pos = base_type.find('[');
      if (bracket_pos != std::string::npos) {
        base_type = base_type.substr(0, bracket_pos);
      }

      static const std::unordered_set<std::string> kBuiltinTypes = {"bool",   "byte",  "char",   "float32", "float64",
                                                                    "int8",   "uint8", "int16",  "uint16",  "int32",
                                                                    "uint32", "int64", "uint64", "string",  "wstring"};
      bool is_builtin = kBuiltinTypes.count(base_type) > 0;

      if (!is_builtin) {
        nested_package = package_name;
        nested_type = base_type;
      }
    }

    if (!nested_type.empty()) {
      std::string full_nested_type = nested_package + "/" + nested_type;
      if (processed_types.find(full_nested_type) == processed_types.end()) {
        processed_types.insert(full_nested_type);
        nested_types.push_back(full_nested_type);
      }
    }
  }

  if (nested_types.size() >= 2) {
    if (nested_types.at(0) == "std_msgs/Header") {
      nested_types.push_back(nested_types.at(0));
      nested_types.erase(nested_types.begin());
    }
  }

  for (const auto& nested_full_type : nested_types) {
    size_t slash_pos = nested_full_type.find('/');
    std::string nested_pkg = nested_full_type.substr(0, slash_pos);
    std::string nested_typ = nested_full_type.substr(slash_pos + 1);

    if (!build_message_definition_recursive(nested_pkg, nested_typ, output, processed_types, false)) {
      return false;
    }
  }

  return true;
}

bool SchemaExtractor::parse_message_type(
    const std::string& message_type, std::string& library_name, std::string& type_name) const {
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
    size_t comment_pos = line.find('#');

    if (comment_pos != std::string::npos) {
      std::string before_comment = line.substr(0, comment_pos);

      size_t end = before_comment.find_last_not_of(" \t\r\n");
      if (end != std::string::npos) {
        before_comment = before_comment.substr(0, end + 1);
      } else {
        before_comment = "";
      }

      if (!before_comment.empty()) {
        output << before_comment << "\n";
      }
    } else {
      output << line << "\n";
    }
  }

  return output.str();
}

}  // namespace pj_bridge
