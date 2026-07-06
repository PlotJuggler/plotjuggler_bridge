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
#include <cstdlib>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <thread>

#include "pj_bridge_ros2/generic_subscription_manager.hpp"

using namespace pj_bridge;

namespace {
// History depth is a purely local resource policy, not one of the
// requested/offered QoS policies DDS discovery is required to propagate
// (unlike reliability/durability, which affect wire compatibility). This
// workspace's default RMW (rmw_fastrtps_cpp) does not report it — publishers
// discovered via get_publishers_info_by_topic() always come back with depth
// 0 / history UNKNOWN — whereas CycloneDDS does propagate it. The QoS depth
// aggregation tests below need a real (non-mocked) discovered depth to
// exercise adapt_qos(), so force CycloneDDS for this test binary.
//
// The RMW implementation is selected once per process and cached on first
// use, so this must happen before any rclcpp::init() call anywhere in the
// binary. Static objects across all translation units are guaranteed to be
// constructed before main() runs (and therefore before any TEST_F body),
// so a file-scope static works regardless of test execution order. An
// operator's own explicit RMW_IMPLEMENTATION is still respected.
struct ForceCycloneDdsForQosDepthTests {
  ForceCycloneDdsForQosDepthTests() {
    if (std::getenv("RMW_IMPLEMENTATION") == nullptr) {
      setenv("RMW_IMPLEMENTATION", "rmw_cyclonedds_cpp", 1);
    }
  }
};
const ForceCycloneDdsForQosDepthTests force_cyclonedds_for_qos_depth_tests;
}  // namespace

class GenericSubscriptionManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rclcpp::init(0, nullptr);
    node_ = std::make_shared<rclcpp::Node>("test_subscription_manager");
    manager_ = std::make_unique<GenericSubscriptionManager>(node_);
  }

  void TearDown() override {
    manager_.reset();
    node_.reset();
    rclcpp::shutdown();
  }

  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<GenericSubscriptionManager> manager_;
};

namespace {
// Publisher discovery is asynchronous even within a single process, so tests
// must poll the graph rather than assume a freshly created publisher is
// immediately visible via get_publishers_info_by_topic().
void wait_for_publisher_count(
    const rclcpp::Node::SharedPtr& node, const std::string& topic_name, size_t expected_count) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (node->get_publishers_info_by_topic(topic_name).size() < expected_count &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
}  // namespace

TEST_F(GenericSubscriptionManagerTest, Subscribe) {
  bool callback_called = false;
  auto callback = [&callback_called](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {
    callback_called = true;
  };

  bool result = manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);

  EXPECT_TRUE(result);
  EXPECT_TRUE(manager_->is_subscribed("/test_topic"));
  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 1);
}

TEST_F(GenericSubscriptionManagerTest, SubscribeTwiceIncreasesReferenceCount) {
  auto callback1 = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};
  auto callback2 = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback1);
  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback2);

  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 2);
}

TEST_F(GenericSubscriptionManagerTest, Unsubscribe) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);
  EXPECT_TRUE(manager_->is_subscribed("/test_topic"));

  bool result = manager_->unsubscribe("/test_topic");

  EXPECT_TRUE(result);
  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));
}

TEST_F(GenericSubscriptionManagerTest, UnsubscribeDecrementsReferenceCount) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);
  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);

  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 2);

  manager_->unsubscribe("/test_topic");

  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 1);
  EXPECT_TRUE(manager_->is_subscribed("/test_topic"));

  manager_->unsubscribe("/test_topic");

  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 0);
  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));
}

TEST_F(GenericSubscriptionManagerTest, UnsubscribeNonExistentTopic) {
  bool result = manager_->unsubscribe("/non_existent_topic");

  EXPECT_FALSE(result);
}

TEST_F(GenericSubscriptionManagerTest, IsSubscribed) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));

  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);

  EXPECT_TRUE(manager_->is_subscribed("/test_topic"));
}

TEST_F(GenericSubscriptionManagerTest, GetReferenceCountForNonExistentTopic) {
  EXPECT_EQ(manager_->get_reference_count("/non_existent_topic"), 0);
}

TEST_F(GenericSubscriptionManagerTest, UnsubscribeAll) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/topic1", "std_msgs/msg/String", callback);
  manager_->subscribe("/topic2", "std_msgs/msg/Int32", callback);
  manager_->subscribe("/topic3", "std_msgs/msg/Float64", callback);

  EXPECT_TRUE(manager_->is_subscribed("/topic1"));
  EXPECT_TRUE(manager_->is_subscribed("/topic2"));
  EXPECT_TRUE(manager_->is_subscribed("/topic3"));

  manager_->unsubscribe_all();

  EXPECT_FALSE(manager_->is_subscribed("/topic1"));
  EXPECT_FALSE(manager_->is_subscribed("/topic2"));
  EXPECT_FALSE(manager_->is_subscribed("/topic3"));
}

TEST_F(GenericSubscriptionManagerTest, SubscribeWithInvalidType) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  // Try to subscribe with an invalid message type
  bool result = manager_->subscribe("/test_topic", "invalid/msg/Type", callback);

  EXPECT_FALSE(result);
  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));
}

TEST_F(GenericSubscriptionManagerTest, DoubleUnsubscribeDoesNotUnderflow) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  // Subscribe once
  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);
  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 1);

  // First unsubscribe succeeds and removes the subscription
  EXPECT_TRUE(manager_->unsubscribe("/test_topic"));
  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 0);
  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));

  // Second unsubscribe returns false (topic gone), no crash or underflow
  EXPECT_FALSE(manager_->unsubscribe("/test_topic"));
  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 0);
}

// Regression test for ref count corruption bug:
// The old code in handle_subscribe() called subscribe() BEFORE extracting the
// schema. If schema extraction failed, it would call unsubscribe() to undo the
// subscription. But if another client had already subscribed to the same topic,
// unsubscribe() would decrement the shared reference count, potentially causing
// message loss for the existing client.
//
// The fix moved schema extraction BEFORE subscribe(). This test verifies that
// the underlying mechanism is safe: subscribing twice (2 clients) then
// unsubscribing once (simulating the old error path) leaves the first client's
// subscription intact.
TEST_F(GenericSubscriptionManagerTest, SpuriousUnsubscribeDoesNotCorruptSharedRef) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  // Client A subscribes
  ASSERT_TRUE(manager_->subscribe("/shared_topic", "std_msgs/msg/String", callback));
  EXPECT_EQ(manager_->get_reference_count("/shared_topic"), 1);

  // Client B subscribes to the same topic (ref_count = 2)
  ASSERT_TRUE(manager_->subscribe("/shared_topic", "std_msgs/msg/String", callback));
  EXPECT_EQ(manager_->get_reference_count("/shared_topic"), 2);

  // Simulate the old bug: client B's schema extraction fails, triggering unsubscribe
  EXPECT_TRUE(manager_->unsubscribe("/shared_topic"));

  // Client A's subscription must still be alive
  EXPECT_TRUE(manager_->is_subscribed("/shared_topic"));
  EXPECT_EQ(manager_->get_reference_count("/shared_topic"), 1);

  // Client A cleanly unsubscribes — subscription fully removed
  EXPECT_TRUE(manager_->unsubscribe("/shared_topic"));
  EXPECT_FALSE(manager_->is_subscribed("/shared_topic"));
  EXPECT_EQ(manager_->get_reference_count("/shared_topic"), 0);
}

TEST_F(GenericSubscriptionManagerTest, MultipleTopicsIndependent) {
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/topic1", "std_msgs/msg/String", callback);
  manager_->subscribe("/topic2", "std_msgs/msg/String", callback);

  EXPECT_EQ(manager_->get_reference_count("/topic1"), 1);
  EXPECT_EQ(manager_->get_reference_count("/topic2"), 1);

  manager_->unsubscribe("/topic1");

  EXPECT_FALSE(manager_->is_subscribed("/topic1"));
  EXPECT_TRUE(manager_->is_subscribed("/topic2"));
}

// ---------------------------------------------------------------------------
// QoS adaptation
//
// Sensor publishers (cameras, lidars, IMUs) typically offer BEST_EFFORT
// reliability. A RELIABLE subscription is QoS-incompatible with them in
// ROS2 — it matches nothing and silently receives zero messages — so the
// manager must adapt its subscription QoS to what publishers actually offer.
// ---------------------------------------------------------------------------
TEST_F(GenericSubscriptionManagerTest, AdaptsQosToBestEffortPublisher) {
  auto publisher = node_->create_publisher<sensor_msgs::msg::Imu>("/qos_be_topic", rclcpp::SensorDataQoS());
  // Wait for discovery before subscribing so adapt_qos() sees the publisher
  // (discovery is asynchronous and its latency varies by RMW implementation).
  wait_for_publisher_count(node_, "/qos_be_topic", 1);

  std::atomic<int> received{0};
  auto callback = [&received](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {
    received++;
  };

  ASSERT_TRUE(manager_->subscribe("/qos_be_topic", "sensor_msgs/msg/Imu", callback));

  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node_);

  sensor_msgs::msg::Imu msg;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    publisher->publish(msg);
    executor.spin_some();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_GT(received.load(), 0) << "RELIABLE subscription never matches a BEST_EFFORT publisher";
}

// ---------------------------------------------------------------------------
// QoS depth aggregation
//
// Depth aggregation adapted from foxglove_bridge (MIT License, Copyright (c)
// Foxglove Technologies Inc): sum the publishers' history depths so a burst
// from every publisher still fits, then clamp to [min_qos_depth, max_qos_depth].
// (See the ForceCycloneDdsForQosDepthTests note above for why this test
// binary forces CycloneDDS.)
// ---------------------------------------------------------------------------

TEST_F(GenericSubscriptionManagerTest, AdaptQosDepthMatchesSinglePublisher) {
  auto publisher = node_->create_publisher<sensor_msgs::msg::Imu>("/qos_depth_topic1", rclcpp::QoS(10));
  wait_for_publisher_count(node_, "/qos_depth_topic1", 1);

  rclcpp::QoS qos = manager_->adapt_qos("/qos_depth_topic1");

  EXPECT_EQ(qos.depth(), 10u);
}

TEST_F(GenericSubscriptionManagerTest, AdaptQosDepthSumsAcrossPublishersClampedToMax) {
  auto publisher1 = node_->create_publisher<sensor_msgs::msg::Imu>("/qos_depth_topic2", rclcpp::QoS(60));
  auto publisher2 = node_->create_publisher<sensor_msgs::msg::Imu>("/qos_depth_topic2", rclcpp::QoS(60));
  wait_for_publisher_count(node_, "/qos_depth_topic2", 2);

  rclcpp::QoS qos = manager_->adapt_qos("/qos_depth_topic2");

  // 60 + 60 = 120, clamped to the default max_qos_depth of 100.
  EXPECT_EQ(qos.depth(), 100u);
}

TEST_F(GenericSubscriptionManagerTest, AdaptQosDepthClampedToConfiguredMinimum) {
  auto manager_with_min = std::make_unique<GenericSubscriptionManager>(node_, /*min_qos_depth=*/30);
  auto publisher = node_->create_publisher<sensor_msgs::msg::Imu>("/qos_depth_topic3", rclcpp::QoS(5));
  wait_for_publisher_count(node_, "/qos_depth_topic3", 1);

  rclcpp::QoS qos = manager_with_min->adapt_qos("/qos_depth_topic3");

  EXPECT_EQ(qos.depth(), 30u);
}

TEST_F(GenericSubscriptionManagerTest, AdaptQosDepthDefaultsTo100WhenNoPublishers) {
  rclcpp::QoS qos = manager_->adapt_qos("/qos_depth_no_publishers");

  EXPECT_EQ(qos.depth(), 100u);
}
