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

#include <cstring>

#include "pj_ros_bridge/message_serializer.hpp"

using namespace pj_ros_bridge;

class MessageSerializerTest : public ::testing::Test {
 protected:
  AggregatedMessageSerializer serializer_;

  // Helper to read little-endian values from buffer
  template <typename T>
  T read_le(const std::vector<uint8_t>& buffer, size_t& offset) {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
      value |= static_cast<T>(buffer[offset++]) << (i * 8);
    }
    return value;
  }
};

TEST_F(MessageSerializerTest, AddMessage) {
  std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  serializer_.add_message("/test_topic", 1000000000, 1000000100, data);

  EXPECT_EQ(serializer_.message_count(), 1);

  serializer_.add_message("/topic2", 2000000000, 2000000200, data);
  EXPECT_EQ(serializer_.message_count(), 2);
}

TEST_F(MessageSerializerTest, Clear) {
  std::vector<uint8_t> data = {1, 2, 3};
  serializer_.add_message("/topic", 100, 200, data);
  serializer_.add_message("/topic2", 300, 400, data);

  EXPECT_EQ(serializer_.message_count(), 2);

  serializer_.clear();
  EXPECT_EQ(serializer_.message_count(), 0);
}

TEST_F(MessageSerializerTest, SerializeEmpty) {
  auto serialized = serializer_.serialize();

  ASSERT_EQ(serialized.size(), 4);  // Just the message count (uint32_t)

  size_t offset = 0;
  uint32_t msg_count = read_le<uint32_t>(serialized, offset);
  EXPECT_EQ(msg_count, 0);
}

TEST_F(MessageSerializerTest, SerializeSingleMessage) {
  std::string topic = "/test_topic";
  uint64_t pub_time = 1234567890123456789ULL;
  uint64_t recv_time = 1234567890223456789ULL;
  std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};

  serializer_.add_message(topic, pub_time, recv_time, data);

  auto serialized = serializer_.serialize();

  // Parse serialized data
  size_t offset = 0;

  // Message count
  uint32_t msg_count = read_le<uint32_t>(serialized, offset);
  EXPECT_EQ(msg_count, 1);

  // Topic name length
  uint16_t topic_len = read_le<uint16_t>(serialized, offset);
  EXPECT_EQ(topic_len, topic.size());

  // Topic name
  std::string parsed_topic(serialized.begin() + offset, serialized.begin() + offset + topic_len);
  offset += topic_len;
  EXPECT_EQ(parsed_topic, topic);

  // Publish timestamp
  uint64_t parsed_pub_time = read_le<uint64_t>(serialized, offset);
  EXPECT_EQ(parsed_pub_time, pub_time);

  // Receive timestamp
  uint64_t parsed_recv_time = read_le<uint64_t>(serialized, offset);
  EXPECT_EQ(parsed_recv_time, recv_time);

  // Data length
  uint32_t data_len = read_le<uint32_t>(serialized, offset);
  EXPECT_EQ(data_len, data.size());

  // Data
  std::vector<uint8_t> parsed_data(serialized.begin() + offset, serialized.begin() + offset + data_len);
  EXPECT_EQ(parsed_data, data);
}

TEST_F(MessageSerializerTest, SerializeMultipleMessages) {
  // Add 3 messages with different data
  serializer_.add_message("/topic1", 100, 101, {0x01, 0x02});
  serializer_.add_message("/topic2", 200, 202, {0x03, 0x04, 0x05});
  serializer_.add_message("/topic3", 300, 303, {0x06});

  auto serialized = serializer_.serialize();

  size_t offset = 0;
  uint32_t msg_count = read_le<uint32_t>(serialized, offset);
  EXPECT_EQ(msg_count, 3);

  // Verify we can parse all three messages
  for (int i = 0; i < 3; ++i) {
    uint16_t topic_len = read_le<uint16_t>(serialized, offset);
    EXPECT_GT(topic_len, 0);

    offset += topic_len;  // Skip topic name
    offset += 8;          // Skip publish timestamp
    offset += 8;          // Skip receive timestamp

    uint32_t data_len = read_le<uint32_t>(serialized, offset);
    EXPECT_GT(data_len, 0);

    offset += data_len;  // Skip data
  }

  // Should have consumed entire buffer
  EXPECT_EQ(offset, serialized.size());
}

TEST_F(MessageSerializerTest, CompressZstdEmpty) {
  std::vector<uint8_t> empty;
  auto compressed = AggregatedMessageSerializer::compress_zstd(empty);
  EXPECT_TRUE(compressed.empty());
}

TEST_F(MessageSerializerTest, CompressDecompressZstd) {
  // Create some test data
  std::vector<uint8_t> original;
  for (int i = 0; i < 1000; ++i) {
    original.push_back(static_cast<uint8_t>(i % 256));
  }

  // Compress
  auto compressed = AggregatedMessageSerializer::compress_zstd(original);

  // Compressed size should be smaller than original
  EXPECT_LT(compressed.size(), original.size());
  EXPECT_GT(compressed.size(), 0);

  // Decompress
  auto decompressed = AggregatedMessageSerializer::decompress_zstd(compressed);

  // Should match original
  EXPECT_EQ(decompressed, original);
}

TEST_F(MessageSerializerTest, CompressZstdHighCompression) {
  // Create highly compressible data (lots of repeats)
  std::vector<uint8_t> data(10000, 0x42);

  auto compressed = AggregatedMessageSerializer::compress_zstd(data);

  // Should compress very well
  EXPECT_LT(compressed.size(), data.size() / 10);

  // Verify decompression
  auto decompressed = AggregatedMessageSerializer::decompress_zstd(compressed);
  EXPECT_EQ(decompressed, data);
}

TEST_F(MessageSerializerTest, CompressZstdDifferentLevels) {
  std::vector<uint8_t> data(1000, 0x55);

  // Try different compression levels
  auto compressed_level_1 = AggregatedMessageSerializer::compress_zstd(data, 1);
  auto compressed_level_9 = AggregatedMessageSerializer::compress_zstd(data, 9);

  // Higher compression level should give smaller or equal size
  EXPECT_LE(compressed_level_9.size(), compressed_level_1.size());

  // Both should decompress correctly
  auto decompressed_1 = AggregatedMessageSerializer::decompress_zstd(compressed_level_1);
  auto decompressed_9 = AggregatedMessageSerializer::decompress_zstd(compressed_level_9);

  EXPECT_EQ(decompressed_1, data);
  EXPECT_EQ(decompressed_9, data);
}

TEST_F(MessageSerializerTest, DecompressInvalidData) {
  std::vector<uint8_t> invalid_data = {0x01, 0x02, 0x03, 0x04};

  EXPECT_THROW(AggregatedMessageSerializer::decompress_zstd(invalid_data), std::runtime_error);
}

TEST_F(MessageSerializerTest, FullRoundTripWithCompression) {
  // Add several messages
  serializer_.add_message("/topic1", 1000, 1001, {0x01, 0x02, 0x03});
  serializer_.add_message("/topic2", 2000, 2002, {0x04, 0x05});
  serializer_.add_message("/topic3", 3000, 3003, {0x06, 0x07, 0x08, 0x09});

  // Serialize
  auto serialized = serializer_.serialize();
  EXPECT_GT(serialized.size(), 0);

  // Compress
  auto compressed = AggregatedMessageSerializer::compress_zstd(serialized);
  EXPECT_GT(compressed.size(), 0);

  // Decompress
  auto decompressed = AggregatedMessageSerializer::decompress_zstd(compressed);

  // Should match original serialized data
  EXPECT_EQ(decompressed, serialized);

  // Parse decompressed data
  size_t offset = 0;
  uint32_t msg_count = read_le<uint32_t>(decompressed, offset);
  EXPECT_EQ(msg_count, 3);
}
