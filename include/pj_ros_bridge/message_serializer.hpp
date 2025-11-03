// Copyright 2025
// ROS2 Bridge - Message Serializer

#ifndef PJ_ROS_BRIDGE__MESSAGE_SERIALIZER_HPP_
#define PJ_ROS_BRIDGE__MESSAGE_SERIALIZER_HPP_

#include <cstdint>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <vector>

namespace pj_ros_bridge {

/**
 * @brief Serializes aggregated messages using streaming zero-copy approach
 *
 * Design principles:
 * - Streaming API: Messages serialized immediately to output buffer (no intermediate storage)
 * - Zero-copy: Reads directly from SerializedMessage without data duplication
 * - No header: Messages serialized sequentially without placeholder or count header
 * - Little-endian: All multi-byte values use little-endian byte order
 *
 * Binary format specification (per message):
 *   - Topic name length (uint16_t little-endian)
 *   - Topic name (N bytes UTF-8)
 *   - Timestamp (uint64_t nanoseconds since epoch, little-endian)
 *   - Message data length (int32_t little-endian)
 *   - Message data (N bytes - CDR serialized from ROS2)
 *
 * Thread safety: Not thread-safe, use from single thread only
 */
class AggregatedMessageSerializer {
 public:
  /**
   * @brief Constructor
   */
  AggregatedMessageSerializer();

  /**
   * @brief Serialize a message directly to the output buffer
   *
   * @param topic_name Topic name
   * @param timestamp_ns Timestamp in nanoseconds since epoch
   * @param serialized_msg ROS2 serialized message (CDR format)
   */
  void serialize_message(
      const std::string& topic_name, uint64_t timestamp_ns, const rclcpp::SerializedMessage& serialized_msg);

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
   * @brief Compress data using ZSTD (compression level 1)
   *
   * @param data Data to compress
   * @param compressed_data Output parameter - receives compressed data
   * @throws std::runtime_error if compression fails
   */
  static void compress_zstd(const std::vector<uint8_t>& data, std::vector<uint8_t>& compressed_data);

  /**
   * @brief Decompress ZSTD data
   *
   * @param compressed_data Compressed data
   * @param decompressed Output parameter - receives decompressed data
   * @throws std::runtime_error if decompression fails
   */
  static void decompress_zstd(const std::vector<uint8_t>& compressed_data, std::vector<uint8_t>& decompressed);

 private:
  // Buffer for serialized data
  std::vector<uint8_t> serialized_data_;

  /**
   * @brief Write a value to buffer in little-endian format
   */
  template <typename T>
  static void write_le(std::vector<uint8_t>& buffer, T value);

  static void write_str(std::vector<uint8_t>& buffer, const std::string& str);
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MESSAGE_SERIALIZER_HPP_
