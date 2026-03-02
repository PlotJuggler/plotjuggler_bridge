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

#include "pj_bridge/message_serializer.hpp"

#include <zstd.h>

#include <cstring>
#include <stdexcept>

#include "pj_bridge/protocol_constants.hpp"

namespace pj_bridge {

AggregatedMessageSerializer::AggregatedMessageSerializer() {}

void AggregatedMessageSerializer::serialize_message(
    const std::string &topic_name, uint64_t timestamp_ns, const std::byte *data, size_t data_size) {
  message_count_++;
  write_str(serialized_data_, topic_name);

  // Timestamp (uint64_t) - receive time
  write_le(serialized_data_, timestamp_ns);

  // Message data length (uint32_t)
  uint32_t msg_size = static_cast<uint32_t>(data_size);
  write_le(serialized_data_, msg_size);

  // Message data (CDR bytes) using memcpy
  size_t old_size = serialized_data_.size();
  serialized_data_.resize(old_size + msg_size);
  std::memcpy(serialized_data_.data() + old_size, data, msg_size);
}

void AggregatedMessageSerializer::clear() {
  serialized_data_.clear();
  message_count_ = 0;
}

size_t AggregatedMessageSerializer::get_message_count() const {
  return message_count_;
}

std::vector<uint8_t> AggregatedMessageSerializer::finalize() {
  // Build 16-byte header (uncompressed)
  std::vector<uint8_t> header(kBinaryHeaderSize);

  // Magic (offset 0, 4 bytes, little-endian)
  uint32_t magic = kBinaryFrameMagic;
  std::memcpy(header.data(), &magic, sizeof(magic));

  // Message count (offset 4, 4 bytes, little-endian)
  uint32_t count = static_cast<uint32_t>(message_count_);
  std::memcpy(header.data() + 4, &count, sizeof(count));

  // Uncompressed size (offset 8, 4 bytes, little-endian)
  uint32_t uncompressed = static_cast<uint32_t>(serialized_data_.size());
  std::memcpy(header.data() + 8, &uncompressed, sizeof(uncompressed));

  // Flags (offset 12, 4 bytes, reserved = 0)
  uint32_t flags = 0;
  std::memcpy(header.data() + 12, &flags, sizeof(flags));

  // Handle empty payload case
  if (serialized_data_.empty()) {
    return header;
  }

  // Compress the payload
  size_t max_compressed = ZSTD_compressBound(serialized_data_.size());
  std::vector<uint8_t> result(kBinaryHeaderSize + max_compressed);

  // Copy header to output
  std::memcpy(result.data(), header.data(), kBinaryHeaderSize);

  // Compress payload after header
  size_t compressed_size = ZSTD_compress(
      result.data() + kBinaryHeaderSize, max_compressed, serialized_data_.data(), serialized_data_.size(),
      1  // compression level
  );

  if (ZSTD_isError(compressed_size)) {
    throw std::runtime_error(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
  }

  // Resize to actual size: header + compressed payload
  result.resize(kBinaryHeaderSize + compressed_size);
  return result;
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

}  // namespace pj_bridge
