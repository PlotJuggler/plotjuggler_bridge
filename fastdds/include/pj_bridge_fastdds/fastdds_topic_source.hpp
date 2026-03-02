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

#include <cstdint>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicType.hpp>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_bridge/topic_source_interface.hpp"

namespace pj_bridge {

// Thread-safe topic discovery + schema provider for eProsima Fast DDS.
//
// Creates one DomainParticipant per domain ID, attaches a
// DomainParticipantListener to discover remote DataWriters, resolves their
// DynamicType from the TypeObject registry, and caches the OMG IDL schema.
class FastDdsTopicSource : public TopicSourceInterface {
 public:
  explicit FastDdsTopicSource(const std::vector<int32_t>& domain_ids);
  ~FastDdsTopicSource() override;

  FastDdsTopicSource(const FastDdsTopicSource&) = delete;
  FastDdsTopicSource& operator=(const FastDdsTopicSource&) = delete;

  // TopicSourceInterface
  std::vector<TopicInfo> get_topics() override;
  std::string get_schema(const std::string& topic_name) override;
  std::string schema_encoding() const override;

  // Used by FastDdsSubscriptionManager
  eprosima::fastdds::dds::DynamicType::_ref_type get_dynamic_type(const std::string& topic_name) const;
  eprosima::fastdds::dds::DomainParticipant* get_participant(int32_t domain_id) const;
  std::optional<int32_t> get_domain_id(const std::string& topic_name) const;

 private:
  class ParticipantListener;

  struct DiscoveredTopic {
    eprosima::fastdds::dds::DynamicType::_ref_type dynamic_type;
    std::string schema_idl;
    int32_t domain_id;
  };

  void on_topic_discovered(
      const std::string& topic_name, eprosima::fastdds::dds::DynamicType::_ref_type dynamic_type,
      const std::string& schema_idl, int32_t domain_id);

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, DiscoveredTopic> topics_;
  std::unordered_map<int32_t, eprosima::fastdds::dds::DomainParticipant*> participants_;
  std::vector<std::shared_ptr<ParticipantListener>> listeners_;
};

}  // namespace pj_bridge
