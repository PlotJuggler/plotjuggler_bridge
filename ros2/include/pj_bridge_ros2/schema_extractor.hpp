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

#include <memory>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pj_bridge {

/**
 * @brief Extracts message definitions in ROS2 interface text format
 *
 * Reads .msg files from ROS2 packages and recursively expands
 * nested message types to produce the full message definition text.
 *
 * Thread safety: Thread-safe for concurrent access using shared_mutex.
 */
class SchemaExtractor {
 public:
  SchemaExtractor() = default;

  /**
   * @brief Get message definition text
   *
   * @param message_type Full message type name (e.g., "std_msgs/msg/String")
   * @return Message definition text, or empty string on failure
   */
  std::string get_message_definition(const std::string& message_type);

 private:
  bool parse_message_type(const std::string& message_type, std::string& library_name, std::string& type_name) const;

  bool build_message_definition_recursive(
      const std::string& package_name, const std::string& type_name, std::ostringstream& output,
      std::unordered_set<std::string>& processed_types, bool is_root) const;

  mutable std::unordered_map<std::string, std::string> definition_cache_;
  mutable std::unordered_map<std::string, std::string> msg_file_cache_;
  mutable std::shared_mutex cache_mutex_;
};

std::string remove_comments_from_schema(const std::string& schema);

}  // namespace pj_bridge
