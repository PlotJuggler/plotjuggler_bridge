// Copyright 2025 Davide Faconti
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>
#include "pj_ros_bridge/generic_subscription_manager.hpp"
#include <rclcpp/rclcpp.hpp>

using namespace pj_ros_bridge;

class GenericSubscriptionManagerTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
    node_ = std::make_shared<rclcpp::Node>("test_subscription_manager");
    manager_ = std::make_unique<GenericSubscriptionManager>(node_);
  }

  void TearDown() override
  {
    manager_.reset();
    node_.reset();
    rclcpp::shutdown();
  }

  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<GenericSubscriptionManager> manager_;
};

TEST_F(GenericSubscriptionManagerTest, Subscribe)
{
  bool callback_called = false;
  auto callback = [&callback_called](
    const std::string&,
    const std::shared_ptr<rclcpp::SerializedMessage>&,
    uint64_t) {
    callback_called = true;
  };

  bool result = manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);

  EXPECT_TRUE(result);
  EXPECT_TRUE(manager_->is_subscribed("/test_topic"));
  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 1);
}

TEST_F(GenericSubscriptionManagerTest, SubscribeTwiceIncreasesReferenceCount)
{
  auto callback1 = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};
  auto callback2 = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback1);
  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback2);

  EXPECT_EQ(manager_->get_reference_count("/test_topic"), 2);
}

TEST_F(GenericSubscriptionManagerTest, Unsubscribe)
{
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);
  EXPECT_TRUE(manager_->is_subscribed("/test_topic"));

  bool result = manager_->unsubscribe("/test_topic");

  EXPECT_TRUE(result);
  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));
}

TEST_F(GenericSubscriptionManagerTest, UnsubscribeDecrementsReferenceCount)
{
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

TEST_F(GenericSubscriptionManagerTest, UnsubscribeNonExistentTopic)
{
  bool result = manager_->unsubscribe("/non_existent_topic");

  EXPECT_FALSE(result);
}

TEST_F(GenericSubscriptionManagerTest, IsSubscribed)
{
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));

  manager_->subscribe("/test_topic", "std_msgs/msg/String", callback);

  EXPECT_TRUE(manager_->is_subscribed("/test_topic"));
}

TEST_F(GenericSubscriptionManagerTest, GetReferenceCountForNonExistentTopic)
{
  EXPECT_EQ(manager_->get_reference_count("/non_existent_topic"), 0);
}

TEST_F(GenericSubscriptionManagerTest, UnsubscribeAll)
{
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

TEST_F(GenericSubscriptionManagerTest, SubscribeWithInvalidType)
{
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  // Try to subscribe with an invalid message type
  bool result = manager_->subscribe("/test_topic", "invalid/msg/Type", callback);

  EXPECT_FALSE(result);
  EXPECT_FALSE(manager_->is_subscribed("/test_topic"));
}

TEST_F(GenericSubscriptionManagerTest, MultipleTopicsIndependent)
{
  auto callback = [](const std::string&, const std::shared_ptr<rclcpp::SerializedMessage>&, uint64_t) {};

  manager_->subscribe("/topic1", "std_msgs/msg/String", callback);
  manager_->subscribe("/topic2", "std_msgs/msg/String", callback);

  EXPECT_EQ(manager_->get_reference_count("/topic1"), 1);
  EXPECT_EQ(manager_->get_reference_count("/topic2"), 1);

  manager_->unsubscribe("/topic1");

  EXPECT_FALSE(manager_->is_subscribed("/topic1"));
  EXPECT_TRUE(manager_->is_subscribed("/topic2"));
}
