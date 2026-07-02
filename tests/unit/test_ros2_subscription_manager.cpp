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

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "pj_bridge_ros2/ros2_subscription_manager.hpp"

using namespace pj_bridge;

namespace {

// Publish an Image with a large data payload through the manager and return
// the size of the serialized message delivered to the bridge callback.
size_t roundtrip_image_bytes(bool strip_large_messages, bool use_default_config) {
  auto node = std::make_shared<rclcpp::Node>(
      use_default_config ? "test_strip_default" : (strip_large_messages ? "test_strip_on" : "test_strip_off"));
  auto manager = use_default_config ? std::make_shared<Ros2SubscriptionManager>(node)
                                    : std::make_shared<Ros2SubscriptionManager>(node, strip_large_messages);

  std::atomic<size_t> received_size{0};
  manager->set_message_callback(
      [&received_size](const std::string&, std::shared_ptr<std::vector<std::byte>> data, uint64_t) {
        received_size = data->size();
      });

  const std::string topic = "/strip_test_image_" + std::string(node->get_name());
  auto publisher = node->create_publisher<sensor_msgs::msg::Image>(topic, rclcpp::QoS(10));
  EXPECT_TRUE(manager->subscribe(topic, "sensor_msgs/msg/Image"));

  sensor_msgs::msg::Image img;
  img.width = 100;
  img.height = 100;
  img.step = 300;
  img.encoding = "rgb8";
  img.data.assign(static_cast<size_t>(img.height) * img.step, 0xAB);

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received_size.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    publisher->publish(img);
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

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
// Stripping is opt-in: by default, large data fields are forwarded intact.
// ---------------------------------------------------------------------------
TEST_F(Ros2SubscriptionManagerTest, DataFieldsIncludedByDefault) {
  size_t received = roundtrip_image_bytes(false, /*use_default_config=*/true);
  ASSERT_GT(received, 0u) << "no message received";
  EXPECT_GE(received, kImagePayloadBytes) << "image data was stripped despite default (opt-in) configuration";
}

// ---------------------------------------------------------------------------
// Opting in still strips: with strip_large_messages=true the payload is
// removed and only the metadata remains.
// ---------------------------------------------------------------------------
TEST_F(Ros2SubscriptionManagerTest, OptInStrippingRemovesData) {
  size_t received = roundtrip_image_bytes(/*strip_large_messages=*/true, /*use_default_config=*/false);
  ASSERT_GT(received, 0u) << "no message received";
  EXPECT_LT(received, kImagePayloadBytes) << "opt-in stripping did not remove the data payload";
}
