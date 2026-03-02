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

#include <cstring>

#include "pj_bridge/message_serializer.hpp"

using namespace pj_bridge;

class MessageSerializerTest : public ::testing::Test {
 protected:
  AggregatedMessageSerializer serializer_;

  // Helper to create test data as a vector of bytes
  std::vector<std::byte> create_test_data(const std::vector<uint8_t>& data) {
    std::vector<std::byte> result(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
      result[i] = static_cast<std::byte>(data[i]);
    }
    return result;
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
  std::vector<uint8_t> raw_data = {0xAA, 0xBB, 0xCC};

  auto data = create_test_data(raw_data);
  serializer_.serialize_message(topic, timestamp, data.data(), data.size());

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

  // Timestamp
  uint64_t parsed_timestamp = read_le<uint64_t>(serialized, offset);
  EXPECT_EQ(parsed_timestamp, timestamp);

  // Data length (uint32_t)
  uint32_t data_len = read_le<uint32_t>(serialized, offset);
  EXPECT_EQ(data_len, raw_data.size());

  // Data
  std::vector<uint8_t> parsed_data(serialized.begin() + offset, serialized.begin() + offset + data_len);
  EXPECT_EQ(parsed_data, raw_data);
}

TEST_F(MessageSerializerTest, SerializeMultipleMessages) {
  // Add 3 messages with different data
  auto data1 = create_test_data({0x01, 0x02});
  auto data2 = create_test_data({0x03, 0x04, 0x05});
  auto data3 = create_test_data({0x06});

  serializer_.serialize_message("/topic1", 100, data1.data(), data1.size());
  serializer_.serialize_message("/topic2", 200, data2.data(), data2.size());
  serializer_.serialize_message("/topic3", 300, data3.data(), data3.size());

  const auto& serialized = serializer_.get_serialized_data();

  size_t offset = 0;

  // Verify we can parse all three messages
  for (int i = 0; i < 3; ++i) {
    uint16_t topic_len = read_le<uint16_t>(serialized, offset);
    EXPECT_GT(topic_len, 0);

    offset += topic_len;  // Skip topic name
    offset += 8;          // Skip timestamp

    uint32_t data_len = read_le<uint32_t>(serialized, offset);
    EXPECT_GT(data_len, 0);

    offset += data_len;  // Skip data
  }

  // Should have consumed entire buffer
  EXPECT_EQ(offset, serialized.size());
}

TEST_F(MessageSerializerTest, Clear) {
  auto data = create_test_data({1, 2, 3});
  serializer_.serialize_message("/topic", 100, data.data(), data.size());

  EXPECT_GT(serializer_.get_serialized_data().size(), 0);

  serializer_.clear();
  EXPECT_EQ(serializer_.get_serialized_data().size(), 0);
}

TEST_F(MessageSerializerTest, StreamingAPI) {
  // Test that messages are added immediately to buffer
  auto initial_size = serializer_.get_serialized_data().size();
  EXPECT_EQ(initial_size, 0);  // Empty

  auto data1 = create_test_data({1, 2, 3});
  serializer_.serialize_message("/topic1", 100, data1.data(), data1.size());
  auto size_after_1 = serializer_.get_serialized_data().size();
  EXPECT_GT(size_after_1, initial_size);

  auto data2 = create_test_data({4, 5, 6, 7});
  serializer_.serialize_message("/topic2", 200, data2.data(), data2.size());
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
  std::vector<uint8_t> large_raw(10240);
  for (size_t i = 0; i < large_raw.size(); ++i) {
    large_raw[i] = static_cast<uint8_t>(i & 0xFF);
  }

  auto large_data = create_test_data(large_raw);
  serializer_.serialize_message("/large_topic", 999999, large_data.data(), large_data.size());

  const auto& serialized = serializer_.get_serialized_data();

  // Should contain the large message
  EXPECT_GT(serialized.size(), 10240);

  // Parse and verify
  size_t offset = 0;
  uint16_t topic_len = read_le<uint16_t>(serialized, offset);
  offset += topic_len + 8;  // Skip topic and timestamp

  uint32_t data_len = read_le<uint32_t>(serialized, offset);
  EXPECT_EQ(data_len, large_raw.size());
}

TEST_F(MessageSerializerTest, MultipleTopicsSameTimestamp) {
  uint64_t timestamp = 123456789;

  auto data1 = create_test_data({1, 2});
  auto data2 = create_test_data({3, 4});
  auto data3 = create_test_data({5, 6, 7});

  serializer_.serialize_message("/topicA", timestamp, data1.data(), data1.size());
  serializer_.serialize_message("/topicB", timestamp, data2.data(), data2.size());
  serializer_.serialize_message("/topicC", timestamp, data3.data(), data3.size());

  // - Topic length: 2 bytes
  // - Topic string: 7 bytes
  // - Timestamp: 8 bytes
  // - Data length: 4 bytes
  // - Data: 2 bytes
  // - Subtotal: 2 + 7 + 8 + 4 + 2 = 23 bytes
  // + 23 (msg1) + 23 (msg2) + 24 (msg3) = 70 bytes

  const auto& serialized = serializer_.get_serialized_data();
  EXPECT_EQ(serialized.size(), 70);
}

TEST_F(MessageSerializerTest, LargeMessageForcesReallocation) {
  // This test forces vector reallocation by serializing many messages
  // with large payloads. Before the fix, this would crash with a dangling
  // pointer because dest_ptr was captured before resize().
  for (int i = 0; i < 100; ++i) {
    std::vector<uint8_t> payload_raw(1024, static_cast<uint8_t>(i & 0xFF));
    auto payload = create_test_data(payload_raw);
    serializer_.serialize_message(
        "/realloc_topic_" + std::to_string(i), static_cast<uint64_t>(i), payload.data(), payload.size());
  }

  const auto& serialized = serializer_.get_serialized_data();

  // Parse all 100 messages to verify data integrity
  size_t offset = 0;
  for (int i = 0; i < 100; ++i) {
    uint16_t topic_len = read_le<uint16_t>(serialized, offset);
    ASSERT_GT(topic_len, 0);

    std::string parsed_topic(serialized.begin() + offset, serialized.begin() + offset + topic_len);
    offset += topic_len;

    uint64_t ts = read_le<uint64_t>(serialized, offset);
    EXPECT_EQ(ts, static_cast<uint64_t>(i));

    uint32_t data_len = read_le<uint32_t>(serialized, offset);
    ASSERT_EQ(data_len, 1024);

    // Verify payload integrity
    for (int j = 0; j < 1024; ++j) {
      ASSERT_EQ(serialized[offset + j], static_cast<uint8_t>(i & 0xFF)) << "Mismatch at message " << i << " byte " << j;
    }
    offset += data_len;
  }
  EXPECT_EQ(offset, serialized.size());
}

TEST_F(MessageSerializerTest, WireFormatMatchesProjectSpec) {
  // PROJECT.md spec:
  //   - topic name: uint16_t length + N bytes
  //   - timestamp: uint64_t nanoseconds
  //   - data: uint32_t length + N bytes
  // Total for "/t" topic, 2-byte data = 2 + 2 + 8 + 4 + 2 = 18 bytes

  std::string topic = "/t";
  uint64_t timestamp = 42;
  std::vector<uint8_t> raw_data = {0xAA, 0xBB};

  auto data = create_test_data(raw_data);
  serializer_.serialize_message(topic, timestamp, data.data(), data.size());

  const auto& serialized = serializer_.get_serialized_data();

  // Expected total size: 2(topic_len) + 2(topic) + 8(timestamp) + 4(data_len) + 2(data) = 18
  EXPECT_EQ(serialized.size(), 18u);

  size_t offset = 0;

  // Topic name length (uint16_t)
  uint16_t topic_len = read_le<uint16_t>(serialized, offset);
  EXPECT_EQ(topic_len, 2u);

  // Topic name
  std::string parsed_topic(serialized.begin() + offset, serialized.begin() + offset + topic_len);
  offset += topic_len;
  EXPECT_EQ(parsed_topic, "/t");

  // Single timestamp (uint64_t)
  uint64_t parsed_ts = read_le<uint64_t>(serialized, offset);
  EXPECT_EQ(parsed_ts, 42u);

  // Data length (uint32_t)
  uint32_t data_len = read_le<uint32_t>(serialized, offset);
  EXPECT_EQ(data_len, 2u);

  // Data
  EXPECT_EQ(serialized[offset], 0xAA);
  EXPECT_EQ(serialized[offset + 1], 0xBB);
}

TEST_F(MessageSerializerTest, ZeroCopySerializedMessage) {
  // Verify that we're working with raw byte data directly
  std::vector<uint8_t> raw_data = {0xDE, 0xAD, 0xBE, 0xEF};
  auto data = create_test_data(raw_data);

  serializer_.serialize_message("/topic", 100, data.data(), data.size());

  const auto& serialized = serializer_.get_serialized_data();
  EXPECT_GT(serialized.size(), 4);

  // Verify data is correct: 2(topic_len) + 6(topic) + 8(ts) + 4(data_len) = 20
  size_t offset = 2 + 6 + 8 + 4;
  EXPECT_EQ(serialized[offset], 0xDE);
  EXPECT_EQ(serialized[offset + 1], 0xAD);
  EXPECT_EQ(serialized[offset + 2], 0xBE);
  EXPECT_EQ(serialized[offset + 3], 0xEF);
}

// ============================================================================
// Binary Frame Header Tests (16-byte header prepended to compressed payload)
// ============================================================================

TEST_F(MessageSerializerTest, FinalizeIncludesBinaryHeader) {
  auto data = create_test_data({1, 2, 3, 4});
  serializer_.serialize_message("/topic", 1000, data.data(), data.size());

  auto result = serializer_.finalize();

  // Must have at least 16-byte header
  ASSERT_GE(result.size(), 16u);

  // Check magic (little-endian "PJRB" = 0x42524A50)
  uint32_t magic;
  std::memcpy(&magic, result.data(), sizeof(magic));
  EXPECT_EQ(magic, 0x42524A50u);

  // Check message count
  uint32_t count;
  std::memcpy(&count, result.data() + 4, sizeof(count));
  EXPECT_EQ(count, 1u);

  // Check uncompressed size is non-zero
  uint32_t uncompressed;
  std::memcpy(&uncompressed, result.data() + 8, sizeof(uncompressed));
  EXPECT_GT(uncompressed, 0u);

  // Check flags are 0
  uint32_t flags;
  std::memcpy(&flags, result.data() + 12, sizeof(flags));
  EXPECT_EQ(flags, 0u);
}

TEST_F(MessageSerializerTest, HeaderMagicIsPJRB) {
  auto data = create_test_data({0});
  serializer_.serialize_message("/t", 0, data.data(), data.size());
  auto result = serializer_.finalize();

  // Verify magic bytes spell "PJRB"
  EXPECT_EQ(result[0], 'P');
  EXPECT_EQ(result[1], 'J');
  EXPECT_EQ(result[2], 'R');
  EXPECT_EQ(result[3], 'B');
}

TEST_F(MessageSerializerTest, MessageCountMatchesAddedMessages) {
  auto data1 = create_test_data({1});
  auto data2 = create_test_data({2});
  auto data3 = create_test_data({3});

  serializer_.serialize_message("/a", 100, data1.data(), data1.size());
  serializer_.serialize_message("/b", 200, data2.data(), data2.size());
  serializer_.serialize_message("/c", 300, data3.data(), data3.size());

  EXPECT_EQ(serializer_.get_message_count(), 3u);

  auto result = serializer_.finalize();
  uint32_t count;
  std::memcpy(&count, result.data() + 4, sizeof(count));
  EXPECT_EQ(count, 3u);
}

TEST_F(MessageSerializerTest, ClearResetsMessageCount) {
  auto data = create_test_data({0});
  serializer_.serialize_message("/t", 0, data.data(), data.size());
  EXPECT_EQ(serializer_.get_message_count(), 1u);

  serializer_.clear();
  EXPECT_EQ(serializer_.get_message_count(), 0u);
}

TEST_F(MessageSerializerTest, DecompressedPayloadMatchesOriginal) {
  std::vector<uint8_t> raw_data = {10, 20, 30, 40, 50};
  auto data = create_test_data(raw_data);
  serializer_.serialize_message("/test", 12345, data.data(), data.size());

  auto result = serializer_.finalize();

  // Extract header info
  uint32_t uncompressed_size;
  std::memcpy(&uncompressed_size, result.data() + 8, sizeof(uncompressed_size));

  // Decompress payload (skip 16-byte header)
  std::vector<uint8_t> compressed(result.begin() + 16, result.end());
  std::vector<uint8_t> decompressed;
  AggregatedMessageSerializer::decompress_zstd(compressed, decompressed);

  EXPECT_EQ(decompressed.size(), uncompressed_size);

  // Payload should match the original serialized data
  EXPECT_EQ(decompressed, serializer_.get_serialized_data());
}

TEST_F(MessageSerializerTest, FinalizeEmptyBuffer) {
  // Finalize with no messages should still produce valid header
  auto result = serializer_.finalize();

  ASSERT_GE(result.size(), 16u);

  uint32_t magic;
  std::memcpy(&magic, result.data(), sizeof(magic));
  EXPECT_EQ(magic, 0x42524A50u);

  uint32_t count;
  std::memcpy(&count, result.data() + 4, sizeof(count));
  EXPECT_EQ(count, 0u);

  uint32_t uncompressed;
  std::memcpy(&uncompressed, result.data() + 8, sizeof(uncompressed));
  EXPECT_EQ(uncompressed, 0u);
}

TEST_F(MessageSerializerTest, UncompressedSizeMatchesPayload) {
  // Create a message with known serialized size
  std::string topic = "/t";
  std::vector<uint8_t> raw_data = {0xAA, 0xBB};
  auto data = create_test_data(raw_data);

  serializer_.serialize_message(topic, 42, data.data(), data.size());

  // Expected size: 2(topic_len) + 2(topic) + 8(timestamp) + 4(data_len) + 2(data) = 18
  EXPECT_EQ(serializer_.get_serialized_data().size(), 18u);

  auto result = serializer_.finalize();

  uint32_t uncompressed_size;
  std::memcpy(&uncompressed_size, result.data() + 8, sizeof(uncompressed_size));
  EXPECT_EQ(uncompressed_size, 18u);
}
