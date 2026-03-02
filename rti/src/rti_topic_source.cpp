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

#include "pj_bridge_rti/rti_topic_source.hpp"

#include "pj_bridge/protocol_constants.hpp"

namespace pj_bridge {

RtiTopicSource::RtiTopicSource(DdsTopicDiscovery& discovery) : discovery_(discovery) {}

std::vector<TopicInfo> RtiTopicSource::get_topics() {
  auto dds_topics = discovery_.get_topics();

  std::vector<TopicInfo> result;
  result.reserve(dds_topics.size());

  for (const auto& t : dds_topics) {
    result.push_back({t.name, t.type_name});
  }

  return result;
}

std::string RtiTopicSource::get_schema(const std::string& topic_name) {
  return discovery_.get_schema(topic_name);
}

std::string RtiTopicSource::schema_encoding() const {
  return kSchemaEncodingOmgIdl;
}

}  // namespace pj_bridge
