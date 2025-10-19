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

#ifndef PJ_ROS_BRIDGE__TOPIC_DISCOVERY_HPP_
#define PJ_ROS_BRIDGE__TOPIC_DISCOVERY_HPP_

#include <rclcpp/rclcpp.hpp>
#include <map>
#include <string>
#include <vector>

namespace pj_ros_bridge
{

/**
 * @brief Structure to hold topic information
 */
struct TopicInfo
{
  std::string name;
  std::string type;
};

/**
 * @brief Discovers and manages ROS2 topics
 *
 * This class uses the ROS2 API to discover available topics and filter
 * system topics that should not be exposed to clients.
 *
 * Thread safety: Not thread-safe. External synchronization required.
 */
class TopicDiscovery
{
public:
  explicit TopicDiscovery(rclcpp::Node::SharedPtr node);

  /**
   * @brief Discover all available ROS2 topics
   *
   * @return Vector of discovered topics (excluding system topics)
   */
  std::vector<TopicInfo> discover_topics();

  /**
   * @brief Get cached topic information
   *
   * @return Vector of previously discovered topics
   */
  std::vector<TopicInfo> get_topics() const;

  /**
   * @brief Refresh the topic list
   *
   * @return true if successful, false otherwise
   */
  bool refresh();

private:
  rclcpp::Node::SharedPtr node_;
  std::vector<TopicInfo> topics_;

  /**
   * @brief Check if a topic should be filtered out
   *
   * @param topic_name Topic name to check
   * @return true if topic should be filtered, false otherwise
   */
  bool should_filter_topic(const std::string& topic_name) const;
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__TOPIC_DISCOVERY_HPP_
