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

#include <gtest/gtest.h>

#include "pj_bridge/transforming_topic_source.hpp"

using namespace pj_bridge;

namespace {
class StubSource : public TopicSourceInterface {
 public:
  std::vector<TopicInfo> get_topics() override {
    return {{"/cloud", "pkg/msg/In", ""}, {"/odom", "pkg/msg/Odom", ""}};
  }
  std::string get_schema(const std::string& topic) override {
    return "schema-of-" + topic;
  }
  std::string schema_encoding() const override {
    return "ros2msg";
  }
  bool is_transient_local(const std::string& topic) const override {
    return topic == "/cloud";
  }
};

class PassThrough : public MessageTransform {
 public:
  tl::expected<void, std::string> apply(std::span<const std::byte> in, std::vector<std::byte>& out) override {
    out.assign(in.begin(), in.end());
    return {};
  }
};

std::shared_ptr<TransformSet> make_set() {
  TransformFactory f;
  f.accepts = [](const std::string& t) { return t == "pkg/msg/In"; };
  f.check_params = [](const nlohmann::json&) -> tl::expected<void, std::string> { return {}; };
  f.output_type = [](const std::string&) { return std::string("pkg/msg/Out"); };
  f.output_schema = [](const std::string&, const std::string& s) { return "OUT:" + s; };
  f.create = [](const std::string&, const nlohmann::json&) { return std::make_unique<PassThrough>(); };
  nlohmann::json profile = {{"transforms", {{{"match_type", "pkg/msg/In"}, {"transform", "t"}}}}};
  return TransformSet::create(profile, {{"t", f}}).value();
}
}  // namespace

TEST(TransformingTopicSourceTest, RewritesTypeAndSchemaOnlyForMatchedTopics) {
  TransformingTopicSource source(std::make_shared<StubSource>(), make_set());

  auto topics = source.get_topics();
  ASSERT_EQ(topics.size(), 2u);
  EXPECT_EQ(topics[0].type, "pkg/msg/Out");
  EXPECT_EQ(topics[0].source_type, "pkg/msg/In");
  EXPECT_EQ(topics[1].type, "pkg/msg/Odom");
  EXPECT_TRUE(topics[1].source_type.empty());

  EXPECT_EQ(source.get_schema("/cloud"), "OUT:schema-of-/cloud");
  EXPECT_EQ(source.get_schema("/odom"), "schema-of-/odom");
  EXPECT_EQ(source.schema_encoding(), "ros2msg");
  EXPECT_TRUE(source.is_transient_local("/cloud"));
}
