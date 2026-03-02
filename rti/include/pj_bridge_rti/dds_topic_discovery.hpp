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
#include <dds/dds.hpp>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pj_bridge {

struct DdsTopicInfo {
  std::string name;
  std::string type_name;
};

class DdsTopicDiscovery {
 public:
  explicit DdsTopicDiscovery(const std::vector<int32_t>& domain_ids, const std::string& qos_profile_path = "");
  ~DdsTopicDiscovery();

  DdsTopicDiscovery(const DdsTopicDiscovery&) = delete;
  DdsTopicDiscovery& operator=(const DdsTopicDiscovery&) = delete;

  std::vector<DdsTopicInfo> get_topics() const;
  std::string get_schema(const std::string& topic_name) const;
  std::optional<dds::core::xtypes::StructType> get_type(const std::string& topic_name) const;
  std::optional<int32_t> get_domain_id(const std::string& topic_name) const;
  dds::domain::DomainParticipant get_participant(int32_t domain_id) const;

 private:
  class PublisherListener;

  struct DiscoveredTopic {
    std::string type_name;
    dds::core::xtypes::StructType struct_type;
    std::string schema_idl;
    int32_t domain_id;

    DiscoveredTopic(const std::string& tn, const dds::core::xtypes::StructType& st, const std::string& idl, int32_t did)
        : type_name(tn), struct_type(st), schema_idl(idl), domain_id(did) {}
  };

  void on_topic_discovered(
      const std::string& topic_name, const std::string& type_name, const dds::core::xtypes::StructType& struct_type,
      const std::string& schema_idl, int32_t domain_id);

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, DiscoveredTopic> topics_;
  std::unordered_map<int32_t, dds::domain::DomainParticipant> participants_;
  std::vector<std::shared_ptr<PublisherListener>> listeners_;
  std::vector<dds::sub::DataReader<dds::topic::PublicationBuiltinTopicData>> builtin_readers_;
};

}  // namespace pj_bridge
