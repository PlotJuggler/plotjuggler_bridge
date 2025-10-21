// Copyright 2025
// ROS2 Bridge - Message Serializer

#ifndef PJ_ROS_BRIDGE__MESSAGE_SERIALIZER_HPP_
#define PJ_ROS_BRIDGE__MESSAGE_SERIALIZER_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace pj_ros_bridge {

/**
 * @brief Serializes aggregated messages to custom binary format
 *
 * Binary format specification:
 * - Number of messages (uint32_t)
 * - For each message:
 *   - Topic name length (uint16_t)
 *   - Topic name (N bytes UTF-8)
 *   - Publish timestamp (uint64_t nanoseconds since epoch)
 *   - Receive timestamp (uint64_t nanoseconds since epoch)
 *   - Message data length (uint32_t)
 *   - Message data (N bytes - CDR serialized)
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
   * @brief Add a message to the aggregation
   *
   * @param topic_name Topic name
   * @param publish_time_ns Publish timestamp in nanoseconds since epoch
   * @param receive_time_ns Receive timestamp in nanoseconds since epoch
   * @param data Serialized message data (CDR format)
   */
  void add_message(
      const std::string& topic_name, uint64_t publish_time_ns, uint64_t receive_time_ns,
      const std::vector<uint8_t>& data);

  /**
   * @brief Serialize all added messages to binary format
   *
   * @return Serialized binary data
   */
  std::vector<uint8_t> serialize() const;

  /**
   * @brief Clear all messages and reset for next aggregation cycle
   */
  void clear();

  /**
   * @brief Get the number of messages added
   *
   * @return Number of messages
   */
  size_t message_count() const;

  /**
   * @brief Compress data using ZSTD
   *
   * @param data Data to compress
   * @param compression_level ZSTD compression level (default: 3)
   * @return Compressed data
   * @throws std::runtime_error if compression fails
   */
  static std::vector<uint8_t> compress_zstd(const std::vector<uint8_t>& data, int compression_level = 3);

  /**
   * @brief Decompress ZSTD data
   *
   * @param compressed_data Compressed data
   * @return Decompressed data
   * @throws std::runtime_error if decompression fails
   */
  static std::vector<uint8_t> decompress_zstd(const std::vector<uint8_t>& compressed_data);

 private:
  struct Message {
    std::string topic_name;
    uint64_t publish_time_ns;
    uint64_t receive_time_ns;
    std::vector<uint8_t> data;
  };

  std::vector<Message> messages_;

  /**
   * @brief Write a value to buffer in little-endian format
   */
  template <typename T>
  static void write_le(std::vector<uint8_t>& buffer, T value);
};

}  // namespace pj_ros_bridge

#endif  // PJ_ROS_BRIDGE__MESSAGE_SERIALIZER_HPP_
