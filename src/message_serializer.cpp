// Copyright 2025
// ROS2 Bridge - Message Serializer Implementation

#include "pj_ros_bridge/message_serializer.hpp"

#include <zstd.h>

#include <cstring>
#include <stdexcept>

namespace pj_ros_bridge {

AggregatedMessageSerializer::AggregatedMessageSerializer() {
  messages_.reserve(100);  // Reserve space for typical aggregation
}

void AggregatedMessageSerializer::add_message(
    const std::string &topic_name, uint64_t publish_time_ns, uint64_t receive_time_ns,
    const std::vector<uint8_t> &data) {
  Message msg;
  msg.topic_name = topic_name;
  msg.publish_time_ns = publish_time_ns;
  msg.receive_time_ns = receive_time_ns;
  msg.data = data;
  messages_.push_back(std::move(msg));
}

std::vector<uint8_t> AggregatedMessageSerializer::serialize() const {
  std::vector<uint8_t> buffer;

  // Calculate approximate size to minimize reallocations
  size_t estimated_size = sizeof(uint32_t);  // message count
  for (const auto &msg : messages_) {
    estimated_size += sizeof(uint16_t) +       // topic name length
                      msg.topic_name.size() +  // topic name
                      sizeof(uint64_t) * 2 +   // timestamps
                      sizeof(uint32_t) +       // data length
                      msg.data.size();         // data
  }
  buffer.reserve(estimated_size);

  // Write message count
  write_le(buffer, static_cast<uint32_t>(messages_.size()));

  // Write each message
  for (const auto &msg : messages_) {
    // Topic name length (uint16_t)
    if (msg.topic_name.size() > UINT16_MAX) {
      throw std::runtime_error("Topic name too long: " + msg.topic_name);
    }
    write_le(buffer, static_cast<uint16_t>(msg.topic_name.size()));

    // Topic name (UTF-8 bytes)
    buffer.insert(buffer.end(), msg.topic_name.begin(), msg.topic_name.end());

    // Publish timestamp (uint64_t)
    write_le(buffer, msg.publish_time_ns);

    // Receive timestamp (uint64_t)
    write_le(buffer, msg.receive_time_ns);

    // Message data length (uint32_t)
    write_le(buffer, static_cast<uint32_t>(msg.data.size()));

    // Message data (CDR bytes)
    buffer.insert(buffer.end(), msg.data.begin(), msg.data.end());
  }

  return buffer;
}

void AggregatedMessageSerializer::clear() {
  messages_.clear();
}

size_t AggregatedMessageSerializer::message_count() const {
  return messages_.size();
}

std::vector<uint8_t> AggregatedMessageSerializer::compress_zstd(
    const std::vector<uint8_t> &data, int compression_level) {
  if (data.empty()) {
    return {};
  }

  // Get maximum compressed size
  size_t max_compressed_size = ZSTD_compressBound(data.size());
  std::vector<uint8_t> compressed(max_compressed_size);

  // Compress
  size_t compressed_size =
      ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), compression_level);

  // Check for errors
  if (ZSTD_isError(compressed_size)) {
    throw std::runtime_error(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
  }

  // Resize to actual compressed size
  compressed.resize(compressed_size);
  return compressed;
}

std::vector<uint8_t> AggregatedMessageSerializer::decompress_zstd(const std::vector<uint8_t> &compressed_data) {
  if (compressed_data.empty()) {
    return {};
  }

  // Get decompressed size
  unsigned long long decompressed_size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

  if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
    throw std::runtime_error("ZSTD: not compressed by zstd");
  }
  if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
    throw std::runtime_error("ZSTD: original size unknown");
  }

  // Allocate buffer for decompressed data
  std::vector<uint8_t> decompressed(decompressed_size);

  // Decompress
  size_t result =
      ZSTD_decompress(decompressed.data(), decompressed.size(), compressed_data.data(), compressed_data.size());

  // Check for errors
  if (ZSTD_isError(result)) {
    throw std::runtime_error(std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(result));
  }

  return decompressed;
}

template <typename T>
void AggregatedMessageSerializer::write_le(std::vector<uint8_t> &buffer, T value) {
  // Write value in little-endian format
  for (size_t i = 0; i < sizeof(T); ++i) {
    buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
  }
}

// Explicit template instantiations
template void AggregatedMessageSerializer::write_le<uint16_t>(std::vector<uint8_t> &, uint16_t);
template void AggregatedMessageSerializer::write_le<uint32_t>(std::vector<uint8_t> &, uint32_t);
template void AggregatedMessageSerializer::write_le<uint64_t>(std::vector<uint8_t> &, uint64_t);

}  // namespace pj_ros_bridge
