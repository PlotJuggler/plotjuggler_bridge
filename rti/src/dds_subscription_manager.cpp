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

#include "pj_bridge_rti/dds_subscription_manager.hpp"

#include <spdlog/spdlog.h>

namespace pj_bridge {

class DdsSubscriptionManager::InternalReaderListener
    : public dds::sub::NoOpDataReaderListener<dds::core::xtypes::DynamicData> {
 public:
  InternalReaderListener(const std::string& topic_name, DdsSubscriptionManager& manager)
      : topic_name_(topic_name), manager_(manager) {}

  void on_data_available(dds::sub::DataReader<dds::core::xtypes::DynamicData>& reader) override {
    try {
      auto samples = reader.take();

      MessageCallback cb;
      {
        std::lock_guard<std::mutex> lock(manager_.mutex_);
        cb = manager_.callback_;
      }

      if (!cb) {
        return;
      }

      for (const auto& sample : samples) {
        if (!sample.info().valid()) {
          continue;
        }

        auto cdr_buffer = sample.data().get_cdr_buffer();
        const auto* raw_data = reinterpret_cast<const std::byte*>(cdr_buffer.first);
        uint32_t data_size = cdr_buffer.second;

        auto cdr_data = std::make_shared<std::vector<std::byte>>(raw_data, raw_data + data_size);

        const auto& source_ts = sample.info().source_timestamp();
        uint64_t timestamp_ns = (static_cast<uint64_t>(source_ts.sec()) * 1'000'000'000ULL) + source_ts.nanosec();

        cb(topic_name_, std::move(cdr_data), timestamp_ns);
      }
    } catch (const std::exception& e) {
      spdlog::error("Exception in DataReaderListener for '{}': {}", topic_name_, e.what());
    }
  }

 private:
  std::string topic_name_;
  DdsSubscriptionManager& manager_;
};

DdsSubscriptionManager::DdsSubscriptionManager(DdsTopicDiscovery& discovery) : discovery_(discovery) {}

DdsSubscriptionManager::~DdsSubscriptionManager() {
  unsubscribe_all();
}

void DdsSubscriptionManager::set_message_callback(MessageCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

bool DdsSubscriptionManager::subscribe(const std::string& topic_name, const std::string& /*topic_type*/) {
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
  auto struct_type_opt = discovery_.get_type(topic_name);
  if (!struct_type_opt) {
    spdlog::error("Topic '{}' not found in discovery", topic_name);
    return false;
  }

  auto domain_id_opt = discovery_.get_domain_id(topic_name);
  if (!domain_id_opt) {
    spdlog::error("No domain ID for topic '{}'", topic_name);
    return false;
  }

  auto& struct_type = *struct_type_opt;

  auto participant_opt = discovery_.get_participant(*domain_id_opt);
  if (!participant_opt) {
    spdlog::error("No participant for domain {}", *domain_id_opt);
    return false;
  }

  try {
    auto& participant = *participant_opt;

    rti::core::xtypes::DynamicDataTypeSerializationProperty ser_prop;
    ser_prop.skip_deserialization(true);

    rti::domain::register_type(participant, struct_type.name(), struct_type, ser_prop);

    dds::topic::Topic<dds::core::xtypes::DynamicData> topic(participant, topic_name, struct_type.name());

    dds::sub::Subscriber subscriber(participant);
    dds::sub::DataReader<dds::core::xtypes::DynamicData> reader(subscriber, topic);

    auto listener = std::make_shared<InternalReaderListener>(topic_name, *this);
    reader.set_listener(listener);

    // Phase 3: Insert into map (re-acquire lock, double-check for concurrent subscribe)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = subscriptions_.find(topic_name);
      if (it != subscriptions_.end()) {
        // Another thread subscribed while we were creating DDS entities
        it->second.reference_count++;
        spdlog::debug("Incremented ref count for '{}' to {} (concurrent)", topic_name, it->second.reference_count);
        // Clean up the entities we just created
        reader.set_listener(nullptr);
        reader.close();
        subscriber.close();
        return true;
      }
      subscriptions_.emplace(
          std::piecewise_construct, std::forward_as_tuple(topic_name),
          std::forward_as_tuple(subscriber, reader, listener, 1));
    }

    spdlog::info("Subscribed to '{}' (type: '{}', domain: {})", topic_name, struct_type.name(), *domain_id_opt);
    return true;
  } catch (const std::exception& e) {
    spdlog::error("Failed to subscribe to '{}': {}", topic_name, e.what());
    return false;
  }
}

bool DdsSubscriptionManager::unsubscribe(const std::string& topic_name) {
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
    close_reader(topic_name, *to_close);
    spdlog::info("Unsubscribed from '{}' (reader destroyed)", topic_name);
  }

  return true;
}

void DdsSubscriptionManager::unsubscribe_all() {
  std::unordered_map<std::string, SubscriptionInfo> to_close;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    to_close.swap(subscriptions_);
  }

  spdlog::debug("[shutdown] unsubscribe_all: {} subscriptions to close", to_close.size());
  for (auto& [name, info] : to_close) {
    spdlog::debug("[shutdown] Closing reader for '{}'...", name);
    close_reader(name, info);
    spdlog::debug("[shutdown] Closed '{}'", name);
  }
  spdlog::debug("[shutdown] unsubscribe_all complete");
}

void DdsSubscriptionManager::close_reader(const std::string& topic_name, SubscriptionInfo& info) {
  try {
    info.reader.set_listener(nullptr);
    info.reader.close();
    info.subscriber.close();
  } catch (const std::exception& e) {
    spdlog::warn("Error cleaning up reader for '{}': {}", topic_name, e.what());
  } catch (...) {
    spdlog::warn("Unknown error cleaning up reader for '{}'", topic_name);
  }
}

}  // namespace pj_bridge
