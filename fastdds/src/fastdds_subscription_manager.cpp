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

#include "pj_bridge_fastdds/fastdds_subscription_manager.hpp"

#include <spdlog/spdlog.h>

#include <cstring>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/qos/TopicQos.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicData.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicDataFactory.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <optional>

using namespace eprosima::fastdds::dds;
using eprosima::fastdds::rtps::SerializedPayload_t;

namespace pj_bridge {

// ---------------------------------------------------------------------------
// InternalReaderListener
// ---------------------------------------------------------------------------
class FastDdsSubscriptionManager::InternalReaderListener : public DataReaderListener {
 public:
  InternalReaderListener(
      const std::string& topic_name, FastDdsSubscriptionManager& manager, DynamicType::_ref_type dynamic_type)
      : topic_name_(topic_name),
        manager_(manager),
        dynamic_type_(dynamic_type),
        pub_sub_type_(dynamic_type),
        data_(DynamicDataFactory::get_instance()->create_data(dynamic_type)) {}

  void on_data_available(DataReader* reader) override {
    try {
      MessageCallback cb;
      {
        std::lock_guard<std::mutex> lock(manager_.mutex_);
        cb = manager_.callback_;
      }

      if (!cb) {
        return;
      }

      SampleInfo info;

      while (RETCODE_OK == reader->take_next_sample(&data_, &info)) {
        if (!info.valid_data) {
          continue;
        }

        // Re-serialize DynamicData to get raw CDR bytes
        uint32_t size =
            pub_sub_type_.calculate_serialized_size(&data_, DataRepresentationId_t::XCDR2_DATA_REPRESENTATION);
        if (payload_.max_size < size) {
          payload_.reserve(size);
        }
        payload_.length = 0;
        if (!pub_sub_type_.serialize(&data_, payload_, DataRepresentationId_t::XCDR2_DATA_REPRESENTATION)) {
          spdlog::warn("Failed to serialize DynamicData for '{}'", topic_name_);
          continue;
        }

        auto cdr_data = std::make_shared<std::vector<std::byte>>(payload_.length);
        std::memcpy(cdr_data->data(), payload_.data, payload_.length);

        uint64_t timestamp_ns =
            static_cast<uint64_t>(info.source_timestamp.seconds()) * 1'000'000'000ULL + info.source_timestamp.nanosec();

        cb(topic_name_, std::move(cdr_data), timestamp_ns);
      }
    } catch (const std::exception& e) {
      spdlog::error("Exception in DataReaderListener for '{}': {}", topic_name_, e.what());
    }
  }

 private:
  std::string topic_name_;
  FastDdsSubscriptionManager& manager_;
  DynamicType::_ref_type dynamic_type_;
  DynamicPubSubType pub_sub_type_;
  DynamicData::_ref_type data_;
  SerializedPayload_t payload_;
};

// ---------------------------------------------------------------------------
// FastDdsSubscriptionManager
// ---------------------------------------------------------------------------
FastDdsSubscriptionManager::FastDdsSubscriptionManager(FastDdsTopicSource& topic_source)
    : topic_source_(topic_source) {}

FastDdsSubscriptionManager::~FastDdsSubscriptionManager() {
  unsubscribe_all();
}

void FastDdsSubscriptionManager::set_message_callback(MessageCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

bool FastDdsSubscriptionManager::subscribe(const std::string& topic_name, const std::string& /*topic_type*/) {
  // Phase 1: Check for existing subscription (under lock)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscriptions_.find(topic_name);
    if (it != subscriptions_.end()) {
      it->second.reference_count++;
      spdlog::debug("Incremented ref count for '{}' to {}", topic_name, it->second.reference_count);
      return true;
    }
  }

  // Phase 2: Lookup and DDS entity creation (unlocked, avoids blocking on_data_available)
  auto dynamic_type = topic_source_.get_dynamic_type(topic_name);
  if (!dynamic_type) {
    spdlog::error("Topic '{}' not found in discovery", topic_name);
    return false;
  }

  auto domain_id_opt = topic_source_.get_domain_id(topic_name);
  if (!domain_id_opt) {
    spdlog::error("No domain ID for topic '{}'", topic_name);
    return false;
  }

  auto* participant = topic_source_.get_participant(*domain_id_opt);
  if (!participant) {
    spdlog::error("No participant for domain {}", *domain_id_opt);
    return false;
  }

  SubscriptionInfo new_sub{};
  new_sub.participant = participant;

  try {
    TypeSupport type_support(new DynamicPubSubType(dynamic_type));
    type_support.register_type(participant);

    new_sub.topic = participant->create_topic(topic_name, type_support.get_type_name(), TOPIC_QOS_DEFAULT);
    if (!new_sub.topic) {
      spdlog::error("Failed to create topic for '{}'", topic_name);
      return false;
    }

    new_sub.subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (!new_sub.subscriber) {
      participant->delete_topic(new_sub.topic);
      spdlog::error("Failed to create subscriber for '{}'", topic_name);
      return false;
    }

    new_sub.listener = std::make_shared<InternalReaderListener>(topic_name, *this, dynamic_type);
    new_sub.reader =
        new_sub.subscriber->create_datareader(new_sub.topic, DATAREADER_QOS_DEFAULT, new_sub.listener.get());
    if (!new_sub.reader) {
      participant->delete_subscriber(new_sub.subscriber);
      participant->delete_topic(new_sub.topic);
      spdlog::error("Failed to create datareader for '{}'", topic_name);
      return false;
    }

    new_sub.reference_count = 1;

    // Phase 3: Insert into map (re-acquire lock, double-check for concurrent subscribe)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = subscriptions_.find(topic_name);
      if (it != subscriptions_.end()) {
        // Another thread subscribed while we were creating DDS entities
        it->second.reference_count++;
        spdlog::debug("Incremented ref count for '{}' to {} (concurrent)", topic_name, it->second.reference_count);
        // Clean up the entities we just created (outside lock below)
        close_subscription(topic_name, new_sub);
        return true;
      }
      subscriptions_.emplace(topic_name, std::move(new_sub));
    }

    spdlog::info(
        "Subscribed to '{}' (type: '{}', domain: {})", topic_name, type_support.get_type_name(), *domain_id_opt);
    return true;
  } catch (const std::exception& e) {
    spdlog::error("Failed to subscribe to '{}': {}", topic_name, e.what());
    return false;
  }
}

bool FastDdsSubscriptionManager::unsubscribe(const std::string& topic_name) {
  std::optional<SubscriptionInfo> to_close;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = subscriptions_.find(topic_name);
    if (it == subscriptions_.end()) {
      return false;
    }

    if (it->second.reference_count == 0) {
      return false;
    }

    it->second.reference_count--;
    spdlog::debug("Decremented ref count for '{}' to {}", topic_name, it->second.reference_count);

    if (it->second.reference_count == 0) {
      to_close.emplace(std::move(it->second));
      subscriptions_.erase(it);
    }
  }

  if (to_close) {
    close_subscription(topic_name, *to_close);
    spdlog::info("Unsubscribed from '{}' (reader destroyed)", topic_name);
  }

  return true;
}

void FastDdsSubscriptionManager::unsubscribe_all() {
  std::unordered_map<std::string, SubscriptionInfo> to_close;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    to_close.swap(subscriptions_);
  }

  spdlog::debug("[shutdown] unsubscribe_all: {} subscriptions to close", to_close.size());
  for (auto& [name, info] : to_close) {
    spdlog::debug("[shutdown] Closing reader for '{}'...", name);
    close_subscription(name, info);
    spdlog::debug("[shutdown] Closed '{}'", name);
  }
  spdlog::debug("[shutdown] unsubscribe_all complete");
}

void FastDdsSubscriptionManager::close_subscription(const std::string& topic_name, SubscriptionInfo& info) {
  try {
    info.reader->set_listener(nullptr);
    info.subscriber->delete_datareader(info.reader);
    info.participant->delete_subscriber(info.subscriber);
    info.participant->delete_topic(info.topic);
  } catch (const std::exception& e) {
    spdlog::warn("Error cleaning up reader for '{}': {}", topic_name, e.what());
  } catch (...) {
    spdlog::warn("Unknown error cleaning up reader for '{}'", topic_name);
  }
}

}  // namespace pj_bridge
