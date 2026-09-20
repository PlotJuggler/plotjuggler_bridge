# Message Transforms (v1: Cloudini) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Operator-configured, per-topic payload transforms in pj_bridge, with Cloudini point cloud compression and the existing stripper as the two implementations.

**Architecture:** A `TransformSet` (rules from a JSON profile + name→factory map) is shared by a `TransformingTopicSource` decorator (rewrites advertised type/schema) and by a hook inside `Ros2SubscriptionManager` (runs `apply()` on the rcl buffer before any copy). `BridgeServer` is untouched except for two informational fields.

**Tech Stack:** C++20, nlohmann/json, tl::expected, spdlog, gtest, Cloudini 1.3.0 (`cloudini_lib`), rclcpp.

**Spec:** `docs/superpowers/specs/2026-09-20-message-transforms-design.md`

## Environment (read first)

All builds and tests run through pixi RoboStack, from the worktree root
`~/ws_plotjuggler/plotjuggler_bridge/.worktrees/message-transforms`:

```bash
set -o pipefail
mkdir -p install && touch install/setup.sh
B="~/.pixi/bin/pixi run -e humble"
$B colcon build --packages-select pj_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON 2>&1 | tail -20
$B bash -c 'source install/setup.bash && build/pj_bridge/pj_bridge_tests --gtest_filter="<Filter>*"'
```

"Build" and "Run tests `<Filter>`" below mean these two commands. The baseline
is 271+ tests passing. Format with `pre-commit run -a` before every commit.
Commit messages end with the repo's standard `Co-Authored-By` trailer.

## File map

| File | Responsibility |
|---|---|
| `app/include/pj_bridge/message_transform.hpp` (new) | `MessageTransform`, `TransformFactory` |
| `app/include/pj_bridge/transform_set.hpp`, `app/src/transform_set.cpp` (new) | Profile parsing, rule matching, per-topic bindings, counters |
| `app/include/pj_bridge/transforming_topic_source.hpp`, `app/src/transforming_topic_source.cpp` (new) | Decorator over `TopicSourceInterface` |
| `app/include/pj_bridge/cloudini_transform.hpp`, `app/src/cloudini_transform.cpp` (new, conditional) | Cloudini factory |
| `ros2/include/pj_bridge_ros2/strip_transform.hpp`, `ros2/src/strip_transform.cpp` (new) | `strip` factory over `MessageStripper` |
| `ros2/src/ros2_subscription_manager.cpp`, its header | The hook; loses its `strip_large_messages` bool |
| `ros2/src/main.cpp` | `transform_profile` param, wiring, stats at shutdown |
| `app/include/pj_bridge/topic_source_interface.hpp` | `TopicInfo::source_type` |
| `app/src/bridge_server.cpp`, `protocol_constants.hpp` | emit `source_type`; `message_transforms` capability |
| `tests/unit/test_transform_set.cpp`, `test_transforming_topic_source.cpp`, `test_cloudini_transform.cpp` (new) | Tests |

---

### Task 1: Move the project to C++20

**Files:** Modify `CMakeLists.txt:18`

- [ ] **Step 1:** Change `set(CMAKE_CXX_STANDARD 17)` to `set(CMAKE_CXX_STANDARD 20)`. Run `grep -rn "cxx_std_17\|CXX_STANDARD 17\|-std=" CMakeLists.txt cmake/ 2>/dev/null` and update any other hit to 20.
- [ ] **Step 2:** Build, then run the whole suite (no filter). Expected: same pass count as before the change. C++20 typically surfaces: implicit `this` capture in `[=]` lambdas (make it `[=, this]`), and `u8""` literal type changes. Fix what the compiler reports, nothing else.
- [ ] **Step 3:** Repeat Step 2 with `-e jazzy`.
- [ ] **Step 4:** FastDDS standalone build still configures and compiles (see CLAUDE.md "FastDDS Backend"); skip if Conan cache is unavailable and say so in the commit body.
- [ ] **Step 5:** Commit: `build: require C++20`. Push the branch and confirm Humble/Jazzy/Kilted/Lyrical CI is green **before** starting Task 2 (so a compiler issue is never confused with the feature).

---

### Task 2: Transform interface

**Files:** Create `app/include/pj_bridge/message_transform.hpp`

- [ ] **Step 1:** Write the header (license banner copied from a sibling header):

```cpp
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <tl/expected.hpp>
#include <vector>

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
};

struct TransformFactory {
  /// Can this transform handle messages of `source_type`?
  std::function<bool(const std::string& source_type)> accepts;
  /// Reject unknown or malformed params. Called once per rule at startup.
  std::function<tl::expected<void, std::string>(const nlohmann::json& params)> check_params;
  /// Type name advertised to clients in place of `source_type`.
  std::function<std::string(const std::string& source_type)> output_type;
  /// Schema advertised to clients. `source_schema` is the untransformed one.
  std::function<std::string(const std::string& source_type, const std::string& source_schema)> output_schema;
  std::function<std::unique_ptr<MessageTransform>(const std::string& source_type, const nlohmann::json& params)>
      create;
};

}  // namespace pj_bridge
```

Check the include spelling of `tl/expected.hpp` against `app/include/pj_bridge/whitelist_filter.hpp` and copy it exactly.

- [ ] **Step 2:** Build (header is not yet included anywhere; this only checks nothing else broke). Commit: `feat(transform): MessageTransform interface`.

---

### Task 3: TransformSet — profile parsing and matching

**Files:** Create `app/include/pj_bridge/transform_set.hpp`, `app/src/transform_set.cpp`, `tests/unit/test_transform_set.cpp`. Modify `CMakeLists.txt` (add the `.cpp` to `pj_bridge_app`, the test to `${PROJECT_NAME}_tests`).

- [ ] **Step 1: Header**

```cpp
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_bridge/message_transform.hpp"

namespace pj_bridge {

/// A transform bound to one topic, plus its counters.
/// run() is called from the ingest thread only; counters are read from others.
struct BoundTransform {
  std::string source_type;
  std::string transform_name;
  std::string output_type;
  std::unique_ptr<MessageTransform> transform;

  std::atomic<uint64_t> samples{0};
  std::atomic<uint64_t> drops{0};
  std::atomic<uint64_t> in_bytes{0};
  std::atomic<uint64_t> out_bytes{0};
  std::atomic<uint64_t> micros{0};

  /// Applies the transform and updates counters. False = drop the sample.
  bool run(const std::string& topic, std::span<const std::byte> in, std::vector<std::byte>& out);
};

/// Ordered transform rules plus the per-topic bindings created from them.
/// Thread safety: all public methods are safe to call concurrently.
class TransformSet {
 public:
  using FactoryMap = std::map<std::string, TransformFactory>;

  /// Parse and validate a profile. Any problem is a startup error.
  static tl::expected<std::shared_ptr<TransformSet>, std::string> create(
      const nlohmann::json& profile, FactoryMap factories);

  /// Append a `match_type` rule after the profile's rules (strip_large_messages sugar).
  tl::expected<void, std::string> append_type_rule(const std::string& match_type, const std::string& transform);

  /// First matching rule wins. Creates and remembers the binding on first sight.
  /// Returns nullptr when no rule applies to this topic.
  std::shared_ptr<BoundTransform> bind(const std::string& topic, const std::string& source_type);

  /// Binding previously created by bind(), or nullptr.
  std::shared_ptr<BoundTransform> find(const std::string& topic) const;

  /// Schema to advertise for a bound topic.
  std::string output_schema(const BoundTransform& bound, const std::string& source_schema) const;

  /// One line per bound topic that processed at least one sample.
  std::string stats_summary() const;

 private:
  struct Rule {
    std::optional<std::string> match_type;
    std::optional<std::regex> match_topic;
    std::string transform;
    nlohmann::json params;
  };

  TransformSet() = default;
  tl::expected<void, std::string> add_rule(Rule rule);

  FactoryMap factories_;
  std::vector<Rule> rules_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<BoundTransform>> bindings_;
  std::set<std::string> warned_topics_;
};

}  // namespace pj_bridge
```

- [ ] **Step 2: Failing tests** — `tests/unit/test_transform_set.cpp`

```cpp
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
  json profile = {{"transforms", json::array({{{"match_topic", "/a"}, {"transform", "fake"}, {"params", {{"fail", true}}}},
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
  json profile = {{"transforms", json::array({{{"match_topic", "/bad"}, {"transform", "fake"}, {"params", {{"fail", true}}}},
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
        BadProfile{"not_an_object", json::array()},
        BadProfile{"missing_transforms", json::object()},
        BadProfile{"unknown_top_level_key", json{{"transforms", json::array()}, {"extra", 1}}},
        BadProfile{"rule_without_matcher", rule({{"transform", "fake"}})},
        BadProfile{"rule_without_transform", rule({{"match_type", "pkg/msg/In"}})},
        BadProfile{"unknown_rule_key", rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}, {"oops", 1}})},
        BadProfile{"unknown_transform", rule({{"match_type", "pkg/msg/In"}, {"transform", "nope"}})},
        BadProfile{"invalid_regex", rule({{"match_topic", "("}, {"transform", "fake"}})},
        BadProfile{"unknown_param", rule({{"match_type", "pkg/msg/In"}, {"transform", "fake"}, {"params", {{"q", 1}}}})},
        BadProfile{"type_not_accepted", rule({{"match_type", "pkg/msg/Unrelated"}, {"transform", "fake"}})}),
    [](const auto& info) { return std::string(info.param.name); });
```

- [ ] **Step 3:** Add both files to `CMakeLists.txt`, build. Expected: link errors for `TransformSet::*` (not implemented).

- [ ] **Step 4: Implementation** — `app/src/transform_set.cpp`

```cpp
#include "pj_bridge/transform_set.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>

namespace pj_bridge {

bool BoundTransform::run(const std::string& topic, std::span<const std::byte> in, std::vector<std::byte>& out) {
  const auto start = std::chrono::steady_clock::now();
  auto result = transform->apply(in, out);
  if (!result) {
    const uint64_t n = drops.fetch_add(1) + 1;
    if ((n & (n - 1)) == 0) {  // 1st, 2nd, 4th, 8th... keeps a broken topic from flooding the log
      spdlog::warn("Transform '{}' failed on '{}' ({} drops so far): {}", transform_name, topic, n, result.error());
    }
    return false;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
  samples.fetch_add(1);
  in_bytes.fetch_add(in.size());
  out_bytes.fetch_add(out.size());
  micros.fetch_add(static_cast<uint64_t>(elapsed.count()));
  return true;
}

tl::expected<std::shared_ptr<TransformSet>, std::string> TransformSet::create(
    const nlohmann::json& profile, FactoryMap factories) {
  if (!profile.is_object() || !profile.contains("transforms") || !profile["transforms"].is_array()) {
    return tl::make_unexpected(std::string("transform profile must be an object with a 'transforms' array"));
  }
  for (auto it = profile.begin(); it != profile.end(); ++it) {
    if (it.key() != "transforms") {
      return tl::make_unexpected("transform profile: unknown key '" + it.key() + "'");
    }
  }

  std::shared_ptr<TransformSet> set(new TransformSet());
  set->factories_ = std::move(factories);

  size_t index = 0;
  for (const auto& entry : profile["transforms"]) {
    const std::string where = "transform profile rule " + std::to_string(index++) + ": ";
    if (!entry.is_object()) {
      return tl::make_unexpected(where + "must be an object");
    }
    Rule rule;
    for (auto it = entry.begin(); it != entry.end(); ++it) {
      const auto& key = it.key();
      if (key == "match_type" && it->is_string()) {
        rule.match_type = it->get<std::string>();
      } else if (key == "match_topic" && it->is_string()) {
        try {
          rule.match_topic.emplace(it->get<std::string>(), std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
          return tl::make_unexpected(where + "invalid match_topic regex: " + e.what());
        }
      } else if (key == "transform" && it->is_string()) {
        rule.transform = it->get<std::string>();
      } else if (key == "params" && it->is_object()) {
        rule.params = *it;
      } else {
        return tl::make_unexpected(where + "unknown or mistyped key '" + key + "'");
      }
    }
    if (rule.params.is_null()) {
      rule.params = nlohmann::json::object();
    }
    if (auto added = set->add_rule(std::move(rule)); !added) {
      return tl::make_unexpected(where + added.error());
    }
  }
  return set;
}

tl::expected<void, std::string> TransformSet::add_rule(Rule rule) {
  if (!rule.match_type && !rule.match_topic) {
    return tl::make_unexpected(std::string("needs match_type and/or match_topic"));
  }
  if (rule.transform.empty()) {
    return tl::make_unexpected(std::string("missing 'transform'"));
  }
  auto factory = factories_.find(rule.transform);
  if (factory == factories_.end()) {
    return tl::make_unexpected("unknown transform '" + rule.transform + "' (not built in, or misspelled)");
  }
  if (auto ok = factory->second.check_params(rule.params); !ok) {
    return tl::make_unexpected("transform '" + rule.transform + "': " + ok.error());
  }
  if (rule.match_type && !factory->second.accepts(*rule.match_type)) {
    return tl::make_unexpected("transform '" + rule.transform + "' does not accept type '" + *rule.match_type + "'");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  rules_.push_back(std::move(rule));
  return {};
}

tl::expected<void, std::string> TransformSet::append_type_rule(
    const std::string& match_type, const std::string& transform) {
  Rule rule;
  rule.match_type = match_type;
  rule.transform = transform;
  rule.params = nlohmann::json::object();
  return add_rule(std::move(rule));
}

std::shared_ptr<BoundTransform> TransformSet::bind(const std::string& topic, const std::string& source_type) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (auto it = bindings_.find(topic); it != bindings_.end()) {
    if (it->second->source_type == source_type) {
      return it->second;
    }
    bindings_.erase(it);  // the topic's type changed: match again
  }

  for (const auto& rule : rules_) {
    if (rule.match_type && *rule.match_type != source_type) {
      continue;
    }
    if (rule.match_topic && !std::regex_match(topic, *rule.match_topic)) {
      continue;
    }
    const auto& factory = factories_.at(rule.transform);
    if (!factory.accepts(source_type)) {
      if (warned_topics_.insert(topic).second) {
        spdlog::warn(
            "Transform '{}' matched topic '{}' but does not accept type '{}'; leaving it untransformed",
            rule.transform, topic, source_type);
      }
      return nullptr;
    }
    auto bound = std::make_shared<BoundTransform>();
    bound->source_type = source_type;
    bound->transform_name = rule.transform;
    bound->output_type = factory.output_type(source_type);
    bound->transform = factory.create(source_type, rule.params);
    bindings_[topic] = bound;
    spdlog::info("Topic '{}' ({}) -> transform '{}' -> {}", topic, source_type, rule.transform, bound->output_type);
    return bound;
  }
  return nullptr;
}

std::shared_ptr<BoundTransform> TransformSet::find(const std::string& topic) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = bindings_.find(topic);
  return it == bindings_.end() ? nullptr : it->second;
}

std::string TransformSet::output_schema(const BoundTransform& bound, const std::string& source_schema) const {
  return factories_.at(bound.transform_name).output_schema(bound.source_type, source_schema);
}

std::string TransformSet::stats_summary() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream os;
  for (const auto& [topic, bound] : bindings_) {
    const uint64_t n = bound->samples.load();
    if (n == 0 && bound->drops.load() == 0) {
      continue;
    }
    const double in = static_cast<double>(bound->in_bytes.load());
    const double out = static_cast<double>(bound->out_bytes.load());
    os << "\n  " << topic << " [" << bound->transform_name << "]: " << n << " samples, " << bound->drops.load()
       << " drops, ratio " << (out > 0 ? in / out : 0.0) << ", " << (n ? bound->micros.load() / n : 0)
       << " us/sample";
  }
  return os.str();
}

}  // namespace pj_bridge
```

`factories_` is written only in `create()` before the object is shared, so reading it without the lock in `output_schema()` is safe.

- [ ] **Step 5:** Build, run tests `TransformSet*`. Expected: 6 + 10 tests PASS.
- [ ] **Step 6:** Commit: `feat(transform): TransformSet profile parsing, matching and counters`.

---

### Task 4: TransformingTopicSource + informational protocol fields

**Files:** Create `app/include/pj_bridge/transforming_topic_source.hpp`, `app/src/transforming_topic_source.cpp`, `tests/unit/test_transforming_topic_source.cpp`. Modify `app/include/pj_bridge/topic_source_interface.hpp`, `app/src/bridge_server.cpp:281`, `app/include/pj_bridge/protocol_constants.hpp`, `tests/unit/test_protocol_constants.cpp`, `CMakeLists.txt`.

- [ ] **Step 1:** Add to `TopicInfo`:

```cpp
  std::string source_type;  ///< set only for transformed topics: the type before the transform
```

- [ ] **Step 2: Failing test**

```cpp
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
```

- [ ] **Step 3:** Build. Expected: compile error, `transforming_topic_source.hpp` not found.

- [ ] **Step 4: Implementation**

Header:

```cpp
#pragma once

#include <memory>

#include "pj_bridge/topic_source_interface.hpp"
#include "pj_bridge/transform_set.hpp"

namespace pj_bridge {

/// Advertises transformed topics with their OUTPUT type and schema, so clients
/// never see the source type. Thread safety: same as the wrapped source.
class TransformingTopicSource : public TopicSourceInterface {
 public:
  TransformingTopicSource(std::shared_ptr<TopicSourceInterface> inner, std::shared_ptr<TransformSet> transforms);

  std::vector<TopicInfo> get_topics() override;
  std::string get_schema(const std::string& topic_name) override;
  std::string schema_encoding() const override;
  bool is_transient_local(const std::string& topic_name) const override;

 private:
  std::shared_ptr<TopicSourceInterface> inner_;
  std::shared_ptr<TransformSet> transforms_;
};

}  // namespace pj_bridge
```

Source:

```cpp
#include "pj_bridge/transforming_topic_source.hpp"

namespace pj_bridge {

TransformingTopicSource::TransformingTopicSource(
    std::shared_ptr<TopicSourceInterface> inner, std::shared_ptr<TransformSet> transforms)
    : inner_(std::move(inner)), transforms_(std::move(transforms)) {}

std::vector<TopicInfo> TransformingTopicSource::get_topics() {
  auto topics = inner_->get_topics();
  for (auto& topic : topics) {
    if (auto bound = transforms_->bind(topic.name, topic.type)) {
      topic.source_type = topic.type;
      topic.type = bound->output_type;
    }
  }
  return topics;
}

std::string TransformingTopicSource::get_schema(const std::string& topic_name) {
  auto schema = inner_->get_schema(topic_name);
  if (auto bound = transforms_->find(topic_name)) {
    return transforms_->output_schema(*bound, schema);
  }
  return schema;
}

std::string TransformingTopicSource::schema_encoding() const {
  return inner_->schema_encoding();
}

bool TransformingTopicSource::is_transient_local(const std::string& topic_name) const {
  return inner_->is_transient_local(topic_name);
}

}  // namespace pj_bridge
```

- [ ] **Step 5:** In `app/src/bridge_server.cpp`, right after `topic_entry["type"] = topic.type;` (line ~281):

```cpp
    if (!topic.source_type.empty()) {
      topic_entry["source_type"] = topic.source_type;
    }
```

`topics_changed` entries (line ~824) are built from a name→type map and do not carry `source_type` in v1; say so in `docs/API.md` (Task 8).

- [ ] **Step 6:** Append to `kServerCapabilities`: `"message_transforms",    // topics may be advertised with a transformed type (+ source_type)`. If `test_protocol_constants.cpp` asserts the list or its size, update it.
- [ ] **Step 7:** Any aggregate initialisation `TopicInfo{name, type}` still compiles (the new member is last and defaulted). Build, run the whole suite. Expected: all PASS.
- [ ] **Step 8:** Commit: `feat(transform): advertise transformed type and schema`.

---

### Task 5: ROS2 hook and the `strip` transform

**Files:** Create `ros2/include/pj_bridge_ros2/strip_transform.hpp`, `ros2/src/strip_transform.cpp`. Modify `ros2/include/pj_bridge_ros2/message_stripper.hpp`, `ros2/src/message_stripper.cpp`, `ros2/include/pj_bridge_ros2/ros2_subscription_manager.hpp`, `ros2/src/ros2_subscription_manager.cpp`, `tests/unit/test_ros2_subscription_manager.cpp`, `CMakeLists.txt`.

- [ ] **Step 1:** Expose the stripper's type list. In `message_stripper.hpp` add to the class:

```cpp
  static const std::unordered_set<std::string>& strippable_types();
```

(`#include <unordered_set>`), and in the `.cpp`: `const std::unordered_set<std::string>& MessageStripper::strippable_types() { return kStrippableTypes; }`.

- [ ] **Step 2:** `strip_transform.hpp`:

```cpp
#pragma once

#include "pj_bridge/message_transform.hpp"

namespace pj_bridge {

/// `strip` transform: MessageStripper behind the MessageTransform interface.
/// Output type and schema equal the input's. Takes no params.
TransformFactory make_strip_transform_factory();

}  // namespace pj_bridge
```

`strip_transform.cpp`:

```cpp
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
```

- [ ] **Step 3:** Change `Ros2SubscriptionManager`. Constructor becomes:

```cpp
  explicit Ros2SubscriptionManager(
      rclcpp::Node::SharedPtr node, std::shared_ptr<TransformSet> transforms = nullptr, size_t min_qos_depth = 1,
      size_t max_qos_depth = 100);
```

Replace the member `bool strip_large_messages_;` with `std::shared_ptr<TransformSet> transforms_;`, include `pj_bridge/transform_set.hpp`, drop the `message_stripper.hpp` include. Update the class comment: "...and applies the topic's transform, if any, on the rcl buffer before copying."

- [ ] **Step 4: Failing test.** First read `tests/unit/test_ros2_subscription_manager.cpp`: every construction passing `true`/`false` as the second argument must change. Replace the strip-specific tests' setup with a `TransformSet` holding the strip factory, and add one new test. Helper + test (adapt the publisher/spin scaffolding to what that file already uses):

```cpp
std::shared_ptr<pj_bridge::TransformSet> strip_everything() {
  auto set = pj_bridge::TransformSet::create(
                 nlohmann::json{{"transforms", nlohmann::json::array()}},
                 {{"strip", pj_bridge::make_strip_transform_factory()}})
                 .value();
  for (const auto& type : pj_bridge::MessageStripper::strippable_types()) {
    EXPECT_TRUE(set->append_type_rule(type, "strip").has_value());
  }
  return set;
}

TEST_F(Ros2SubscriptionManagerTest, SubscribesWithSourceTypeAndForwardsTransformedBytes) {
  // A transform whose output type differs from the source type: the manager is
  // asked to subscribe with the ADVERTISED type and must use the source type.
  pj_bridge::TransformFactory f;
  f.accepts = [](const std::string& t) { return t == "std_msgs/msg/String"; };
  f.check_params = [](const nlohmann::json&) -> tl::expected<void, std::string> { return {}; };
  f.output_type = [](const std::string&) { return std::string("fake_msgs/msg/Out"); };
  f.output_schema = [](const std::string&, const std::string& s) { return s; };
  f.create = [](const std::string&, const nlohmann::json&) -> std::unique_ptr<pj_bridge::MessageTransform> {
    struct Marker : pj_bridge::MessageTransform {
      tl::expected<void, std::string> apply(std::span<const std::byte>, std::vector<std::byte>& out) override {
        out.assign(3, std::byte{0x5A});
        return {};
      }
    };
    return std::make_unique<Marker>();
  };
  auto set = pj_bridge::TransformSet::create(
                 nlohmann::json{{"transforms", {{{"match_topic", "/transform_me"}, {"transform", "marker"}}}}},
                 {{"marker", f}})
                 .value();
  ASSERT_NE(set->bind("/transform_me", "std_msgs/msg/String"), nullptr);  // what get_topics would have done

  pj_bridge::Ros2SubscriptionManager manager(node_, set);
  std::vector<std::byte> received;
  manager.set_message_callback(
      [&](const std::string&, std::shared_ptr<std::vector<std::byte>> data, uint64_t) { received = *data; });

  ASSERT_TRUE(manager.subscribe("/transform_me", "fake_msgs/msg/Out"));
  // publish one std_msgs/msg/String on /transform_me and spin until `received` is non-empty,
  // using the same publish-and-spin helper the neighbouring tests use.
  publish_string_and_spin("/transform_me", "hello", [&] { return !received.empty(); });

  EXPECT_EQ(received, std::vector<std::byte>(3, std::byte{0x5A}));
}
```

If the file has no publish-and-spin helper, add one next to the fixture:

```cpp
  void publish_string_and_spin(const std::string& topic, const std::string& text, const std::function<bool()>& done) {
    auto pub = node_->create_publisher<std_msgs::msg::String>(topic, 10);
    std_msgs::msg::String msg;
    msg.data = text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      pub->publish(msg);
      rclcpp::spin_some(node_);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
```

- [ ] **Step 5:** Build. Expected: FAIL (constructor signature, behaviour).

- [ ] **Step 6: Implement the hook.** `subscribe()` becomes:

```cpp
bool Ros2SubscriptionManager::subscribe(const std::string& topic_name, const std::string& topic_type) {
  // A transformed topic is advertised with its output type; subscribe with the real one.
  std::shared_ptr<BoundTransform> bound = transforms_ ? transforms_->find(topic_name) : nullptr;
  const std::string& source_type = bound ? bound->source_type : topic_type;

  Ros2MessageCallback ros2_callback =
      [this, bound](
          const std::string& topic, const std::shared_ptr<rclcpp::SerializedMessage>& msg, uint64_t receive_time_ns) {
        const auto& rcl_msg = msg->get_rcl_serialized_message();
        const auto* bytes = reinterpret_cast<const std::byte*>(rcl_msg.buffer);

        std::shared_ptr<std::vector<std::byte>> data;
        if (bound) {
          data = std::make_shared<std::vector<std::byte>>();
          if (!bound->run(topic, {bytes, rcl_msg.buffer_length}, *data)) {
            return;  // dropped and counted; never forward the untransformed bytes
          }
        } else {
          data = std::make_shared<std::vector<std::byte>>(bytes, bytes + rcl_msg.buffer_length);
        }

        MessageCallback cb;
        {
          std::lock_guard<std::mutex> lock(callback_mutex_);
          cb = callback_;
        }
        if (cb) {
          cb(topic, std::move(data), receive_time_ns);
        }
      };

  return inner_manager_.subscribe(topic_name, source_type, ros2_callback);
}
```

Constructor: `: inner_manager_(node, min_qos_depth, max_qos_depth), transforms_(std::move(transforms)) {}`. Remove `#include <optional>` if now unused.

- [ ] **Step 7:** Add `ros2/src/strip_transform.cpp` to `pj_bridge_ros2_lib`. `ros2/src/main.cpp` will not compile yet (it passes a bool): change that call temporarily to pass `nullptr` so the tree builds; Task 6 wires it properly.
- [ ] **Step 8:** Build, run tests `Ros2SubscriptionManager*:MessageStripper*`. Expected: PASS.
- [ ] **Step 9:** Commit: `feat(transform): ROS2 ingest hook; strip becomes a transform`.

---

### Task 6: Wire `transform_profile` and `strip_large_messages` in main

**Files:** Modify `ros2/src/main.cpp`.

- [ ] **Step 1:** Next to the other `declare_parameter` calls: `node->declare_parameter<std::string>("transform_profile", "");` and read it into `std::string transform_profile`. Add it to the "Configuration:" log line.
- [ ] **Step 2:** Add includes: `<fstream>`, `"pj_bridge/transform_set.hpp"`, `"pj_bridge/transforming_topic_source.hpp"`, `"pj_bridge_ros2/strip_transform.hpp"`, `"pj_bridge_ros2/message_stripper.hpp"`.
- [ ] **Step 3:** Inside the `try`, replace the `topic_source` / `sub_manager` construction with:

```cpp
    std::shared_ptr<pj_bridge::TopicSourceInterface> topic_source = std::make_shared<pj_bridge::Ros2TopicSource>(node);

    std::shared_ptr<pj_bridge::TransformSet> transforms;
    if (!transform_profile.empty() || strip_large_messages) {
      nlohmann::json profile = {{"transforms", nlohmann::json::array()}};
      if (!transform_profile.empty()) {
        std::ifstream file(transform_profile);
        if (!file) {
          RCLCPP_ERROR(node->get_logger(), "Cannot open transform_profile '%s'", transform_profile.c_str());
          rclcpp::shutdown();
          return 1;
        }
        profile = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        if (profile.is_discarded()) {
          RCLCPP_ERROR(node->get_logger(), "transform_profile '%s' is not valid JSON", transform_profile.c_str());
          rclcpp::shutdown();
          return 1;
        }
      }

      pj_bridge::TransformSet::FactoryMap factories;
      factories["strip"] = pj_bridge::make_strip_transform_factory();

      auto created = pj_bridge::TransformSet::create(profile, std::move(factories));
      if (!created) {
        RCLCPP_ERROR(node->get_logger(), "%s", created.error().c_str());
        rclcpp::shutdown();
        return 1;
      }
      transforms = *created;

      if (strip_large_messages) {  // after the profile's rules, so an explicit rule wins
        for (const auto& type : pj_bridge::MessageStripper::strippable_types()) {
          transforms->append_type_rule(type, "strip");
        }
      }
      topic_source = std::make_shared<pj_bridge::TransformingTopicSource>(topic_source, transforms);
    }

    auto sub_manager = std::make_shared<pj_bridge::Ros2SubscriptionManager>(
        node, transforms, static_cast<size_t>(min_qos_depth), static_cast<size_t>(max_qos_depth));
```

Check that `BridgeServer`'s constructor takes `std::shared_ptr<TopicSourceInterface>` (it does today via implicit conversion from `shared_ptr<Ros2TopicSource>`).

- [ ] **Step 4:** After the "Final statistics" log:

```cpp
    if (transforms) {
      const auto summary = transforms->stats_summary();
      if (!summary.empty()) {
        RCLCPP_INFO(node->get_logger(), "Transform statistics:%s", summary.c_str());
      }
    }
```

- [ ] **Step 5: Manual check.** Build. Then, in three terminals under `pixi run -e humble` with `install/setup.bash` sourced:
  1. `pj_bridge_ros2` with `--ros-args -p strip_large_messages:=true` → log shows no error; with `-p transform_profile:=/nonexistent.json` → exits 1 with "Cannot open"; with a file containing `{"transforms":[{"match_type":"sensor_msgs/msg/Image","transform":"nope"}]}` → exits 1 naming the unknown transform.
  2. Whole test suite still passes.
- [ ] **Step 6:** Commit: `feat(transform): transform_profile parameter; strip_large_messages as sugar`.

---

### Task 7: Cloudini transform

**Files:** Create `app/include/pj_bridge/cloudini_transform.hpp`, `app/src/cloudini_transform.cpp`, `tests/unit/test_cloudini_transform.cpp`. Modify `CMakeLists.txt`, `ros2/src/main.cpp`, `package.xml`.

- [ ] **Step 1: CMake.** After the IXWebSocket block:

```cmake
# Cloudini (optional): point cloud compression transform
option(PJ_BRIDGE_FETCH_CLOUDINI "Fetch Cloudini when it is not installed" ON)
find_package(cloudini_lib QUIET)
if(NOT cloudini_lib_FOUND AND PJ_BRIDGE_FETCH_CLOUDINI)
  include(FetchContent)
  set(CLOUDINI_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(CLOUDINI_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(cloudini
    URL https://github.com/facontidavide/cloudini/archive/refs/tags/1.3.0.tar.gz
    URL_HASH SHA256=26c9322d5a08694b9b24bbec975b7814b05cf7d4962327f35266b38bb15b5e66
    SOURCE_SUBDIR cloudini_lib
    EXCLUDE_FROM_ALL)
  # Cloudini switches to a SHARED ament package (install rules, ament_package())
  # when it finds ament_cmake. Hide it for the subdirectory: we want the plain
  # static library. Scoped to Cloudini's directory; our own ament state is untouched.
  set(BUILD_TESTING_SAVED ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake ON)
  FetchContent_MakeAvailable(cloudini)
  set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake OFF)
  set(BUILD_TESTING ${BUILD_TESTING_SAVED})
  if(NOT TARGET cloudini::cloudini_lib)
    add_library(cloudini::cloudini_lib ALIAS cloudini_lib)
  endif()
  set_target_properties(cloudini_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set(cloudini_lib_FOUND TRUE)
endif()
```

and, after `pj_bridge_app` is defined:

```cmake
if(cloudini_lib_FOUND)
  target_sources(pj_bridge_app PRIVATE app/src/cloudini_transform.cpp)
  target_link_libraries(pj_bridge_app PRIVATE cloudini::cloudini_lib)
  target_compile_definitions(pj_bridge_app PUBLIC PJ_BRIDGE_HAS_CLOUDINI)
  message(STATUS "Cloudini transform: enabled")
else()
  message(STATUS "Cloudini transform: disabled (cloudini_lib not found)")
endif()
```

The hash above was computed from the 1.3.0 tag tarball on 2026-09-20. After configuring, confirm in the CMake output that Cloudini did **not** take its ament branch (`grep -c "ament" build/pj_bridge/_deps/cloudini-build/CMakeCache.txt` should show no `ament_cmake_DIR` pointing to a real path) and that `libcloudini_lib.a` (static) was produced. The pixi environments ship CMake 4.x, so `EXCLUDE_FROM_ALL` in `FetchContent_Declare` (needs >= 3.28) is fine there; the Humble CI container has an older CMake — if configure fails on that keyword, remove it and wrap `FetchContent_MakeAvailable` with `set(CMAKE_SKIP_INSTALL_RULES ON)` / `OFF`. If the static `cloudini_lib` is linked PRIVATE into the static `pj_bridge_app`, the final executables still need it on their link line — CMake propagates this automatically for static libraries (`$<LINK_ONLY:...>`); confirm `pj_bridge_ros2` links.

- [ ] **Step 2:** `cloudini_transform.hpp`:

```cpp
#pragma once

#include "pj_bridge/message_transform.hpp"

namespace pj_bridge {

/// `cloudini` transform: sensor_msgs PointCloud2 -> point_cloud_interfaces
/// CompressedPointCloud2. Works on CDR bytes, so it serves any backend whose
/// PointCloud2 is CDR-encoded. Only declared when Cloudini is available.
///
/// Params: resolution (float, default 0.001), fields ({name: resolution}, 0
/// removes the field), viz_preprocessing (bool, default false).
TransformFactory make_cloudini_transform_factory();

}  // namespace pj_bridge
```

- [ ] **Step 3: Failing smoke test** — `tests/unit/test_cloudini_transform.cpp`, whole file inside `#ifdef PJ_BRIDGE_HAS_CLOUDINI`:

```cpp
#ifdef PJ_BRIDGE_HAS_CLOUDINI

#include <gtest/gtest.h>

#include <rclcpp/serialization.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>

#include "pj_bridge/cloudini_transform.hpp"

using namespace pj_bridge;

namespace {
std::vector<std::byte> make_cloud_cdr(size_t points) {
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

#endif  // PJ_BRIDGE_HAS_CLOUDINI
```

Add the file to the test target unconditionally (it is empty without the define). Build: expected link error for `make_cloudini_transform_factory`.

- [ ] **Step 4: Implementation** — `app/src/cloudini_transform.cpp`

```cpp
#include "pj_bridge/cloudini_transform.hpp"

#include <cloudini_lib/cloudini.hpp>
#include <cloudini_lib/ros_msg_utils.hpp>
// Defines non-inline `const char*` globals: include in this translation unit ONLY.
#include <cloudini_lib/ros_message_definitions.hpp>

namespace pj_bridge {
namespace {

constexpr const char* kPointCloud2 = "sensor_msgs/msg/PointCloud2";

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
  f.output_type = [](const std::string&) { return std::string(compressed_schema_name); };
  f.output_schema = [](const std::string&, const std::string&) { return std::string(compressed_schema_data); };
  f.create = [](const std::string&, const nlohmann::json& params) {
    return std::make_unique<CloudiniTransform>(params);
  };
  return f;
}

}  // namespace pj_bridge
```

Verify against the installed headers before building: the namespace of `compressed_schema_name` / `compressed_schema_data` in `ros_message_definitions.hpp`, the spelling `Cloudini::CompressionOption::NONE`, and that `toEncodingInfo` leaves `encoding_opt` at `LOSSY`. Known cost, accepted for v1: `convertPointCloud2ToCompressedCloud` builds a `PointcloudEncoder` per call and the result is copied once (compressed size) from `scratch_` into `out`. Removing both needs a small Cloudini API addition (byte-generic output, reusable encoder) — out of scope here.

- [ ] **Step 5:** Register it in `ros2/src/main.cpp` next to the strip factory:

```cpp
#ifdef PJ_BRIDGE_HAS_CLOUDINI
      factories["cloudini"] = pj_bridge::make_cloudini_transform_factory();
#endif
```

with `#ifdef PJ_BRIDGE_HAS_CLOUDINI` / `#include "pj_bridge/cloudini_transform.hpp"` / `#endif` among the includes. Without Cloudini, a profile naming `cloudini` already fails at startup with "unknown transform 'cloudini' (not built in, or misspelled)".

- [ ] **Step 6:** Build for Humble and Jazzy, run tests `CloudiniTransform*`. Expected: 2 PASS. Then the whole suite.
- [ ] **Step 7:** `package.xml`: do **not** add `<depend>cloudini_lib</depend>` until the key resolves on every target distro (check `rosdep resolve cloudini_lib --rosdistro humble|jazzy|lyrical`); record the result in the commit body.
- [ ] **Step 8:** Commit: `feat(transform): Cloudini point cloud compression`.

---

### Task 8: Documentation

**Files:** Modify `docs/API.md`, `README.md`, `CLAUDE.md`, `CHANGELOG.rst`. Create `docs/transform_profile.example.json`.

- [ ] **Step 1:** `docs/transform_profile.example.json`:

```json
{
  "transforms": [
    {
      "match_type": "sensor_msgs/msg/PointCloud2",
      "transform": "cloudini",
      "params": {"resolution": 0.001, "viz_preprocessing": false}
    }
  ]
}
```

- [ ] **Step 2:** `docs/API.md`: new section "Message transforms" covering: operator-only and off by default; a transformed topic is advertised with its **output** type and schema and the optional `source_type` (absent from `topics_changed` entries in this version); the `message_transforms` capability; the profile format, matching rules and first-match-wins; the two transforms and their params; every startup error; runtime failures drop the sample; `strip_large_messages: true` equals strip rules appended after the profile; clients need a `CompressedPointCloud2`-capable parser; Cloudini is lossy at `resolution`.
- [ ] **Step 3:** `README.md` and `CLAUDE.md`: add `transform_profile: ""` to the ROS2 configuration block; CLAUDE.md gets C++20, the new components (TransformSet, TransformingTopicSource, the hook replacing the stripper call), Cloudini as an optional dependency, and the updated test count.
- [ ] **Step 4:** `CHANGELOG.rst`: Forthcoming section listing the feature, C++20, and the `strip` runtime-failure behaviour change (drop instead of forwarding the original).
- [ ] **Step 5:** `pre-commit run -a`; commit: `docs: message transforms`.

---

### Task 9: Acceptance measurement

Uses the pinned rig and scripts described in `docs/perf/README.md` (bridge on cores 4-7, player 12-19, client 8-11, governor `performance`, 20 s windows from `/proc/<pid>/stat`). Large temporary files go on disk, never in `/tmp` (tmpfs).

- [ ] **Step 1:** 4-lidar bag, one client subscribed to all topics, three runs each: (a) no profile, (b) `docs/transform_profile.example.json`, (c) same with `viz_preprocessing: true`.
- [ ] **Step 2:** Record per configuration: bridge CPU (% of one core), wire bytes/s from the client, client-side per-topic message counts, and the shutdown "Transform statistics" (ratio, µs/sample).
- [ ] **Step 3:** Pass criteria: per-topic message counts in (b) and (c) equal (a) within one publish period; no transform drops. If counts fall short, ingest is being starved by encode time: stop and report — the fallback (per-topic worker, 1-deep latest-wins slot) is a separate design step, do not improvise it.
- [ ] **Step 4:** End to end: PlotJuggler built from `pj-official-plugins` branch `feat/compressed-pointcloud` connects, subscribes to a lidar topic and renders the cloud.
- [ ] **Step 5:** Add the numbers to `docs/perf/README.md`; commit: `docs(perf): message transform measurements`.
