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

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <std_msgs/msg/string.hpp>
#include <thread>

#include "fake_transform.hpp"
#include "pj_bridge/transform_set.hpp"
#include "pj_bridge_ros2/ros2_subscription_manager.hpp"
#include "pj_bridge_ros2/strip_transform.hpp"

using namespace pj_bridge;

namespace {

// A TransformSet with the `strip` rule appended for every strippable type,
// as strip_large_messages=true wires it up in main.cpp.
std::shared_ptr<TransformSet> strip_everything() {
  auto set = TransformSet::create(
                 nlohmann::json{{"transforms", nlohmann::json::array()}}, {{"strip", make_strip_transform_factory()}})
                 .value();
  EXPECT_TRUE(append_strip_rules(*set).has_value());
  return set;
}

// Publish `msg` and spin until `done()` or 5 s have passed.
template <typename MsgT>
void publish_until(
    const rclcpp::Node::SharedPtr& node, const typename rclcpp::Publisher<MsgT>::SharedPtr& publisher, const MsgT& msg,
    const std::function<bool()>& done) {
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    publisher->publish(msg);
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Publish an Image with a large data payload through the manager and return
// the size of the serialized message delivered to the bridge callback.
// `transforms` is nullptr for "no transform configured".
size_t roundtrip_image_bytes(std::shared_ptr<TransformSet> transforms, bool use_default_config) {
  auto node = std::make_shared<rclcpp::Node>(use_default_config ? "test_strip_default" : "test_strip_on");
  auto manager = use_default_config ? std::make_shared<Ros2SubscriptionManager>(node)
                                    : std::make_shared<Ros2SubscriptionManager>(node, transforms);

  std::atomic<size_t> received_size{0};
  manager->set_message_callback(
      [&received_size](const std::string&, std::shared_ptr<std::vector<std::byte>> data, uint64_t) {
        received_size = data->size();
      });

  const std::string topic = "/strip_test_image_" + std::string(node->get_name());
  if (transforms) {
    // ASSERT_* requires a void-returning function; this helper returns size_t.
    EXPECT_NE(transforms->bind(topic, "sensor_msgs/msg/Image"), nullptr);  // what get_topics would have done
  }
  auto publisher = node->create_publisher<sensor_msgs::msg::Image>(topic, rclcpp::QoS(10));
  EXPECT_TRUE(manager->subscribe(topic, "sensor_msgs/msg/Image"));

  sensor_msgs::msg::Image img;
  img.width = 100;
  img.height = 100;
  img.step = 300;
  img.encoding = "rgb8";
  img.data.assign(static_cast<size_t>(img.height) * img.step, 0xAB);

  publish_until<sensor_msgs::msg::Image>(node, publisher, img, [&] { return received_size.load() != 0; });

  manager->unsubscribe_all();
  return received_size.load();
}

constexpr size_t kImagePayloadBytes = 100 * 300;

}  // namespace

class Ros2SubscriptionManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rclcpp::init(0, nullptr);
  }

  void TearDown() override {
    rclcpp::shutdown();
  }
};

// ---------------------------------------------------------------------------
// Stripping is opt-in: by default, no transform is configured and large data
// fields are forwarded intact.
// ---------------------------------------------------------------------------
TEST_F(Ros2SubscriptionManagerTest, DataFieldsIncludedByDefault) {
  size_t received = roundtrip_image_bytes(/*transforms=*/nullptr, /*use_default_config=*/true);
  ASSERT_GT(received, 0u) << "no message received";
  EXPECT_GE(received, kImagePayloadBytes) << "image data was stripped despite default (opt-in) configuration";
}

// ---------------------------------------------------------------------------
// Opting in still strips: with a `strip` transform bound to the topic, the
// payload is removed and only the metadata remains.
// ---------------------------------------------------------------------------
TEST_F(Ros2SubscriptionManagerTest, OptInStrippingRemovesData) {
  size_t received = roundtrip_image_bytes(strip_everything(), /*use_default_config=*/false);
  ASSERT_GT(received, 0u) << "no message received";
  EXPECT_LT(received, kImagePayloadBytes) << "opt-in stripping did not remove the data payload";
}

// ---------------------------------------------------------------------------
// The hook subscribes with the topic's SOURCE type (not the advertised
// output type) and forwards whatever bytes the bound transform produces.
// ---------------------------------------------------------------------------
TEST_F(Ros2SubscriptionManagerTest, SubscribesWithSourceTypeAndForwardsTransformedBytes) {
  // The output type differs from the source type: the manager is asked to
  // subscribe with the ADVERTISED type and must use the source type.
  auto set = TransformSet::create(
                 test_helpers::rule(
                     {{"match_type", "std_msgs/msg/String"}, {"match_topic", "/transform_me"}, {"transform", "fake"}}),
                 {{"fake", test_helpers::fake_factory("std_msgs/msg/String", "fake_msgs/msg/Out")}})
                 .value();
  ASSERT_NE(set->bind("/transform_me", "std_msgs/msg/String"), nullptr);  // what get_topics would have done

  auto node = std::make_shared<rclcpp::Node>("test_transform_hook");
  Ros2SubscriptionManager manager(node, set);
  std::vector<std::byte> received;
  manager.set_message_callback(
      [&](const std::string&, std::shared_ptr<std::vector<std::byte>> data, uint64_t) { received = *data; });

  // subscribe() is called with the ADVERTISED (output) type, as BridgeServer does.
  ASSERT_TRUE(manager.subscribe("/transform_me", "fake_msgs/msg/Out"));

  auto publisher = node->create_publisher<std_msgs::msg::String>("/transform_me", rclcpp::QoS(10));
  std_msgs::msg::String msg;
  msg.data = "hello";
  publish_until<std_msgs::msg::String>(node, publisher, msg, [&] { return !received.empty(); });

  ASSERT_FALSE(received.empty());
  EXPECT_EQ(received.back(), std::byte{0xAB});  // FakeTransform's marker: the transform ran
}

// The client was told the output type at subscribe time, so a sample whose
// transform fails must be dropped, never forwarded untransformed.
TEST_F(Ros2SubscriptionManagerTest, FailedTransformDropsTheSample) {
  auto set = TransformSet::create(
                 test_helpers::rule(
                     {{"match_type", "std_msgs/msg/String"},
                      {"match_topic", "/always_fails"},
                      {"transform", "fake"},
                      {"params", {{"fail", true}}}}),
                 {{"fake", test_helpers::fake_factory("std_msgs/msg/String", "std_msgs/msg/String")}})
                 .value();
  auto bound = set->bind("/always_fails", "std_msgs/msg/String");
  ASSERT_NE(bound, nullptr);

  auto node = std::make_shared<rclcpp::Node>("test_transform_hook_failure");
  Ros2SubscriptionManager manager(node, set);
  std::atomic<int> forwarded{0};
  manager.set_message_callback(
      [&](const std::string&, std::shared_ptr<std::vector<std::byte>>, uint64_t) { forwarded++; });
  ASSERT_TRUE(manager.subscribe("/always_fails", "std_msgs/msg/String"));

  auto publisher = node->create_publisher<std_msgs::msg::String>("/always_fails", rclcpp::QoS(10));
  std_msgs::msg::String msg;
  msg.data = "hello";
  publish_until<std_msgs::msg::String>(node, publisher, msg, [&] { return bound->drops.load() >= 3; });

  EXPECT_GE(bound->drops.load(), 3u);
  EXPECT_EQ(forwarded.load(), 0);
}
