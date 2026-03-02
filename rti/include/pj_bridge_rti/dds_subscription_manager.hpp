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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_bridge_rti/dds_topic_discovery.hpp"

namespace pj_bridge {

class DdsSubscriptionManager {
 public:
  using DdsMessageCallback = std::function<void(
      const std::string& topic_name, std::shared_ptr<std::vector<std::byte>> cdr_data, uint64_t timestamp_ns)>;

  explicit DdsSubscriptionManager(DdsTopicDiscovery& discovery);
  ~DdsSubscriptionManager();

  DdsSubscriptionManager(const DdsSubscriptionManager&) = delete;
  DdsSubscriptionManager& operator=(const DdsSubscriptionManager&) = delete;

  void set_message_callback(DdsMessageCallback callback);

  bool subscribe(const std::string& topic_name);
  bool unsubscribe(const std::string& topic_name);
  size_t ref_count(const std::string& topic_name) const;
  void unsubscribe_all();

 private:
  class InternalReaderListener;

  struct SubscriptionInfo {
    dds::sub::Subscriber subscriber;
    dds::sub::DataReader<dds::core::xtypes::DynamicData> reader;
    std::shared_ptr<InternalReaderListener> listener;
    size_t reference_count;

    SubscriptionInfo(
        dds::sub::Subscriber sub, dds::sub::DataReader<dds::core::xtypes::DynamicData> rdr,
        std::shared_ptr<InternalReaderListener> lst, size_t rc)
        : subscriber(std::move(sub)), reader(std::move(rdr)), listener(std::move(lst)), reference_count(rc) {}
  };

  DdsTopicDiscovery& discovery_;
  DdsMessageCallback callback_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, SubscriptionInfo> subscriptions_;
};

}  // namespace pj_bridge
