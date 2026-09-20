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

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

  /// One line per bound topic that processed at least one sample.
  std::string stats_summary() const;

 private:
  struct Rule {
    std::string match_type;  // mandatory
    std::optional<std::regex> match_topic;
    std::string transform;
    nlohmann::json params = nlohmann::json::object();
  };

  TransformSet() = default;
  tl::expected<void, std::string> add_rule(Rule rule);

  FactoryMap factories_;
  std::vector<Rule> rules_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<BoundTransform>> bindings_;
  std::unordered_set<std::string> setup_failed_;  // topics whose setup error was already reported
};

}  // namespace pj_bridge
