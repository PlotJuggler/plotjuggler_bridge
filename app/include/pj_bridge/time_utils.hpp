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

#include <chrono>
#include <cstdint>

namespace pj_bridge {

inline uint64_t get_current_time_ns() {
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

}  // namespace pj_bridge
