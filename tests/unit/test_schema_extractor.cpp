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

#include <fstream>
#include <sstream>

#include "data_path.hpp"
#include "pj_ros_bridge/schema_extractor.hpp"

using namespace pj_ros_bridge;

// Helper function to read expected schema from file
std::string read_expected_schema(const std::string& filename) {
  std::string filepath = Test::DATA_PATH + filename;
  std::ifstream file(filepath);
  if (!file.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

class SchemaExtractorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    extractor_ = std::make_unique<SchemaExtractor>();
  }

  void TearDown() override {
    extractor_.reset();
  }

  std::unique_ptr<SchemaExtractor> extractor_;
};

TEST_F(SchemaExtractorTest, PointCloud2SchemaMatchesExpected) {
  // Read expected schema from file
  std::string expected = read_expected_schema("sensor_msgs-pointcloud2.txt");
  ASSERT_FALSE(expected.empty()) << "Failed to read expected schema file";

  // Extract actual schema
  std::string actual = extractor_->get_message_definition("sensor_msgs/msg/PointCloud2");
  ASSERT_FALSE(actual.empty()) << "Failed to extract schema for sensor_msgs/msg/PointCloud2";

  // Remove comments from both schemas for comparison
  std::string expected_no_comments = remove_comments_from_schema(expected);
  std::string actual_no_comments = remove_comments_from_schema(actual);

  // Compare schemas without comments
  EXPECT_EQ(actual_no_comments, expected_no_comments) << "Schema mismatch for sensor_msgs/msg/PointCloud2";

  if (actual_no_comments != expected_no_comments) {
    // For debugging, print both schemas without comments
    std::cout << "Expected Schema (no comments):\n" << expected_no_comments << std::endl;
    std::cout << "Actual Schema (no comments):\n" << actual_no_comments << std::endl;
  }
}

TEST_F(SchemaExtractorTest, ImuSchemaMatchesExpected) {
  // Read expected schema from file
  std::string expected = read_expected_schema("sensor_msgs-imu.txt");
  ASSERT_FALSE(expected.empty()) << "Failed to read expected schema file";

  // Extract actual schema
  std::string actual = extractor_->get_message_definition("sensor_msgs/msg/Imu");
  ASSERT_FALSE(actual.empty()) << "Failed to extract schema for sensor_msgs/msg/Imu";

  // Remove comments from both schemas for comparison
  std::string expected_no_comments = remove_comments_from_schema(expected);
  std::string actual_no_comments = remove_comments_from_schema(actual);

  // Compare schemas without comments
  EXPECT_EQ(actual_no_comments, expected_no_comments) << "Schema mismatch for sensor_msgs/msg/Imu";

  if (actual_no_comments != expected_no_comments) {
    // For debugging, print both schemas without comments
    std::cout << "Expected Schema (no comments):\n" << expected_no_comments << std::endl;
    std::cout << "Actual Schema (no comments):\n" << actual_no_comments << std::endl;
  }
}

TEST_F(SchemaExtractorTest, PoseWithCovarianceStampedSchemaMatchesExpected) {
  // Read expected schema from file
  std::string expected = read_expected_schema("pose_with_covariance_stamped.txt");
  ASSERT_FALSE(expected.empty()) << "Failed to read expected schema file";

  // Extract actual schema
  std::string actual = extractor_->get_message_definition("geometry_msgs/msg/PoseWithCovarianceStamped");
  ASSERT_FALSE(actual.empty()) << "Failed to extract schema for geometry_msgs/msg/PoseWithCovarianceStamped";

  // Remove comments from both schemas for comparison
  std::string expected_no_comments = remove_comments_from_schema(expected);
  std::string actual_no_comments = remove_comments_from_schema(actual);

  // Compare schemas without comments
  EXPECT_EQ(actual_no_comments, expected_no_comments)
      << "Schema mismatch for geometry_msgs/msg/PoseWithCovarianceStamped";

  if (actual_no_comments != expected_no_comments) {
    // For debugging, print both schemas without comments
    std::cout << "Expected Schema (no comments):\n" << expected_no_comments << std::endl;
    std::cout << "Actual Schema (no comments):\n" << actual_no_comments << std::endl;
  }
}
