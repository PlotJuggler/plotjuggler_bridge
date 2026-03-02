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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pj_bridge {

/// Callback invoked when a new message arrives on a subscribed topic.
/// @param topic_name  the topic the message was received on
/// @param cdr_data    serialized message payload (CDR-encoded)
/// @param timestamp_ns  nanoseconds since epoch (source-clock timestamp)
using MessageCallback = std::function<void(
    const std::string& topic_name, std::shared_ptr<std::vector<std::byte>> cdr_data, uint64_t timestamp_ns)>;

/// Abstract interface for managing middleware subscriptions with reference counting.
///
/// Implementations wrap a backend-specific subscription mechanism (e.g. rclcpp
/// GenericSubscription for ROS2, DDS DataReader for RTI) behind a uniform API.
///
/// Subscriptions are reference-counted: multiple clients can subscribe to the
/// same topic, and the underlying middleware subscription is only destroyed when
/// the last client unsubscribes. Each subscribe() call increments the ref count;
/// each unsubscribe() call decrements it.
///
/// A single global MessageCallback is set once at startup. All incoming messages
/// from all topics are delivered through this callback, which feeds them into
/// the MessageBuffer for later aggregation and delivery.
///
/// Thread safety: implementations must be safe to call from any thread.
class SubscriptionManagerInterface {
 public:
  virtual ~SubscriptionManagerInterface() = default;

  /// Set the global callback for all incoming messages. Must be called once
  /// before any subscribe() call.
  virtual void set_message_callback(MessageCallback callback) = 0;

  /// Subscribe to a topic, incrementing its reference count.
  /// If this is the first subscriber, the underlying middleware subscription is created.
  /// @param topic_name  fully-qualified topic name (e.g. "/sensor/imu")
  /// @param topic_type  message type string (e.g. "sensor_msgs/msg/Imu");
  ///                    required by ROS2, ignored by DDS backends.
  /// @return true on success, false if the subscription could not be created.
  virtual bool subscribe(const std::string& topic_name, const std::string& topic_type) = 0;

  /// Unsubscribe from a topic, decrementing its reference count.
  /// The underlying middleware subscription is destroyed when the count reaches zero.
  /// @return true if the topic was found and ref count decremented, false otherwise.
  virtual bool unsubscribe(const std::string& topic_name) = 0;

  /// Unsubscribe from all topics, destroying all underlying subscriptions.
  /// Called during shutdown.
  virtual void unsubscribe_all() = 0;
};

}  // namespace pj_bridge
