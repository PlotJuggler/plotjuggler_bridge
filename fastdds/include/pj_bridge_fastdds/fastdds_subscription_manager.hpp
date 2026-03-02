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
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicPubSubType.hpp>
#include <fastdds/dds/xtypes/dynamic_types/DynamicType.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "pj_bridge/subscription_manager_interface.hpp"
#include "pj_bridge_fastdds/fastdds_topic_source.hpp"

namespace pj_bridge {

// SubscriptionManagerInterface implementation for eProsima Fast DDS.
//
// Creates DataReaders with DynamicPubSubType for each subscribed topic.
// Incoming samples are deserialized into DynamicData and re-serialized to
// extract raw CDR bytes, which are passed to the message callback.
// Subscriptions are reference-counted.
class FastDdsSubscriptionManager : public SubscriptionManagerInterface {
 public:
  explicit FastDdsSubscriptionManager(FastDdsTopicSource& topic_source);
  ~FastDdsSubscriptionManager() override;

  FastDdsSubscriptionManager(const FastDdsSubscriptionManager&) = delete;
  FastDdsSubscriptionManager& operator=(const FastDdsSubscriptionManager&) = delete;

  void set_message_callback(MessageCallback callback) override;
  bool subscribe(const std::string& topic_name, const std::string& topic_type) override;
  bool unsubscribe(const std::string& topic_name) override;
  void unsubscribe_all() override;

 private:
  class InternalReaderListener;

  struct SubscriptionInfo {
    eprosima::fastdds::dds::DomainParticipant* participant;
    eprosima::fastdds::dds::Subscriber* subscriber;
    eprosima::fastdds::dds::Topic* topic;
    eprosima::fastdds::dds::DataReader* reader;
    std::shared_ptr<InternalReaderListener> listener;
    size_t reference_count;
  };

  void close_subscription(const std::string& topic_name, SubscriptionInfo& info);

  FastDdsTopicSource& topic_source_;
  MessageCallback callback_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, SubscriptionInfo> subscriptions_;
};

}  // namespace pj_bridge
