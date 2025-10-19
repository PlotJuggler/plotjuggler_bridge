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
#include "pj_ros_bridge/schema_extractor.hpp"

#include <nlohmann/json.hpp>

using namespace pj_ros_bridge;

class SchemaExtractorTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    extractor_ = std::make_unique<SchemaExtractor>();
  }

  void TearDown() override
  {
    extractor_.reset();
  }

  std::unique_ptr<SchemaExtractor> extractor_;
};

TEST_F(SchemaExtractorTest, ExtractSchemaForStdMsgsString)
{
  // std_msgs/String should be available in any ROS2 installation
  auto schema = extractor_->extract_schema("std_msgs/msg/String");

  // Schema should not be null
  EXPECT_FALSE(schema.is_null());

  if (!schema.is_null()) {
    // Should have properties
    EXPECT_TRUE(schema.contains("properties"));

    // std_msgs/String has a "data" field
    if (schema.contains("properties")) {
      EXPECT_TRUE(schema["properties"].contains("data"));
    }
  }
}

TEST_F(SchemaExtractorTest, GetSchemaJsonReturnsString)
{
  auto json_str = extractor_->get_schema_json("std_msgs/msg/String");

  // Should return a non-empty string for valid message type
  EXPECT_FALSE(json_str.empty());

  // Should be valid JSON
  if (!json_str.empty()) {
    EXPECT_NO_THROW({
      auto parsed = nlohmann::json::parse(json_str);
    });
  }
}

TEST_F(SchemaExtractorTest, InvalidMessageTypeReturnsNull)
{
  auto schema = extractor_->extract_schema("invalid/msg/Type");

  // Should return null for invalid type
  EXPECT_TRUE(schema.is_null());
}

TEST_F(SchemaExtractorTest, InvalidMessageTypeReturnsEmptyString)
{
  auto json_str = extractor_->get_schema_json("invalid/msg/Type");

  // Should return empty string for invalid type
  EXPECT_TRUE(json_str.empty());
}

TEST_F(SchemaExtractorTest, MalformedMessageTypeReturnsNull)
{
  // Missing /msg/ part
  auto schema = extractor_->extract_schema("std_msgs/String");

  EXPECT_TRUE(schema.is_null());
}
