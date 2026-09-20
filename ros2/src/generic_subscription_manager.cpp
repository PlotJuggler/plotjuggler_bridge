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

#include "pj_bridge_ros2/generic_subscription_manager.hpp"

#include <rclcpp/version.h>

#include <algorithm>
#include <rclcpp/typesupport_helpers.hpp>

#include "pj_bridge/time_utils.hpp"

namespace pj_bridge {

namespace {
constexpr size_t kMaxDrainPerCallback = 1000;

// The executor hands over one message per subscription per wait cycle, and
// each cycle rebuilds the whole wait set (O(subscriptions)). This subscription
// drains whatever else its reader holds, so a burst costs one cycle instead of
// one per message. It overrides handle_serialized_message() rather than using
// a callback because only this hook sees the MessageInfo of the first message:
// with batched delivery, "now" is no longer a usable receive time.
class DrainingSubscription : public rclcpp::GenericSubscription {
 public:
  using Handler = std::function<void(const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t)>;

  DrainingSubscription(
      rclcpp::node_interfaces::NodeBaseInterface* node_base, std::shared_ptr<rcpputils::SharedLibrary> ts_lib,
      const std::string& topic_name, const std::string& topic_type, const rclcpp::QoS& qos, Handler handler)
      : rclcpp::GenericSubscription(
            node_base, std::move(ts_lib), topic_name, topic_type, qos, unused_callback(),
            rclcpp::SubscriptionOptions()),
        handler_(std::move(handler)) {}

  void handle_serialized_message(
      const std::shared_ptr<rclcpp::SerializedMessage>& message, const rclcpp::MessageInfo& info) override {
    handler_(message, receive_time_ns(info));
    // Bounded so a publisher faster than we can drain cannot starve other topics.
    for (size_t i = 0; i < kMaxDrainPerCallback; ++i) {
      auto extra = std::make_shared<rclcpp::SerializedMessage>();
      rclcpp::MessageInfo extra_info;
      try {
        if (!take_serialized(*extra, extra_info)) {
          break;
        }
      } catch (const std::exception& e) {
        RCLCPP_WARN(rclcpp::get_logger("pj_bridge"), "take failed on '%s': %s", get_topic_name(), e.what());
        break;
      }
      handler_(extra, receive_time_ns(extra_info));
    }
  }

 private:
  static uint64_t receive_time_ns(const rclcpp::MessageInfo& info) {
    // Not every RMW fills received_timestamp.
    const auto received = info.get_rmw_message_info().received_timestamp;
    return received > 0 ? static_cast<uint64_t>(received) : get_current_time_ns();
  }

#if RCLCPP_VERSION_MAJOR >= 28  // Jazzy+: the constructor takes an AnySubscriptionCallback
  static rclcpp::AnySubscriptionCallback<rclcpp::SerializedMessage, std::allocator<void>> unused_callback() {
    rclcpp::AnySubscriptionCallback<rclcpp::SerializedMessage, std::allocator<void>> callback;
    callback.set([](std::shared_ptr<const rclcpp::SerializedMessage>){});
    return callback;
  }
#else
  static std::function<void(std::shared_ptr<rclcpp::SerializedMessage>)> unused_callback() {
    return [](std::shared_ptr<rclcpp::SerializedMessage>) {};
  }
#endif

  Handler handler_;
};
}  // namespace

GenericSubscriptionManager::GenericSubscriptionManager(
    rclcpp::Node::SharedPtr node, size_t min_qos_depth, size_t max_qos_depth)
    : node_(node), min_qos_depth_(min_qos_depth), max_qos_depth_(max_qos_depth) {}

rclcpp::QoS GenericSubscriptionManager::adapt_qos(const std::string& topic_name) const {
  // Match the QoS the publishers actually offer (same policy as rosbag2):
  // a RELIABLE subscription never matches a BEST_EFFORT publisher (sensor
  // topics!) and a VOLATILE one misses latched (TRANSIENT_LOCAL) samples.
  rclcpp::QoS qos(100);

  // Historical default depth, also used as the per-publisher fallback when a
  // publisher's depth is unknown (see below).
  constexpr size_t kFallbackDepth = 100;

  auto publishers = node_->get_publishers_info_by_topic(topic_name);
  if (publishers.empty()) {
    // No info yet → same fallback the per-publisher rule below uses.
    qos.keep_last(std::min(kFallbackDepth, max_qos_depth_));
    return qos;
  }

  bool any_best_effort = false;
  bool all_transient_local = true;
  // Depth aggregation adapted from foxglove_bridge (MIT License,
  // Copyright (c) Foxglove Technologies Inc):
  // sum the publishers' history depths so a burst from every publisher
  // still fits, then clamp to [min_qos_depth, max_qos_depth].
  //
  // Deviations from foxglove_bridge:
  // - A publisher reporting depth 0 means the RMW didn't propagate depth
  //   through discovery (e.g. rmw_fastrtps_cpp reports 0 for everyone) or
  //   the publisher uses KEEP_ALL. It contributes kFallbackDepth instead of
  //   0 — assuming the historical generous default is safer than shrinking
  //   the queue and dropping messages on high-rate topics.
  // - The sum is saturating: each contribution is capped at the remaining
  //   headroom below max_qos_depth_ (total_depth <= max_qos_depth_ is a loop
  //   invariant), so the addition is structurally incapable of wrapping
  //   size_t no matter how many publishers report huge depths — even for
  //   extreme max_qos_depth_ values passed via the constructor API.
  size_t total_depth = 0;
  for (const auto& info : publishers) {
    const auto& profile = info.qos_profile();
    if (profile.reliability() == rclcpp::ReliabilityPolicy::BestEffort) {
      any_best_effort = true;
    }
    if (profile.durability() != rclcpp::DurabilityPolicy::TransientLocal) {
      all_transient_local = false;
    }
    const size_t publisher_depth = profile.depth() > 0 ? profile.depth() : kFallbackDepth;
    const size_t headroom = max_qos_depth_ - total_depth;  // total_depth <= max_qos_depth_ invariant
    total_depth += std::min(publisher_depth, headroom);
  }
  qos.keep_last(std::clamp(total_depth, min_qos_depth_, max_qos_depth_));

  // BEST_EFFORT matches both kinds of publisher; TRANSIENT_LOCAL only
  // matches if every publisher offers it.
  if (any_best_effort) {
    qos.best_effort();
  }
  if (all_transient_local) {
    qos.transient_local();
  }

  return qos;
}

bool GenericSubscriptionManager::subscribe(
    const std::string& topic_name, const std::string& topic_type, Ros2MessageCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);

  if (it != subscriptions_.end()) {
    it->second.reference_count++;
    return true;
  }

  try {
    rclcpp::QoS qos = adapt_qos(topic_name);
    bool transient_local = (qos.durability() == rclcpp::DurabilityPolicy::TransientLocal);

    auto subscription = std::make_shared<DrainingSubscription>(
        node_->get_node_base_interface().get(), rclcpp::get_typesupport_library(topic_type, "rosidl_typesupport_cpp"),
        topic_name, topic_type, qos,
        [topic_name, callback](const std::shared_ptr<rclcpp::SerializedMessage>& msg, uint64_t receive_time_ns) {
          callback(topic_name, msg, receive_time_ns);
        });
    node_->get_node_topics_interface()->add_subscription(subscription, nullptr);

    subscriptions_[topic_name] = SubscriptionInfo{subscription, 1, transient_local};

    return true;
  } catch (const std::exception& e) {
    RCLCPP_ERROR(
        node_->get_logger(), "Failed to create subscription for topic '%s' (type '%s'): %s", topic_name.c_str(),
        topic_type.c_str(), e.what());
    return false;
  }
}

bool GenericSubscriptionManager::unsubscribe(const std::string& topic_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);
  if (it == subscriptions_.end()) {
    return false;
  }

  if (it->second.reference_count == 0) {
    return false;
  }

  it->second.reference_count--;

  if (it->second.reference_count == 0) {
    subscriptions_.erase(it);
  }

  return true;
}

bool GenericSubscriptionManager::is_subscribed(const std::string& topic_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subscriptions_.find(topic_name) != subscriptions_.end();
}

size_t GenericSubscriptionManager::subscription_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return subscriptions_.size();
}

size_t GenericSubscriptionManager::get_reference_count(const std::string& topic_name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = subscriptions_.find(topic_name);
  if (it != subscriptions_.end()) {
    return it->second.reference_count;
  }

  return 0;
}

void GenericSubscriptionManager::unsubscribe_all() {
  std::lock_guard<std::mutex> lock(mutex_);
  subscriptions_.clear();
}

bool GenericSubscriptionManager::is_transient_local(const std::string& topic_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = subscriptions_.find(topic_name);
  if (it == subscriptions_.end()) {
    return false;
  }
  return it->second.transient_local;
}

}  // namespace pj_bridge
