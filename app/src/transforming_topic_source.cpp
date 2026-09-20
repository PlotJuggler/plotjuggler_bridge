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
