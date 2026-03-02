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

#include <memory>
#include <rclcpp/rclcpp.hpp>

#include "pj_bridge_ros2/topic_discovery.hpp"

using namespace pj_bridge;

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
  // In a test environment with no publishers, system topics are filtered,
  // so the result should be empty (only /rosout and /parameter_events exist)
  for (const auto& topic : topics) {
    EXPECT_FALSE(topic.name.empty());
    EXPECT_FALSE(topic.type.empty());
  }
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
