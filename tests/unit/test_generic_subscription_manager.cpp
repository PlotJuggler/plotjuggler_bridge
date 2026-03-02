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

#include "pj_bridge_ros2/generic_subscription_manager.hpp"

using namespace pj_bridge;

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
