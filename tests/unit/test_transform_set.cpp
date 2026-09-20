#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "fake_transform.hpp"
#include "pj_bridge/transform_set.hpp"

using namespace pj_bridge;
using namespace pj_bridge::test_helpers;
using nlohmann::json;

namespace {

TransformSet::FactoryMap factories() {
  return {{"fake", fake_factory("pkg/msg/In")}, {"other", fake_factory("pkg/msg/Other")}};
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
  EXPECT_EQ((*set)->bind("/a", "pkg/msg/In"), bound);  // cached: counters and codec state survive topic polls
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
      {"transforms",
       json::array(
           {{{"match_type", "pkg/msg/In"}, {"match_topic", "/a"}, {"transform", "fake"}, {"params", {{"fail", true}}}},
            {{"match_type", "pkg/msg/In"}, {"transform", "fake"}}})}};
  auto set = TransformSet::create(profile, factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  ASSERT_TRUE((*set)->append_type_rule("pkg/msg/Other", "other").has_value());

  std::vector<std::byte> in{std::byte{1}}, out;
  EXPECT_FALSE((*set)->bind("/a", "pkg/msg/In")->run("/a", in, out));  // rule 0 (fail=true)
  EXPECT_TRUE((*set)->bind("/b", "pkg/msg/In")->run("/b", in, out));   // rule 1
  EXPECT_EQ((*set)->bind("/c", "pkg/msg/Other")->transform_name, "other");
}

// A transform can only be paired with a type it accepts, and that is settled at
// startup: a topic of another type is simply not matched by the rule.
TEST(TransformSetTest, RuleNeverAppliesToATopicOfAnotherType) {
  auto set = TransformSet::create(
      rule({{"match_type", "pkg/msg/In"}, {"match_topic", "/lidar/.*"}, {"transform", "fake"}}), factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  ASSERT_TRUE((*set)->append_type_rule("pkg/msg/Other", "other").has_value());
  auto bound = (*set)->bind("/lidar/image", "pkg/msg/Other");  // falls through to the later rule
  ASSERT_NE(bound, nullptr);
  EXPECT_EQ(bound->transform_name, "other");
  EXPECT_EQ((*set)->bind("/lidar/unrelated", "pkg/msg/Unrelated"), nullptr);
}

TEST(TransformSetTest, FactoryReturningNullOrThrowingLeavesTopicUntransformed) {
  auto broken = fake_factory("pkg/msg/In");
  broken.create = [](const std::string&, const json& p) -> std::unique_ptr<MessageTransform> {
    if (p.value("fail", false)) {
      throw std::runtime_error("no codec");
    }
    return nullptr;
  };
  json profile = {
      {"transforms", json::array(
                         {{{"match_type", "pkg/msg/In"},
                           {"match_topic", "/throws"},
                           {"transform", "broken"},
                           {"params", {{"fail", true}}}},
                          {{"match_type", "pkg/msg/In"}, {"transform", "broken"}}})}};
  auto set = TransformSet::create(profile, {{"broken", broken}});
  ASSERT_TRUE(set.has_value()) << set.error();
  EXPECT_EQ((*set)->bind("/throws", "pkg/msg/In"), nullptr);
  EXPECT_EQ((*set)->bind("/null", "pkg/msg/In"), nullptr);
  EXPECT_EQ((*set)->find("/null"), nullptr);
}

TEST(TransformSetTest, ThrowingApplyIsADropNotACrash) {
  struct Thrower : MessageTransform {
    tl::expected<void, std::string> apply(std::span<const std::byte>, std::vector<std::byte>&) override {
      throw std::length_error("corrupt size field");
    }
  };
  auto f = fake_factory("pkg/msg/In");
  f.create = [](const std::string&, const json&) { return std::make_unique<Thrower>(); };
  auto set = TransformSet::create(rule({{"match_type", "pkg/msg/In"}, {"transform", "t"}}), {{"t", f}});
  ASSERT_TRUE(set.has_value()) << set.error();
  auto bound = (*set)->bind("/a", "pkg/msg/In");
  std::vector<std::byte> in(4), out;
  EXPECT_FALSE(bound->run("/a", in, out));
  EXPECT_EQ(bound->drops.load(), 1u);
}

TEST(TransformSetTest, StatsSummaryListsOnlyTopicsThatProcessedSamples) {
  auto set = TransformSet::create(rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}}), factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  std::vector<std::byte> in(4), out;
  (*set)->bind("/busy", "pkg/msg/In")->run("/busy", in, out);
  (*set)->bind("/idle", "pkg/msg/In");
  const auto summary = (*set)->stats_summary();
  EXPECT_NE(summary.find("/busy"), std::string::npos);
  EXPECT_EQ(summary.find("/idle"), std::string::npos);
}

// Gives TSAN the real threading model: run() on one thread, everything else on another.
TEST(TransformSetTest, RunIsSafeConcurrentlyWithBindFindAndStats) {
  auto set = TransformSet::create(rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}}), factories());
  ASSERT_TRUE(set.has_value()) << set.error();
  auto bound = (*set)->bind("/a", "pkg/msg/In");
  std::atomic<bool> stop{false};
  std::thread ingest([&] {
    std::vector<std::byte> in(64), out;
    while (!stop) {
      bound->run("/a", in, out);
    }
  });
  // Keep going until the ingest thread has really overlapped with us: on a busy
  // machine 2000 iterations can finish before it is even scheduled.
  for (int i = 0; i < 2000 || bound->samples.load() < 100; ++i) {
    (*set)->bind("/a", "pkg/msg/In");
    (*set)->bind("/other" + std::to_string(i % 8), "pkg/msg/In");
    (*set)->find("/a");
    (*set)->stats_summary();
  }
  stop = true;
  ingest.join();
  EXPECT_GT(bound->samples.load(), 0u);
}

// bind() runs for every topic on every topic poll: a transform that cannot be
// set up must be reported once, not once per second.
TEST(TransformSetTest, SetupErrorIsLoggedOncePerTopic) {
  std::ostringstream captured;
  // Restored on every exit path: the sink points at `captured`, which dies with this test.
  struct RestoreLogger {
    std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
    ~RestoreLogger() {
      spdlog::set_default_logger(previous);
    }
  } restore_logger;
  spdlog::set_default_logger(
      std::make_shared<spdlog::logger>("capture", std::make_shared<spdlog::sinks::ostream_sink_mt>(captured)));

  auto broken = fake_factory("pkg/msg/In");
  broken.create = [](const std::string&, const json&) -> std::unique_ptr<MessageTransform> {
    throw std::runtime_error("no codec");
  };
  auto set = TransformSet::create(rule({{"match_type", "pkg/msg/In"}, {"transform", "broken"}}), {{"broken", broken}});
  ASSERT_TRUE(set.has_value()) << set.error();

  EXPECT_EQ((*set)->bind("/a", "pkg/msg/In"), nullptr);
  (*set)->bind("/a", "pkg/msg/In");  // second poll
  (*set)->bind("/b", "pkg/msg/In");  // another topic is reported separately

  const std::string log = captured.str();
  size_t reports = 0;
  for (size_t at = log.find("could not be set up"); at != std::string::npos;
       at = log.find("could not be set up", at + 1)) {
    ++reports;
  }
  EXPECT_EQ(reports, 2u) << log;
  EXPECT_NE(log.find("'/a'"), std::string::npos) << log;
  EXPECT_NE(log.find("'/b'"), std::string::npos) << log;
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
                         {{{"match_type", "pkg/msg/In"},
                           {"match_topic", "/bad"},
                           {"transform", "fake"},
                           {"params", {{"fail", true}}}},
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
  const char* expected_in_error;  // the operator must be told WHY, not just "no"
};

class TransformSetBadProfileTest : public ::testing::TestWithParam<BadProfile> {};

TEST_P(TransformSetBadProfileTest, IsRejectedAtStartupWithAClearReason) {
  auto set = TransformSet::create(GetParam().profile, factories());
  ASSERT_FALSE(set.has_value());
  EXPECT_NE(set.error().find(GetParam().expected_in_error), std::string::npos) << set.error();
}

INSTANTIATE_TEST_SUITE_P(
    All, TransformSetBadProfileTest,
    ::testing::Values(
        BadProfile{"not_an_object", json::array(), "'transforms' array"},
        BadProfile{"missing_transforms", json::object(), "'transforms' array"},
        BadProfile{"unknown_top_level_key", json{{"transforms", json::array()}, {"extra", 1}}, "unknown key 'extra'"},
        BadProfile{"transforms_not_an_array", json{{"transforms", 5}}, "'transforms' array"},
        BadProfile{"rule_not_an_object", json{{"transforms", json::array({5})}}, "rule 0: must be an object"},
        BadProfile{
            "mistyped_rule_key", rule({{"match_type", 5}, {"transform", "fake"}}),
            "key 'match_type' is unknown or has the wrong JSON type"},
        BadProfile{"empty_match_type", rule({{"match_type", ""}, {"transform", "fake"}}), "non-empty 'match_type'"},
        BadProfile{
            "topic_only_rule", rule({{"match_topic", "/lidar/.*"}, {"transform", "fake"}}), "non-empty 'match_type'"},
        BadProfile{"rule_without_matcher", rule({{"transform", "fake"}}), "non-empty 'match_type'"},
        BadProfile{"rule_without_transform", rule({{"match_type", "pkg/msg/In"}}), "missing 'transform'"},
        BadProfile{
            "unknown_rule_key", rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}, {"oops", 1}}),
            "key 'oops' is unknown"},
        BadProfile{
            "unknown_transform", rule({{"match_type", "pkg/msg/In"}, {"transform", "nope"}}),
            "unknown transform 'nope'"},
        BadProfile{
            "invalid_regex", rule({{"match_type", "pkg/msg/In"}, {"match_topic", "("}, {"transform", "fake"}}),
            "invalid match_topic regex"},
        BadProfile{
            "unknown_param", rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}, {"params", {{"q", 1}}}}),
            "unknown param 'q'"},
        // the short / ROS1 spelling is the likely real-world mistake: the message must show the expected form
        BadProfile{
            "type_not_accepted", rule({{"match_type", "pkg/In"}, {"transform", "fake"}}),
            "does not accept type 'pkg/In' (use the full type name, e.g. 'sensor_msgs/msg/PointCloud2')"}),
    [](const auto& info) { return std::string(info.param.name); });
