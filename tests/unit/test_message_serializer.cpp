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

  // Helper to create a serialized message with test data
  rclcpp::SerializedMessage create_test_message(const std::vector<uint8_t>& data) {
    rclcpp::SerializedMessage msg(data.size());
    auto& rcl_msg = msg.get_rcl_serialized_message();
    std::memcpy(rcl_msg.buffer, data.data(), data.size());
    rcl_msg.buffer_length = data.size();
    return msg;
  }

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

TEST_F(MessageSerializerTest, InitialState) {
  // Serializer should start empty
  const auto& data = serializer_.get_serialized_data();
  EXPECT_EQ(data.size(), 0);
}

TEST_F(MessageSerializerTest, SerializeSingleMessage) {
  std::string topic = "/test_topic";
  uint64_t timestamp = 1234567890123456789ULL;
  std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};

  auto msg = create_test_message(data);
  serializer_.serialize_message(topic, timestamp, msg);

  const auto& serialized = serializer_.get_serialized_data();

  // Parse serialized data
  size_t offset = 0;

  // Topic name length
  uint16_t topic_len = read_le<uint16_t>(serialized, offset);
  EXPECT_EQ(topic_len, topic.size());

  // Topic name
  std::string parsed_topic(serialized.begin() + offset, serialized.begin() + offset + topic_len);
  offset += topic_len;
  EXPECT_EQ(parsed_topic, topic);

  // Publish timestamp
  uint64_t parsed_timestamp = read_le<uint64_t>(serialized, offset);
  EXPECT_EQ(parsed_timestamp, timestamp);

  // Receive timestamp (same as publish in this API)
  uint64_t parsed_recv_time = read_le<uint64_t>(serialized, offset);
  EXPECT_EQ(parsed_recv_time, timestamp);

  // Data length
  int32_t data_len = read_le<int32_t>(serialized, offset);
  EXPECT_EQ(data_len, data.size());

  // Data
  std::vector<uint8_t> parsed_data(serialized.begin() + offset, serialized.begin() + offset + data_len);
  EXPECT_EQ(parsed_data, data);
}

TEST_F(MessageSerializerTest, SerializeMultipleMessages) {
  // Add 3 messages with different data
  auto msg1 = create_test_message({0x01, 0x02});
  auto msg2 = create_test_message({0x03, 0x04, 0x05});
  auto msg3 = create_test_message({0x06});

  serializer_.serialize_message("/topic1", 100, msg1);
  serializer_.serialize_message("/topic2", 200, msg2);
  serializer_.serialize_message("/topic3", 300, msg3);

  const auto& serialized = serializer_.get_serialized_data();

  size_t offset = 0;

  // Verify we can parse all three messages
  for (int i = 0; i < 3; ++i) {
    uint16_t topic_len = read_le<uint16_t>(serialized, offset);
    EXPECT_GT(topic_len, 0);

    offset += topic_len;  // Skip topic name
    offset += 8;          // Skip publish timestamp
    offset += 8;          // Skip receive timestamp

    int32_t data_len = read_le<int32_t>(serialized, offset);
    EXPECT_GT(data_len, 0);

    offset += data_len;  // Skip data
  }

  // Should have consumed entire buffer
  EXPECT_EQ(offset, serialized.size());
}

TEST_F(MessageSerializerTest, Clear) {
  auto msg = create_test_message({1, 2, 3});
  serializer_.serialize_message("/topic", 100, msg);

  EXPECT_GT(serializer_.get_serialized_data().size(), 0);

  serializer_.clear();
  EXPECT_EQ(serializer_.get_serialized_data().size(), 0);
}

TEST_F(MessageSerializerTest, StreamingAPI) {
  // Test that messages are added immediately to buffer
  auto initial_size = serializer_.get_serialized_data().size();
  EXPECT_EQ(initial_size, 0);  // Empty

  auto msg1 = create_test_message({1, 2, 3});
  serializer_.serialize_message("/topic1", 100, msg1);
  auto size_after_1 = serializer_.get_serialized_data().size();
  EXPECT_GT(size_after_1, initial_size);

  auto msg2 = create_test_message({4, 5, 6, 7});
  serializer_.serialize_message("/topic2", 200, msg2);
  auto size_after_2 = serializer_.get_serialized_data().size();
  EXPECT_GT(size_after_2, size_after_1);
}

TEST_F(MessageSerializerTest, CompressZstdEmpty) {
  std::vector<uint8_t> empty;
  std::vector<uint8_t> compressed;
  AggregatedMessageSerializer::compress_zstd(empty, compressed);
  EXPECT_TRUE(compressed.empty());
}

TEST_F(MessageSerializerTest, CompressDecompressZstd) {
  // Create some test data
  std::vector<uint8_t> original;
  for (int i = 0; i < 1000; ++i) {
    original.push_back(static_cast<uint8_t>(i % 256));
  }

  // Compress
  std::vector<uint8_t> compressed;
  AggregatedMessageSerializer::compress_zstd(original, compressed);

  // Compressed size should be smaller than original (for repetitive data)
  EXPECT_GT(compressed.size(), 0);

  // Decompress
  std::vector<uint8_t> decompressed;
  AggregatedMessageSerializer::decompress_zstd(compressed, decompressed);

  // Should match original
  EXPECT_EQ(decompressed, original);
}

TEST_F(MessageSerializerTest, CompressZstdHighCompression) {
  // Create highly compressible data (lots of repeats)
  std::vector<uint8_t> data(10000, 0x42);

  std::vector<uint8_t> compressed;
  AggregatedMessageSerializer::compress_zstd(data, compressed);

  // Should compress very well
  EXPECT_LT(compressed.size(), data.size() / 10);

  // Verify decompression
  std::vector<uint8_t> decompressed;
  AggregatedMessageSerializer::decompress_zstd(compressed, decompressed);
  EXPECT_EQ(decompressed, data);
}

TEST_F(MessageSerializerTest, CompressZstdOutParameter) {
  std::vector<uint8_t> data(1000, 0xAB);

  // Test that output parameter is properly cleared and filled
  std::vector<uint8_t> compressed{9, 9, 9};  // Pre-filled with junk
  AggregatedMessageSerializer::compress_zstd(data, compressed);

  // Should not contain old data
  EXPECT_NE(compressed[0], 9);
  EXPECT_GT(compressed.size(), 0);

  // Verify it decompresses correctly
  std::vector<uint8_t> decompressed;
  AggregatedMessageSerializer::decompress_zstd(compressed, decompressed);
  EXPECT_EQ(decompressed, data);
}

TEST_F(MessageSerializerTest, DecompressZstdOutParameter) {
  std::vector<uint8_t> original{1, 2, 3, 4, 5};
  std::vector<uint8_t> compressed;
  AggregatedMessageSerializer::compress_zstd(original, compressed);

  // Test that output parameter is properly cleared
  std::vector<uint8_t> decompressed{9, 9, 9};  // Pre-filled
  AggregatedMessageSerializer::decompress_zstd(compressed, decompressed);

  EXPECT_EQ(decompressed, original);
  EXPECT_NE(decompressed.size(), 3);  // Should not be old size
}

TEST_F(MessageSerializerTest, LargeMessage) {
  // Test with a large message (10 KB)
  std::vector<uint8_t> large_data(10240);
  for (size_t i = 0; i < large_data.size(); ++i) {
    large_data[i] = static_cast<uint8_t>(i & 0xFF);
  }

  auto msg = create_test_message(large_data);
  serializer_.serialize_message("/large_topic", 999999, msg);

  const auto& serialized = serializer_.get_serialized_data();

  // Should contain the large message
  EXPECT_GT(serialized.size(), 10240);

  // Parse and verify
  size_t offset = 0;
  uint16_t topic_len = read_le<uint16_t>(serialized, offset);
  offset += topic_len + 16;  // Skip topic and timestamps

  int32_t data_len = read_le<int32_t>(serialized, offset);
  EXPECT_EQ(data_len, large_data.size());
}

TEST_F(MessageSerializerTest, MultipleTopicsSameTimestamp) {
  uint64_t timestamp = 123456789;

  auto msg1 = create_test_message({1, 2});
  auto msg2 = create_test_message({3, 4});
  auto msg3 = create_test_message({5, 6, 7});

  serializer_.serialize_message("/topicA", timestamp, msg1);
  serializer_.serialize_message("/topicB", timestamp, msg2);
  serializer_.serialize_message("/topicC", timestamp, msg3);

  // - Topic length: 2 bytes
  // - Topic string: 7 bytes
  // - Timestamps: 16 bytes
  // - Data length: 4 bytes
  // - Data: 2 bytes
  // - Subtotal: 2 + 7 + 8 + 8 + 4 + 2 = 31 bytes
  // + 31 (msg1) + 31 (msg2) + 32 (msg3) = 94 bytes

  const auto& serialized = serializer_.get_serialized_data();
  EXPECT_EQ(serialized.size(), 94);
}

TEST_F(MessageSerializerTest, ZeroCopySerializedMessage) {
  // Verify that we're working with SerializedMessage directly
  std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
  auto msg = create_test_message(data);

  serializer_.serialize_message("/topic", 100, msg);

  // Serializer should have read directly from the SerializedMessage
  // (This test mainly documents the expected behavior)
  const auto& serialized = serializer_.get_serialized_data();
  EXPECT_GT(serialized.size(), 4);

  // Verify data is correct
  size_t offset = 2 + 6 + 8 + 8 + 4;  // Skip to data
  EXPECT_EQ(serialized[offset], 0xDE);
  EXPECT_EQ(serialized[offset + 1], 0xAD);
  EXPECT_EQ(serialized[offset + 2], 0xBE);
  EXPECT_EQ(serialized[offset + 3], 0xEF);
}
