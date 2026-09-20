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
#include <string>

#include "pj_bridge/transform_set.hpp"

namespace pj_bridge::test_helpers {

/// Appends one marker byte (0xAB) to the input; advertises `output_type` and
/// "OUT:" + schema. With param {"fail": true}, apply() returns an error.
class FakeTransform : public MessageTransform {
 public:
  FakeTransform(bool fail, std::string output_type) : fail_(fail), output_type_(std::move(output_type)) {}

  tl::expected<void, std::string> apply(std::span<const std::byte> in, std::vector<std::byte>& out) override {
    if (fail_) {
      return tl::make_unexpected(std::string("boom"));
    }
    out.assign(in.begin(), in.end());
    out.push_back(std::byte{0xAB});
    return {};
  }

  std::string output_type(const std::string&) const override {
    return output_type_;
  }

  std::string output_schema(const std::string& source_schema) const override {
    return "OUT:" + source_schema;
  }

 private:
  bool fail_;
  std::string output_type_;
};

/// Factory accepting exactly `accepted_type`; its only param is "fail".
inline TransformFactory fake_factory(const std::string& accepted_type, const std::string& output_type = "pkg/msg/Out") {
  TransformFactory f;
  f.accepts = [accepted_type](const std::string& t) { return t == accepted_type; };
  f.check_params = [](const nlohmann::json& p) -> tl::expected<void, std::string> {
    for (auto it = p.begin(); it != p.end(); ++it) {
      if (it.key() != "fail") {
        return tl::make_unexpected("unknown param '" + it.key() + "'");
      }
    }
    return {};
  };
  f.create = [output_type](const std::string&, const nlohmann::json& p) {
    return std::make_unique<FakeTransform>(p.value("fail", false), output_type);
  };
  return f;
}

/// Profile holding a single rule.
inline nlohmann::json rule(nlohmann::json fields) {
  return nlohmann::json{{"transforms", nlohmann::json::array({std::move(fields)})}};
}

}  // namespace pj_bridge::test_helpers
