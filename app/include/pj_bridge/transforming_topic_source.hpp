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
