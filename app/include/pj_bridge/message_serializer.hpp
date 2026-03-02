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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pj_bridge {

/**
 * @brief Serializes aggregated messages using streaming zero-copy approach
 *
 * Design principles:
 * - Streaming API: Messages serialized immediately to output buffer (no intermediate storage)
 * - No header: Messages serialized sequentially without placeholder or count header
 * - Little-endian: All multi-byte values use little-endian byte order
 *
 * Binary format specification (per message):
 *   - Topic name length (uint16_t little-endian)
 *   - Topic name (N bytes UTF-8)
 *   - Timestamp (uint64_t nanoseconds since epoch, little-endian)
 *   - Message data length (uint32_t little-endian)
 *   - Message data (N bytes - CDR serialized)
 *
 * Thread safety: Not thread-safe, use from single thread only
 */
class AggregatedMessageSerializer {
 public:
  AggregatedMessageSerializer();

  /**
   * @brief Serialize a message directly to the output buffer
   *
   * @param topic_name Topic name
   * @param timestamp_ns Timestamp in nanoseconds since epoch
   * @param data Pointer to CDR serialized message bytes
   * @param data_size Size of the message data in bytes
   */
  void serialize_message(const std::string& topic_name, uint64_t timestamp_ns, const std::byte* data, size_t data_size);

  /**
   * @brief Get reference to the serialized data buffer
   */
  const std::vector<uint8_t>& get_serialized_data() const {
    return serialized_data_;
  }

  /**
   * @brief Clear the buffer for next aggregation cycle
   */
  void clear();

  /**
   * @brief Get number of messages added so far
   */
  size_t get_message_count() const;

  /**
   * @brief Finalize the aggregated messages with header and compression
   *
   * Produces output with 16-byte header (uncompressed) followed by ZSTD-compressed payload:
   *   - Offset 0: magic (uint32_t "PJRB" = 0x42524A50, little-endian)
   *   - Offset 4: message_count (uint32_t, little-endian)
   *   - Offset 8: uncompressed_size (uint32_t, little-endian)
   *   - Offset 12: flags (uint32_t, reserved = 0)
   *   - Offset 16+: ZSTD-compressed payload
   *
   * @return Vector containing header + compressed payload
   */
  std::vector<uint8_t> finalize();

  /**
   * @brief Compress data using ZSTD (compression level 1)
   */
  static void compress_zstd(const std::vector<uint8_t>& data, std::vector<uint8_t>& compressed_data);

  /**
   * @brief Decompress ZSTD data
   */
  static void decompress_zstd(const std::vector<uint8_t>& compressed_data, std::vector<uint8_t>& decompressed);

 private:
  std::vector<uint8_t> serialized_data_;
  size_t message_count_{0};

  template <typename T>
  static void write_le(std::vector<uint8_t>& buffer, T value);

  static void write_str(std::vector<uint8_t>& buffer, const std::string& str);
};

}  // namespace pj_bridge
