/*
 * Copyright (C) 2026 Davide Faconti
 *
 * This file is part of pj_ros_bridge.
 *
 * pj_ros_bridge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pj_ros_bridge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with pj_ros_bridge. If not, see <https://www.gnu.org/licenses/>.
 */

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

TEST_F(SchemaExtractorTest, PointCloud2SchemaContainsExpectedFields) {
  // Extract actual schema
  std::string actual = extractor_->get_message_definition("sensor_msgs/msg/PointCloud2");
  ASSERT_FALSE(actual.empty()) << "Failed to extract schema for sensor_msgs/msg/PointCloud2";

  // Remove comments from schema for checking
  std::string actual_no_comments = remove_comments_from_schema(actual);

  // Check that all expected fields are present (newer ROS versions may add more)
  EXPECT_NE(actual_no_comments.find("std_msgs/Header header"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("uint32 height"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("uint32 width"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("PointField[] fields"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("bool    is_bigendian"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("uint32  point_step"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("uint32  row_step"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("uint8[] data"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("bool is_dense"), std::string::npos);

  // Check nested PointField has core constants (INT8 through FLOAT64)
  EXPECT_NE(actual_no_comments.find("uint8 INT8    = 1"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("uint8 FLOAT64 = 8"), std::string::npos);

  // Check nested Header and Time are included
  EXPECT_NE(actual_no_comments.find("MSG: std_msgs/Header"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("MSG: builtin_interfaces/Time"), std::string::npos);
  EXPECT_NE(actual_no_comments.find("string frame_id"), std::string::npos);
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

TEST_F(SchemaExtractorTest, CachingReturnsSameResult) {
  const std::string message_type = "sensor_msgs/msg/PointCloud2";

  // First call - cache miss
  std::string first_result = extractor_->get_message_definition(message_type);
  ASSERT_FALSE(first_result.empty()) << "First call failed to extract schema";

  // Second call - should use cache
  std::string second_result = extractor_->get_message_definition(message_type);
  ASSERT_FALSE(second_result.empty()) << "Second call failed to extract schema";

  // Results should be identical
  EXPECT_EQ(first_result, second_result) << "Cached result differs from original";
}

TEST_F(SchemaExtractorTest, CachingWorksForMultipleTypes) {
  const std::string type1 = "sensor_msgs/msg/PointCloud2";
  const std::string type2 = "sensor_msgs/msg/Imu";

  // Get both schemas twice
  std::string result1a = extractor_->get_message_definition(type1);
  std::string result2a = extractor_->get_message_definition(type2);
  std::string result1b = extractor_->get_message_definition(type1);
  std::string result2b = extractor_->get_message_definition(type2);

  ASSERT_FALSE(result1a.empty());
  ASSERT_FALSE(result2a.empty());
  ASSERT_FALSE(result1b.empty());
  ASSERT_FALSE(result2b.empty());

  // Cached results should match originals
  EXPECT_EQ(result1a, result1b) << "Cached PointCloud2 differs from original";
  EXPECT_EQ(result2a, result2b) << "Cached Imu differs from original";
}

TEST_F(SchemaExtractorTest, InvalidMessageTypeFormat) {
  // The parser requires "package/msg/Type" format; a string without slashes should
  // return an empty definition.
  std::string result = extractor_->get_message_definition("no_slashes");
  EXPECT_TRUE(result.empty()) << "Expected empty string for invalid message type format";
}

TEST_F(SchemaExtractorTest, NonExistentPackage) {
  // A completely fabricated package should return an empty definition.
  std::string result = extractor_->get_message_definition("fake_nonexistent_package/msg/FakeType");
  EXPECT_TRUE(result.empty()) << "Expected empty string for non-existent package";
}

TEST_F(SchemaExtractorTest, RemoveCommentsEmpty) {
  // Removing comments from an empty string should return an empty string.
  std::string result = remove_comments_from_schema("");
  EXPECT_TRUE(result.empty()) << "Expected empty string when removing comments from empty input";
}

TEST_F(SchemaExtractorTest, RemoveCommentsOnlyComments) {
  // A schema that contains only comment lines should produce no '#' characters
  // and be effectively empty or whitespace-only after comment removal.
  std::string input = "# comment line\n# another comment\n";
  std::string result = remove_comments_from_schema(input);

  // Verify no '#' characters remain
  EXPECT_EQ(result.find('#'), std::string::npos) << "Result should contain no '#' characters";

  // Verify the result is empty or whitespace-only
  bool is_empty_or_whitespace = true;
  for (char c : result) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      is_empty_or_whitespace = false;
      break;
    }
  }
  EXPECT_TRUE(is_empty_or_whitespace) << "Result should be empty or whitespace-only, got: '" << result << "'";
}
