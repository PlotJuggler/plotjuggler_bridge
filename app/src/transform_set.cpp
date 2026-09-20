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

#include "pj_bridge/transform_set.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace pj_bridge {

bool BoundTransform::run(const std::string& topic, std::span<const std::byte> in, std::vector<std::byte>& out) {
  const auto start = std::chrono::steady_clock::now();
  tl::expected<void, std::string> result;
  try {
    result = transform->apply(in, out);
  } catch (const std::exception& e) {  // a codec throwing on bad input costs one sample, not the bridge
    result = tl::make_unexpected(std::string("exception: ") + e.what());
  }
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

  // ponytail: unmatched topics re-run every rule on each call (once per topic poll);
  // cache the misses too if topics x rules ever shows up in a profile.
  for (const auto& rule : rules_) {
    try {
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
              "Transform '{}' matched topic '{}' but does not accept type '{}'; rule skipped", rule.transform, topic,
              source_type);
        }
        continue;  // a later rule (e.g. the strip_large_messages sugar) may still apply
      }
      auto bound = std::make_shared<BoundTransform>();
      bound->source_type = source_type;
      bound->transform_name = rule.transform;
      bound->output_type = factory.output_type(source_type);
      bound->transform = factory.create(source_type, rule.params);
      if (!bound->transform) {
        throw std::runtime_error("factory returned no transform");
      }
      bindings_[topic] = bound;
      spdlog::info("Topic '{}' ({}) -> transform '{}' -> {}", topic, source_type, rule.transform, bound->output_type);
      return bound;
    } catch (const std::exception& e) {
      spdlog::error(
          "Transform '{}' could not be set up for '{}': {}; leaving it untransformed", rule.transform, topic, e.what());
      return nullptr;
    }
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
       << " drops, ratio " << (out > 0 ? in / out : 0.0) << ", " << (n ? bound->micros.load() / n : 0) << " us/sample";
  }
  return os.str();
}

}  // namespace pj_bridge
