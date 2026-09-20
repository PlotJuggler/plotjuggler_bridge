#include <gtest/gtest.h>

#include "pj_bridge/transform_set.hpp"

using namespace pj_bridge;
using nlohmann::json;

namespace {

/// Appends one marker byte to the input. `fail` makes apply() return an error.
class FakeTransform : public MessageTransform {
 public:
  explicit FakeTransform(bool fail) : fail_(fail) {}
  tl::expected<void, std::string> apply(std::span<const std::byte> in, std::vector<std::byte>& out) override {
    if (fail_) {
      return tl::make_unexpected(std::string("boom"));
    }
    out.assign(in.begin(), in.end());
    out.push_back(std::byte{0xAB});
    return {};
  }

 private:
  bool fail_;
};

TransformFactory fake_factory(const std::string& accepted_type) {
  TransformFactory f;
  f.accepts = [accepted_type](const std::string& t) { return t == accepted_type; };
  f.check_params = [](const json& p) -> tl::expected<void, std::string> {
    for (auto it = p.begin(); it != p.end(); ++it) {
      if (it.key() != "fail") {
        return tl::make_unexpected("unknown param '" + it.key() + "'");
      }
    }
    return {};
  };
  f.output_type = [](const std::string&) { return std::string("pkg/msg/Out"); };
  f.output_schema = [](const std::string&, const std::string& s) { return "OUT:" + s; };
  f.create = [](const std::string&, const json& p) { return std::make_unique<FakeTransform>(p.value("fail", false)); };
  return f;
}

TransformSet::FactoryMap factories() {
  return {{"fake", fake_factory("pkg/msg/In")}, {"other", fake_factory("pkg/msg/Other")}};
}

json rule(json fields) {
  return json{{"transforms", json::array({std::move(fields)})}};
}

}  // namespace

TEST(TransformSetTest, MatchesByType) {
  auto set = TransformSet::create(rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}}), factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  auto bound = (*set)->bind("/a", "pkg/msg/In");
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(bound->source_type, "pkg/msg/In");
  EXPECT_EQ(bound->output_type, "pkg/msg/Out");
  EXPECT_EQ((*set)->bind("/b", "pkg/msg/Unrelated"), nullptr);
  EXPECT_EQ((*set)->find("/a"), bound);
  EXPECT_EQ((*set)->find("/b"), nullptr);
}

TEST(TransformSetTest, TopicRegexIsFullMatchAndCombinesWithType) {
  auto set = TransformSet::create(
      rule({{"match_type", "pkg/msg/In"}, {"match_topic", "/lidar/.*"}, {"transform", "fake"}}), factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  EXPECT_NE((*set)->bind("/lidar/front", "pkg/msg/In"), nullptr);
  EXPECT_EQ((*set)->bind("/x/lidar/front", "pkg/msg/In"), nullptr);  // full match, not search
  EXPECT_EQ((*set)->bind("/lidar/rear", "pkg/msg/Unrelated"), nullptr);
}

TEST(TransformSetTest, FirstMatchingRuleWinsAndAppendedRulesComeLast) {
  json profile = {
      {"transforms", json::array(
                         {{{"match_topic", "/a"}, {"transform", "fake"}, {"params", {{"fail", true}}}},
                          {{"match_type", "pkg/msg/In"}, {"transform", "fake"}}})}};
  auto set = TransformSet::create(profile, factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  ASSERT_TRUE((*set)->append_type_rule("pkg/msg/Other", "other").has_value());

  std::vector<std::byte> in{std::byte{1}}, out;
  EXPECT_FALSE((*set)->bind("/a", "pkg/msg/In")->run("/a", in, out));  // rule 0 (fail=true)
  EXPECT_TRUE((*set)->bind("/b", "pkg/msg/In")->run("/b", in, out));   // rule 1
  EXPECT_EQ((*set)->bind("/c", "pkg/msg/Other")->transform_name, "other");
}

TEST(TransformSetTest, TopicOnlyRuleWithNonAcceptedTypeLeavesTopicUntransformed) {
  auto set = TransformSet::create(rule({{"match_topic", "/a"}, {"transform", "fake"}}), factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  EXPECT_EQ((*set)->bind("/a", "pkg/msg/Unrelated"), nullptr);
}

TEST(TransformSetTest, RebindsWhenSourceTypeChanges) {
  auto set = TransformSet::create(rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}}), factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  EXPECT_NE((*set)->bind("/a", "pkg/msg/In"), nullptr);
  EXPECT_EQ((*set)->bind("/a", "pkg/msg/Unrelated"), nullptr);
  EXPECT_EQ((*set)->find("/a"), nullptr);
}

TEST(TransformSetTest, RunCountsSamplesBytesAndDrops) {
  json profile = {
      {"transforms", json::array(
                         {{{"match_topic", "/bad"}, {"transform", "fake"}, {"params", {{"fail", true}}}},
                          {{"match_type", "pkg/msg/In"}, {"transform", "fake"}}})}};
  auto set = TransformSet::create(profile, factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  std::vector<std::byte> in(10, std::byte{7}), out;

  auto good = (*set)->bind("/good", "pkg/msg/In");
  ASSERT_TRUE(good->run("/good", in, out));
  EXPECT_EQ(out.size(), 11u);
  EXPECT_EQ(good->samples.load(), 1u);
  EXPECT_EQ(good->in_bytes.load(), 10u);
  EXPECT_EQ(good->out_bytes.load(), 11u);
  EXPECT_EQ(good->drops.load(), 0u);

  auto bad = (*set)->bind("/bad", "pkg/msg/In");
  EXPECT_FALSE(bad->run("/bad", in, out));
  EXPECT_EQ(bad->drops.load(), 1u);
  EXPECT_EQ(bad->samples.load(), 0u);
}

struct BadProfile {
  const char* name;
  json profile;
};

class TransformSetBadProfileTest : public ::testing::TestWithParam<BadProfile> {};

TEST_P(TransformSetBadProfileTest, IsRejectedAtStartup) {
  auto set = TransformSet::create(GetParam().profile, factories());
  EXPECT_FALSE(set.has_value());
}

INSTANTIATE_TEST_SUITE_P(
    All, TransformSetBadProfileTest,
    ::testing::Values(
        BadProfile{"not_an_object", json::array()}, BadProfile{"missing_transforms", json::object()},
        BadProfile{"unknown_top_level_key", json{{"transforms", json::array()}, {"extra", 1}}},
        BadProfile{"rule_without_matcher", rule({{"transform", "fake"}})},
        BadProfile{"rule_without_transform", rule({{"match_type", "pkg/msg/In"}})},
        BadProfile{"unknown_rule_key", rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}, {"oops", 1}})},
        BadProfile{"unknown_transform", rule({{"match_type", "pkg/msg/In"}, {"transform", "nope"}})},
        BadProfile{"invalid_regex", rule({{"match_topic", "("}, {"transform", "fake"}})},
        BadProfile{
            "unknown_param", rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}, {"params", {{"q", 1}}}})},
        BadProfile{"type_not_accepted", rule({{"match_type", "pkg/msg/Unrelated"}, {"transform", "fake"}})}),
    [](const auto& info) { return std::string(info.param.name); });
