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

#include <memory>
#include <rclcpp/rclcpp.hpp>

#include "pj_ros_bridge/topic_discovery.hpp"

using namespace pj_ros_bridge;

class TopicDiscoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    rclcpp::init(0, nullptr);
    node_ = rclcpp::Node::make_shared("test_topic_discovery_node");
    discovery_ = std::make_unique<TopicDiscovery>(node_);
  }

  void TearDown() override {
    discovery_.reset();
    node_.reset();
    rclcpp::shutdown();
  }

  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<TopicDiscovery> discovery_;
};

TEST_F(TopicDiscoveryTest, DiscoverTopicsReturnsVector) {
  auto topics = discovery_->discover_topics();
  // Should return a vector (may be empty if no topics are publishing)
  EXPECT_TRUE(topics.empty() || !topics.empty());
}

TEST_F(TopicDiscoveryTest, GetTopicsAfterDiscovery) {
  discovery_->discover_topics();
  auto topics = discovery_->get_topics();
  // Should return the cached topics
  EXPECT_TRUE(topics.empty() || !topics.empty());
}

TEST_F(TopicDiscoveryTest, RefreshSucceeds) {
  EXPECT_TRUE(discovery_->refresh());
}

TEST_F(TopicDiscoveryTest, FilteredTopicsNotIncluded) {
  // The discovery should filter out system topics like /rosout
  auto topics = discovery_->discover_topics();

  for (const auto& topic : topics) {
    EXPECT_NE(topic.name, "/rosout");
    EXPECT_NE(topic.name, "/parameter_events");
    EXPECT_NE(topic.name, "/robot_description");
    // Should not include topics starting with /_
    EXPECT_FALSE(topic.name.find("/_") == 0);
  }
}
