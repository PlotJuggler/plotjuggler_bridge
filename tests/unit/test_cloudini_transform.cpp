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

#ifdef PJ_BRIDGE_HAS_CLOUDINI

#include <gtest/gtest.h>

#include <functional>
#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>

#include "pj_bridge/cloudini_transform.hpp"

using namespace pj_bridge;

namespace {
std::vector<std::byte> make_cloud_cdr(
    size_t points, const std::function<void(sensor_msgs::msg::PointCloud2&)>& corrupt = nullptr) {
  sensor_msgs::msg::PointCloud2 cloud;
  cloud.header.frame_id = "lidar";
  sensor_msgs::PointCloud2Modifier modifier(cloud);
  modifier.setPointCloud2FieldsByString(1, "xyz");
  modifier.resize(points);
  sensor_msgs::PointCloud2Iterator<float> x(cloud, "x"), y(cloud, "y"), z(cloud, "z");
  for (size_t i = 0; i < points; ++i, ++x, ++y, ++z) {
    *x = 0.01f * static_cast<float>(i);
    *y = 1.0f;
    *z = -0.5f;
  }
  if (corrupt) {
    corrupt(cloud);
  }
  rclcpp::SerializedMessage serialized;
  rclcpp::Serialization<sensor_msgs::msg::PointCloud2>().serialize_message(&cloud, &serialized);
  const auto& rcl = serialized.get_rcl_serialized_message();
  const auto* bytes = reinterpret_cast<const std::byte*>(rcl.buffer);
  return {bytes, bytes + rcl.buffer_length};
}
}  // namespace

TEST(CloudiniTransformTest, CompressesPointCloud2) {
  auto factory = make_cloudini_transform_factory();
  EXPECT_TRUE(factory.accepts("sensor_msgs/msg/PointCloud2"));
  EXPECT_FALSE(factory.accepts("sensor_msgs/msg/Image"));
  EXPECT_EQ(factory.output_type("sensor_msgs/msg/PointCloud2"), "point_cloud_interfaces/msg/CompressedPointCloud2");
  EXPECT_NE(factory.output_schema("sensor_msgs/msg/PointCloud2", "").find("format"), std::string::npos);

  const nlohmann::json params = {{"resolution", 0.001}};
  ASSERT_TRUE(factory.check_params(params).has_value());
  EXPECT_FALSE(factory.check_params({{"resolutoin", 0.001}}).has_value());
  EXPECT_FALSE(factory.check_params({{"resolution", -1.0}}).has_value());

  auto transform = factory.create("sensor_msgs/msg/PointCloud2", params);
  const auto in = make_cloud_cdr(10000);
  std::vector<std::byte> out;
  ASSERT_TRUE(transform->apply(in, out).has_value());
  EXPECT_FALSE(out.empty());
  EXPECT_LT(out.size(), in.size());

  ASSERT_TRUE(transform->apply(in, out).has_value());  // instance is reusable
}

TEST(CloudiniTransformTest, GarbageInputIsAnErrorNotACrash) {
  auto transform = make_cloudini_transform_factory().create("sensor_msgs/msg/PointCloud2", nlohmann::json::object());
  const std::vector<std::byte> in(7, std::byte{0xFF});
  std::vector<std::byte> out;
  EXPECT_FALSE(transform->apply(in, out).has_value());
}

// Any DDS peer can publish a PointCloud2 whose metadata lies about its payload.
// The encoder trusts field offsets and the header repeats width/height, so these
// must be rejected here: a drop, not an over-read or an undecodable frame.
TEST(CloudiniTransformTest, InconsistentCloudMetadataIsRejected) {
  auto transform = make_cloudini_transform_factory().create("sensor_msgs/msg/PointCloud2", nlohmann::json::object());
  std::vector<std::byte> out;

  const auto field_past_point = make_cloud_cdr(100, [](auto& c) { c.fields[2].offset = c.point_step; });
  EXPECT_FALSE(transform->apply(field_past_point, out).has_value());

  const auto width_lies = make_cloud_cdr(100, [](auto& c) { c.width = 1000; });
  EXPECT_FALSE(transform->apply(width_lies, out).has_value());

  EXPECT_TRUE(transform->apply(make_cloud_cdr(100), out).has_value());  // and a sane one still works
}

#endif  // PJ_BRIDGE_HAS_CLOUDINI
