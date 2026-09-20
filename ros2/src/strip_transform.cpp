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

#include "pj_bridge_ros2/strip_transform.hpp"

#include <cstring>

#include "pj_bridge_ros2/message_stripper.hpp"

namespace pj_bridge {
namespace {

class StripTransform : public MessageTransform {
 public:
  explicit StripTransform(std::string type) : type_(std::move(type)) {}

  tl::expected<void, std::string> apply(std::span<const std::byte> in, std::vector<std::byte>& out) override {
    try {
      // MessageStripper needs a SerializedMessage: one copy of the input.
      rclcpp::SerializedMessage input(in.size());
      auto& rcl_in = input.get_rcl_serialized_message();
      std::memcpy(rcl_in.buffer, in.data(), in.size());
      rcl_in.buffer_length = in.size();

      const auto stripped = MessageStripper::strip(type_, input);
      const auto& rcl_out = stripped.get_rcl_serialized_message();
      const auto* bytes = reinterpret_cast<const std::byte*>(rcl_out.buffer);
      out.assign(bytes, bytes + rcl_out.buffer_length);
      return {};
    } catch (const std::exception& e) {
      return tl::make_unexpected(std::string(e.what()));
    }
  }

 private:
  std::string type_;
};

}  // namespace

TransformFactory make_strip_transform_factory() {
  TransformFactory f;
  f.accepts = [](const std::string& type) { return MessageStripper::should_strip(type); };
  f.check_params = [](const nlohmann::json& params) -> tl::expected<void, std::string> {
    if (!params.empty()) {
      return tl::make_unexpected(std::string("takes no params"));
    }
    return {};
  };
  f.output_type = [](const std::string& type) { return type; };
  f.output_schema = [](const std::string&, const std::string& schema) { return schema; };
  f.create = [](const std::string& type, const nlohmann::json&) { return std::make_unique<StripTransform>(type); };
  return f;
}

}  // namespace pj_bridge
