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

#include "pj_bridge_rti/dds_topic_discovery.hpp"

#include <spdlog/spdlog.h>

namespace pj_bridge {

class DdsTopicDiscovery::PublisherListener
    : public dds::sub::NoOpDataReaderListener<dds::topic::PublicationBuiltinTopicData> {
 public:
  PublisherListener(DdsTopicDiscovery& discovery, int32_t domain_id) : discovery_(discovery), domain_id_(domain_id) {}

  void on_data_available(dds::sub::DataReader<dds::topic::PublicationBuiltinTopicData>& reader) override {
    try {
      auto samples = reader.select().state(dds::sub::status::DataState::new_instance()).take();

      for (const auto& sample : samples) {
        if (!sample.info().valid()) {
          continue;
        }

        const auto& topic_name = sample.data().topic_name();
        const auto& type = sample.data().extensions().type();

        if (!type) {
          spdlog::debug("Topic '{}' discovered without type info in domain {}", topic_name.to_std_string(), domain_id_);
          continue;
        }

        const auto& type_name = type->name();

        if (type->kind() != dds::core::xtypes::TypeKind::STRUCTURE_TYPE) {
          spdlog::debug("Topic '{}' has non-struct type '{}', skipping", topic_name.to_std_string(), type_name);
          continue;
        }

        auto struct_type = static_cast<const dds::core::xtypes::StructType&>(type.get());

        rti::core::xtypes::DynamicTypePrintFormatProperty print_format{
            0, false, rti::core::xtypes::DynamicTypePrintKind::idl, true};
        std::string schema_idl = rti::core::xtypes::to_string(type.get(), print_format);

        spdlog::info(
            "Discovered topic '{}' (type: '{}') in domain {}", topic_name.to_std_string(), type_name, domain_id_);

        discovery_.on_topic_discovered(topic_name.to_std_string(), type_name, struct_type, schema_idl, domain_id_);
      }
    } catch (const std::exception& e) {
      spdlog::error("Exception in PublisherListener on domain {}: {}", domain_id_, e.what());
    }
  }

 private:
  DdsTopicDiscovery& discovery_;
  int32_t domain_id_;
};

DdsTopicDiscovery::DdsTopicDiscovery(const std::vector<int32_t>& domain_ids, const std::string& qos_profile_path) {
  if (!qos_profile_path.empty()) {
    rti::core::QosProviderParams params;
    params.url_profile({qos_profile_path});
    rti::core::default_qos_provider_params(params);
    spdlog::info("Set default QoS profile from '{}'", qos_profile_path);
  }

  for (int32_t domain_id : domain_ids) {
    spdlog::info("Creating DomainParticipant for domain {}", domain_id);

    dds::domain::DomainParticipant participant(domain_id);
    participants_.emplace(domain_id, participant);

    auto listener = std::make_shared<PublisherListener>(*this, domain_id);
    listeners_.push_back(listener);

    dds::sub::Subscriber builtin_subscriber = dds::sub::builtin_subscriber(participant);
    std::vector<dds::sub::DataReader<dds::topic::PublicationBuiltinTopicData>> readers;
    dds::sub::find<dds::sub::DataReader<dds::topic::PublicationBuiltinTopicData>>(
        builtin_subscriber, dds::topic::publication_topic_name(), std::back_inserter(readers));

    if (readers.empty()) {
      spdlog::error("No built-in publication DataReader found for domain {}", domain_id);
      continue;
    }

    readers[0].set_listener(listener);
    builtin_readers_.push_back(std::move(readers[0]));

    spdlog::info("Topic discovery active on domain {}", domain_id);
  }
}

DdsTopicDiscovery::~DdsTopicDiscovery() {
  spdlog::debug("[shutdown] ~DdsTopicDiscovery: clearing {} builtin readers", builtin_readers_.size());
  for (auto& reader : builtin_readers_) {
    try {
      reader.set_listener(nullptr);
    } catch (const std::exception& e) {
      spdlog::warn("Error clearing builtin reader listener: {}", e.what());
    } catch (...) {
      spdlog::warn("Unknown error clearing builtin reader listener");
    }
  }
  builtin_readers_.clear();
  listeners_.clear();
  spdlog::debug("[shutdown] ~DdsTopicDiscovery: builtin readers cleared");

  for (auto& [domain_id, participant] : participants_) {
    try {
      spdlog::debug("[shutdown] Closing DomainParticipant for domain {}...", domain_id);
      participant.close();
      spdlog::debug("[shutdown] DomainParticipant for domain {} closed", domain_id);
    } catch (const std::exception& e) {
      spdlog::warn("Error closing participant for domain {}: {}", domain_id, e.what());
    } catch (...) {
      spdlog::warn("Unknown error closing participant for domain {}", domain_id);
    }
  }
  spdlog::debug("[shutdown] ~DdsTopicDiscovery complete");
}

void DdsTopicDiscovery::on_topic_discovered(
    const std::string& topic_name, const std::string& type_name, const dds::core::xtypes::StructType& struct_type,
    const std::string& schema_idl, int32_t domain_id) {
  std::unique_lock<std::shared_mutex> lock(mutex_);

  if (topics_.find(topic_name) != topics_.end()) {
    return;
  }

  topics_.emplace(topic_name, DiscoveredTopic{type_name, struct_type, schema_idl, domain_id});
  spdlog::info("Cached topic '{}' (type: '{}', domain: {})", topic_name, type_name, domain_id);
}

std::vector<DdsTopicInfo> DdsTopicDiscovery::get_topics() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  std::vector<DdsTopicInfo> result;
  result.reserve(topics_.size());

  for (const auto& [name, topic] : topics_) {
    result.push_back({name, topic.type_name});
  }

  return result;
}

std::string DdsTopicDiscovery::get_schema(const std::string& topic_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = topics_.find(topic_name);
  if (it == topics_.end()) {
    return "";
  }

  return it->second.schema_idl;
}

std::optional<dds::core::xtypes::StructType> DdsTopicDiscovery::get_type(const std::string& topic_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = topics_.find(topic_name);
  if (it == topics_.end()) {
    return std::nullopt;
  }

  return it->second.struct_type;
}

std::optional<int32_t> DdsTopicDiscovery::get_domain_id(const std::string& topic_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = topics_.find(topic_name);
  if (it == topics_.end()) {
    return std::nullopt;
  }

  return it->second.domain_id;
}

dds::domain::DomainParticipant DdsTopicDiscovery::get_participant(int32_t domain_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = participants_.find(domain_id);
  if (it == participants_.end()) {
    throw std::runtime_error("No participant for domain " + std::to_string(domain_id));
  }
  return it->second;
}

}  // namespace pj_bridge
