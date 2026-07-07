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

#include "pj_bridge/whitelist_filter.hpp"

namespace pj_bridge {

tl::expected<WhitelistFilter, std::string> WhitelistFilter::create(const std::vector<std::string>& patterns) {
  WhitelistFilter filter;
  filter.patterns_.reserve(patterns.size());

  for (const auto& pattern : patterns) {
    try {
      filter.patterns_.emplace_back(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
      return tl::unexpected<std::string>("Invalid whitelist regex '" + pattern + "': " + e.what());
    }
  }

  return filter;
}

bool WhitelistFilter::matches(const std::string& topic_name) const {
  if (patterns_.empty()) {
    return true;
  }
  for (const auto& pattern : patterns_) {
    if (std::regex_match(topic_name, pattern)) {
      return true;
    }
  }
  return false;
}

}  // namespace pj_bridge
