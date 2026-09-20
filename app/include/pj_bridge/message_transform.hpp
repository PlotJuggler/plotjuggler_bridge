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

#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

#include "tl/expected.hpp"

namespace pj_bridge {

/// One instance per topic, called from a single thread (the ingest executor).
/// Implementations SHOULD keep codec contexts and scratch buffers as members.
class MessageTransform {
 public:
  virtual ~MessageTransform() = default;

  /// `in` views the middleware's buffer and is valid only during the call.
  /// `out` is the final storage later held by MessageBuffer: clear and fill it.
  /// On error the caller DROPS the sample. It must never forward `in` instead:
  /// the client was told the output type at subscribe time.
  virtual tl::expected<void, std::string> apply(std::span<const std::byte> in, std::vector<std::byte>& out) = 0;

  /// Type name advertised to clients in place of `source_type`. Default: unchanged.
  virtual std::string output_type(const std::string& source_type) const {
    return source_type;
  }

  /// Schema advertised to clients, given the untransformed one. Default: unchanged.
  virtual std::string output_schema(const std::string& source_schema) const {
    return source_schema;
  }
};

struct TransformFactory {
  /// Can this transform handle messages of `source_type`?
  std::function<bool(const std::string& source_type)> accepts;
  /// Reject unknown or malformed params. Called once per rule at startup.
  std::function<tl::expected<void, std::string>(const nlohmann::json& params)> check_params;
  std::function<std::unique_ptr<MessageTransform>(const std::string& source_type, const nlohmann::json& params)> create;
};

}  // namespace pj_bridge
