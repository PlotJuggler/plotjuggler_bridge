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

#include <regex>
#include <string>
#include <vector>

#include "tl/expected.hpp"

namespace pj_bridge {

/// Filters topic names against a set of full-match ECMAScript regex patterns.
///
/// Mirrors foxglove_bridge's `topic_whitelist` semantics: a topic is allowed
/// if it fully matches at least one pattern (std::regex_match, not
/// regex_search — a pattern must match the entire topic name, not just a
/// prefix or substring).
///
/// A default-constructed (or empty-pattern-list) filter matches every topic,
/// preserving today's "no whitelist" behavior.
class WhitelistFilter {
 public:
  /// Default: no patterns configured, matches every topic name.
  WhitelistFilter() = default;

  /// Compile `patterns` as ECMAScript regexes.
  /// @return the filter on success, or an error message naming the first
  ///         pattern that failed to compile.
  static tl::expected<WhitelistFilter, std::string> create(const std::vector<std::string>& patterns);

  /// @return true if `topic_name` fully matches any configured pattern, or
  ///         if no patterns are configured (match-all default).
  bool matches(const std::string& topic_name) const;

 private:
  std::vector<std::regex> patterns_;  // empty = match all
};

}  // namespace pj_bridge
