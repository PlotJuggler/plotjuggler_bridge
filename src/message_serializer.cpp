// Copyright 2025
// ROS2 Bridge - Message Serializer Implementation

#include "pj_ros_bridge/message_serializer.hpp"

#include <zstd.h>

#include <cstring>
#include <stdexcept>

namespace pj_ros_bridge {

AggregatedMessageSerializer::AggregatedMessageSerializer() {}

void AggregatedMessageSerializer::serialize_message(
    const std::string &topic_name, uint64_t timestamp_ns, const rclcpp::SerializedMessage &serialized_msg) {
  write_str(serialized_data_, topic_name);

  // Timestamp (uint64_t) - receive time
  write_le(serialized_data_, timestamp_ns);

  // Message data length (uint32_t)
  uint32_t msg_size = static_cast<uint32_t>(serialized_msg.size());
  write_le(serialized_data_, msg_size);

  // Message data (CDR bytes) using memcpy
  const uint8_t *read_ptr = static_cast<const uint8_t *>(serialized_msg.get_rcl_serialized_message().buffer);
  size_t old_size = serialized_data_.size();
  serialized_data_.resize(old_size + msg_size);
  std::memcpy(serialized_data_.data() + old_size, read_ptr, msg_size);
}

void AggregatedMessageSerializer::clear() {
  serialized_data_.clear();
}

void AggregatedMessageSerializer::compress_zstd(
    const std::vector<uint8_t> &data, std::vector<uint8_t> &compressed_data) {
  compressed_data.clear();
  if (data.empty()) {
    return;
  }

  // Get maximum compressed size
  size_t max_compressed_size = ZSTD_compressBound(data.size());
  compressed_data.resize(max_compressed_size);

  // Compress
  size_t compressed_size = ZSTD_compress(compressed_data.data(), compressed_data.size(), data.data(), data.size(), 1);

  // Check for errors
  if (ZSTD_isError(compressed_size)) {
    throw std::runtime_error(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
  }

  // Resize to actual compressed size
  compressed_data.resize(compressed_size);
}

void AggregatedMessageSerializer::decompress_zstd(
    const std::vector<uint8_t> &compressed_data, std::vector<uint8_t> &decompressed) {
  decompressed.clear();
  if (compressed_data.empty()) {
    return;
  }

  // Get decompressed size
  unsigned long long decompressed_size = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

  if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
    throw std::runtime_error("ZSTD: not compressed by zstd");
  }
  if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
    throw std::runtime_error("ZSTD: original size unknown");
  }
  decompressed.resize(decompressed_size);

  // Decompress
  size_t result =
      ZSTD_decompress(decompressed.data(), decompressed.size(), compressed_data.data(), compressed_data.size());

  // Check for errors
  if (ZSTD_isError(result)) {
    throw std::runtime_error(std::string("ZSTD decompression failed: ") + ZSTD_getErrorName(result));
  }
}

template <typename T>
void AggregatedMessageSerializer::write_le(std::vector<uint8_t> &buffer, T value) {
  // Write value in little-endian format
  for (size_t i = 0; i < sizeof(T); ++i) {
    buffer.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
  }
}

void AggregatedMessageSerializer::write_str(std::vector<uint8_t> &buffer, const std::string &str) {
  // Write string length
  write_le<uint16_t>(buffer, static_cast<uint16_t>(str.size()));
  // Write string data
  buffer.insert(buffer.end(), str.begin(), str.end());
}

// Explicit template instantiations
template void AggregatedMessageSerializer::write_le<uint16_t>(std::vector<uint8_t> &, uint16_t);
template void AggregatedMessageSerializer::write_le<uint32_t>(std::vector<uint8_t> &, uint32_t);
template void AggregatedMessageSerializer::write_le<uint64_t>(std::vector<uint8_t> &, uint64_t);

}  // namespace pj_ros_bridge
