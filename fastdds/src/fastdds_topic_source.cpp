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

#include "pj_bridge_fastdds/fastdds_topic_source.hpp"

#include <spdlog/spdlog.h>

#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeBuilder.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicTypeBuilderFactory.hpp>
#include <fastdds/dds/xtypes/type_representation/ITypeObjectRegistry.hpp>
#include <fastdds/dds/xtypes/type_representation/TypeObject.hpp>
#include <fastdds/dds/xtypes/utils.hpp>
#include <sstream>

#include "pj_bridge/protocol_constants.hpp"

using namespace eprosima::fastdds::dds;
using eprosima::fastdds::rtps::WriterDiscoveryStatus;

namespace pj_bridge {

// ---------------------------------------------------------------------------
// ParticipantListener — handles on_data_writer_discovery callbacks
// ---------------------------------------------------------------------------
class FastDdsTopicSource::ParticipantListener : public DomainParticipantListener {
 public:
  ParticipantListener(FastDdsTopicSource& source, int32_t domain_id) : source_(source), domain_id_(domain_id) {}

  void on_data_writer_discovery(
      DomainParticipant* /*participant*/, WriterDiscoveryStatus reason, const PublicationBuiltinTopicData& info,
      bool& should_be_ignored) override {
    should_be_ignored = false;

    if (reason != WriterDiscoveryStatus::DISCOVERED_WRITER) {
      return;
    }

    try {
      std::string topic_name(info.topic_name);

      // Skip if already cached (fast check before heavy TypeObject work)
      {
        std::shared_lock<std::shared_mutex> lock(source_.mutex_);
        if (source_.topics_.count(topic_name) > 0) {
          return;
        }
      }

      // Get TypeObject from the global registry via the type identifier in the
      // discovery info's TypeInformation
      xtypes::TypeObject type_object;
      auto& registry = DomainParticipantFactory::get_instance()->type_object_registry();
      const auto& type_id = info.type_information.type_information.complete().typeid_with_size().type_id();

      if (RETCODE_OK != registry.get_type_object(type_id, type_object)) {
        spdlog::debug("Topic '{}' discovered but TypeObject not yet in registry (domain {})", topic_name, domain_id_);
        return;
      }

      // Build DynamicType
      auto builder = DynamicTypeBuilderFactory::get_instance()->create_type_w_type_object(type_object);
      if (!builder) {
        spdlog::warn("Failed to create DynamicTypeBuilder for '{}' (domain {})", topic_name, domain_id_);
        return;
      }
      DynamicType::_ref_type dynamic_type = builder->build();
      if (!dynamic_type) {
        spdlog::warn("Failed to build DynamicType for '{}' (domain {})", topic_name, domain_id_);
        return;
      }

      // Only accept struct types (like RTI backend)
      if (dynamic_type->get_kind() != TK_STRUCTURE) {
        spdlog::debug(
            "Topic '{}' has non-struct type '{}', skipping", topic_name, std::string(dynamic_type->get_name()));
        return;
      }

      // Generate OMG IDL schema
      std::ostringstream oss;
      if (RETCODE_OK != idl_serialize(dynamic_type, oss)) {
        spdlog::warn("Failed to serialize IDL for '{}' (domain {})", topic_name, domain_id_);
        return;
      }

      spdlog::info(
          "Discovered topic '{}' (type: '{}') in domain {}", topic_name, std::string(dynamic_type->get_name()),
          domain_id_);

      source_.on_topic_discovered(topic_name, dynamic_type, oss.str(), domain_id_);

    } catch (const std::exception& e) {
      spdlog::error("Exception in ParticipantListener on domain {}: {}", domain_id_, e.what());
    }
  }

 private:
  FastDdsTopicSource& source_;
  int32_t domain_id_;
};

// ---------------------------------------------------------------------------
// FastDdsTopicSource
// ---------------------------------------------------------------------------
FastDdsTopicSource::FastDdsTopicSource(const std::vector<int32_t>& domain_ids) {
  auto* factory = DomainParticipantFactory::get_instance();

  for (int32_t domain_id : domain_ids) {
    spdlog::info("Creating DomainParticipant for domain {}", domain_id);

    DomainParticipantQos pqos;
    pqos.name("pj_bridge_fastdds_" + std::to_string(domain_id));
    // TypeLookup service is enabled by default in Fast DDS 3.4

    auto listener = std::make_shared<ParticipantListener>(*this, domain_id);
    listeners_.push_back(listener);

    DomainParticipant* participant = factory->create_participant(domain_id, pqos, listener.get(), StatusMask::none());

    if (!participant) {
      spdlog::error("Failed to create DomainParticipant for domain {}", domain_id);
      continue;
    }

    participants_.emplace(domain_id, participant);
    spdlog::info("Topic discovery active on domain {}", domain_id);
  }
}

FastDdsTopicSource::~FastDdsTopicSource() {
  auto* factory = DomainParticipantFactory::get_instance();

  for (auto& [domain_id, participant] : participants_) {
    try {
      spdlog::debug("[shutdown] Deleting DomainParticipant for domain {}...", domain_id);
      // Remove listener before deletion to prevent callbacks during teardown
      participant->set_listener(nullptr);
      factory->delete_participant(participant);
      spdlog::debug("[shutdown] DomainParticipant for domain {} deleted", domain_id);
    } catch (const std::exception& e) {
      spdlog::warn("Error deleting participant for domain {}: {}", domain_id, e.what());
    } catch (...) {
      spdlog::warn("Unknown error deleting participant for domain {}", domain_id);
    }
  }
  listeners_.clear();
  spdlog::debug("[shutdown] ~FastDdsTopicSource complete");
}

void FastDdsTopicSource::on_topic_discovered(
    const std::string& topic_name, DynamicType::_ref_type dynamic_type, const std::string& schema_idl,
    int32_t domain_id) {
  std::unique_lock<std::shared_mutex> lock(mutex_);

  if (topics_.find(topic_name) != topics_.end()) {
    return;
  }

  topics_.emplace(topic_name, DiscoveredTopic{dynamic_type, schema_idl, domain_id});
  spdlog::info(
      "Cached topic '{}' (type: '{}', domain: {})", topic_name, std::string(dynamic_type->get_name()), domain_id);
}

std::vector<TopicInfo> FastDdsTopicSource::get_topics() {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  std::vector<TopicInfo> result;
  result.reserve(topics_.size());

  for (const auto& [name, topic] : topics_) {
    result.push_back({name, std::string(topic.dynamic_type->get_name())});
  }

  return result;
}

std::string FastDdsTopicSource::get_schema(const std::string& topic_name) {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = topics_.find(topic_name);
  if (it == topics_.end()) {
    return "";
  }

  return it->second.schema_idl;
}

std::string FastDdsTopicSource::schema_encoding() const {
  return kSchemaEncodingOmgIdl;
}

DynamicType::_ref_type FastDdsTopicSource::get_dynamic_type(const std::string& topic_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = topics_.find(topic_name);
  if (it == topics_.end()) {
    return nullptr;
  }

  return it->second.dynamic_type;
}

DomainParticipant* FastDdsTopicSource::get_participant(int32_t domain_id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = participants_.find(domain_id);
  if (it == participants_.end()) {
    return nullptr;
  }
  return it->second;
}

std::optional<int32_t> FastDdsTopicSource::get_domain_id(const std::string& topic_name) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);

  auto it = topics_.find(topic_name);
  if (it == topics_.end()) {
    return std::nullopt;
  }

  return it->second.domain_id;
}

}  // namespace pj_bridge
