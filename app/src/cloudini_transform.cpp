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

#include "pj_bridge/cloudini_transform.hpp"

#include <cloudini_lib/cloudini.hpp>
#include <cloudini_lib/ros_msg_utils.hpp>
// Defines non-inline `const char*` globals: include in this translation unit ONLY.
#include <cloudini_lib/ros_message_definitions.hpp>

namespace pj_bridge {
namespace {

constexpr const char* kPointCloud2 = "sensor_msgs/msg/PointCloud2";

// The encoder trusts field offsets, and the output header repeats width/height:
// a cloud whose metadata disagrees with its payload must not reach it.
tl::expected<void, std::string> check_cloud(const cloudini_ros::RosPointCloud2& cloud) {
  const uint64_t declared = uint64_t{cloud.width} * cloud.height * cloud.point_step;
  if (declared != cloud.data.size()) {
    return tl::make_unexpected(
        "width*height*point_step = " + std::to_string(declared) + " but data has " + std::to_string(cloud.data.size()) +
        " bytes");
  }
  for (const auto& field : cloud.fields) {
    if (uint64_t{field.offset} + static_cast<uint64_t>(Cloudini::SizeOf(field.type)) > cloud.point_step) {
      return tl::make_unexpected("field '" + field.name + "' extends past point_step");
    }
  }
  return {};
}

class CloudiniTransform : public MessageTransform {
 public:
  explicit CloudiniTransform(const nlohmann::json& params)
      : resolution_(params.value("resolution", 0.001f)), viz_preprocessing_(params.value("viz_preprocessing", false)) {
    if (params.contains("fields")) {
      for (auto it = params["fields"].begin(); it != params["fields"].end(); ++it) {
        profile_[it.key()] = it->get<float>();
      }
    }
  }

  tl::expected<void, std::string> apply(std::span<const std::byte> in, std::vector<std::byte>& out) override {
    try {
      const Cloudini::ConstBufferView raw(reinterpret_cast<const uint8_t*>(in.data()), in.size());
      auto cloud = cloudini_ros::getDeserializedPointCloudMessage(raw);
      if (auto valid = check_cloud(cloud); !valid) {
        return valid;
      }
      cloudini_ros::applyResolutionProfile(profile_, cloud.fields, resolution_);
      if (viz_preprocessing_) {
        cloudini_ros::applyVizLossyPreprocessing(cloud);
      }
      auto info = cloudini_ros::toEncodingInfo(cloud);
      // The frame is zstd-compressed by the bridge: a second stage here would compress twice.
      info.compression_opt = Cloudini::CompressionOption::NONE;
      info.use_threads = false;

      // Cloudini writes CDR into a vector<uint8_t>; scratch_ keeps its capacity across calls.
      cloudini_ros::convertPointCloud2ToCompressedCloud(cloud, info, scratch_);
      const auto* bytes = reinterpret_cast<const std::byte*>(scratch_.data());
      out.assign(bytes, bytes + scratch_.size());
      return {};
    } catch (const std::exception& e) {
      return tl::make_unexpected(std::string(e.what()));
    }
  }

  std::string output_type(const std::string&) const override {
    return compressed_schema_name;
  }

  std::string output_schema(const std::string&) const override {
    return compressed_schema_data;
  }

 private:
  float resolution_;
  bool viz_preprocessing_;
  cloudini_ros::ResolutionProfile profile_;
  std::vector<uint8_t> scratch_;
};

tl::expected<void, std::string> check_cloudini_params(const nlohmann::json& params) {
  for (auto it = params.begin(); it != params.end(); ++it) {
    const auto& key = it.key();
    if (key == "resolution") {
      if (!it->is_number() || it->get<double>() <= 0.0) {
        return tl::make_unexpected(std::string("'resolution' must be a number > 0"));
      }
    } else if (key == "viz_preprocessing") {
      if (!it->is_boolean()) {
        return tl::make_unexpected(std::string("'viz_preprocessing' must be a boolean"));
      }
    } else if (key == "fields") {
      if (!it->is_object()) {
        return tl::make_unexpected(std::string("'fields' must be an object of {name: resolution}"));
      }
      for (auto f = it->begin(); f != it->end(); ++f) {
        if (!f->is_number() || f->get<double>() < 0.0) {
          return tl::make_unexpected("'fields." + f.key() + "' must be a number >= 0 (0 removes the field)");
        }
      }
    } else {
      return tl::make_unexpected("unknown param '" + key + "'");
    }
  }
  return {};
}

}  // namespace

TransformFactory make_cloudini_transform_factory() {
  TransformFactory f;
  f.accepts = [](const std::string& type) { return type == kPointCloud2; };
  f.check_params = check_cloudini_params;
  f.create = [](const std::string&, const nlohmann::json& params) {
    return std::make_unique<CloudiniTransform>(params);
  };
  return f;
}

}  // namespace pj_bridge
