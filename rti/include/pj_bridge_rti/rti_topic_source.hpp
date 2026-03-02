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

#include "pj_bridge/topic_source_interface.hpp"
#include "pj_bridge_rti/dds_topic_discovery.hpp"

namespace pj_bridge {

/**
 * @brief RTI Connext DDS implementation of TopicSourceInterface
 *
 * Thin adapter over DdsTopicDiscovery, mapping DdsTopicInfo → TopicInfo
 * and forwarding schema/encoding queries.
 */
class RtiTopicSource : public TopicSourceInterface {
 public:
  explicit RtiTopicSource(DdsTopicDiscovery& discovery);

  std::vector<TopicInfo> get_topics() override;
  std::string get_schema(const std::string& topic_name) override;
  std::string schema_encoding() const override;

 private:
  DdsTopicDiscovery& discovery_;
};

}  // namespace pj_bridge
