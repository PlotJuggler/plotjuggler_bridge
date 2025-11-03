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

#include <chrono>
#include <thread>

#include "pj_ros_bridge/message_buffer.hpp"

using namespace pj_ros_bridge;

class MessageBufferTest : public ::testing::Test {
 protected:
  MessageBuffer buffer_;

  // Helper function to create a serialized message with test data
  std::shared_ptr<rclcpp::SerializedMessage> create_test_message(const std::vector<uint8_t>& data) {
    auto msg = std::make_shared<rclcpp::SerializedMessage>(data.size());
    auto& rcl_msg = msg->get_rcl_serialized_message();
    std::memcpy(rcl_msg.buffer, data.data(), data.size());
    rcl_msg.buffer_length = data.size();
    return msg;
  }

  // Helper to extract data from serialized message for comparison
  std::vector<uint8_t> extract_data(const std::shared_ptr<rclcpp::SerializedMessage>& msg) {
    const auto& rcl_msg = msg->get_rcl_serialized_message();
    return std::vector<uint8_t>(rcl_msg.buffer, rcl_msg.buffer + rcl_msg.buffer_length);
  }

  uint64_t get_current_time_ns() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  }
};

TEST_F(MessageBufferTest, AddAndMoveMessages) {
  std::vector<uint8_t> data1 = {1, 2, 3};
  std::vector<uint8_t> data2 = {4, 5, 6};

  uint64_t current_time_ns = get_current_time_ns();

  auto msg1 = create_test_message(data1);
  auto msg2 = create_test_message(data2);

  buffer_.add_message("topic1", current_time_ns, msg1);
  buffer_.add_message("topic2", current_time_ns, msg2);

  EXPECT_EQ(buffer_.size(), 2);

  // Move messages out
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);

  // Buffer should be empty after move
  EXPECT_EQ(buffer_.size(), 0);

  // Should have 2 topics
  EXPECT_EQ(messages.size(), 2);
  EXPECT_TRUE(messages.find("topic1") != messages.end());
  EXPECT_TRUE(messages.find("topic2") != messages.end());

  // Each topic should have 1 message
  EXPECT_EQ(messages["topic1"].size(), 1);
  EXPECT_EQ(messages["topic2"].size(), 1);

  // Verify message data
  EXPECT_EQ(extract_data(messages["topic1"][0].msg), data1);
  EXPECT_EQ(extract_data(messages["topic2"][0].msg), data2);

  // Verify timestamps
  EXPECT_EQ(messages["topic1"][0].timestamp_ns, current_time_ns);
  EXPECT_EQ(messages["topic2"][0].timestamp_ns, current_time_ns);
}

TEST_F(MessageBufferTest, MoveMessagesEmptiesBuffer) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_message(data));
  buffer_.add_message("topic1", current_time_ns + 1, create_test_message(data));

  EXPECT_EQ(buffer_.size(), 2);

  // First move
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages1;
  buffer_.move_messages(messages1);
  EXPECT_EQ(buffer_.size(), 0);
  EXPECT_EQ(messages1["topic1"].size(), 2);

  // Add new message
  buffer_.add_message("topic2", current_time_ns + 2, create_test_message(data));
  EXPECT_EQ(buffer_.size(), 1);

  // Second move should only return the new message
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages2;
  buffer_.move_messages(messages2);
  EXPECT_EQ(buffer_.size(), 0);
  EXPECT_EQ(messages2.size(), 1);
  EXPECT_TRUE(messages2.find("topic2") != messages2.end());
  EXPECT_EQ(messages2["topic2"].size(), 1);
}

TEST_F(MessageBufferTest, MultipleMessagesPerTopic) {
  std::vector<uint8_t> data1 = {1, 2, 3};
  std::vector<uint8_t> data2 = {4, 5, 6};
  std::vector<uint8_t> data3 = {7, 8, 9};

  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_message(data1));
  buffer_.add_message("topic2", current_time_ns + 1, create_test_message(data2));
  buffer_.add_message("topic1", current_time_ns + 2, create_test_message(data3));

  EXPECT_EQ(buffer_.size(), 3);

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);

  EXPECT_EQ(messages.size(), 2);
  EXPECT_EQ(messages["topic1"].size(), 2);
  EXPECT_EQ(messages["topic2"].size(), 1);

  // Verify topic1 has data1 and data3 in order
  EXPECT_EQ(extract_data(messages["topic1"][0].msg), data1);
  EXPECT_EQ(extract_data(messages["topic1"][1].msg), data3);
  EXPECT_EQ(messages["topic1"][0].timestamp_ns, current_time_ns);
  EXPECT_EQ(messages["topic1"][1].timestamp_ns, current_time_ns + 2);

  // Verify topic2 has data2
  EXPECT_EQ(extract_data(messages["topic2"][0].msg), data2);
  EXPECT_EQ(messages["topic2"][0].timestamp_ns, current_time_ns + 1);
}

TEST_F(MessageBufferTest, Clear) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_message(data));
  EXPECT_EQ(buffer_.size(), 1);

  buffer_.clear();
  EXPECT_EQ(buffer_.size(), 0);

  // Move should return empty map
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);
  EXPECT_EQ(messages.size(), 0);
}

TEST_F(MessageBufferTest, Size) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  EXPECT_EQ(buffer_.size(), 0);

  buffer_.add_message("topic1", current_time_ns, create_test_message(data));
  EXPECT_EQ(buffer_.size(), 1);

  buffer_.add_message("topic2", current_time_ns, create_test_message(data));
  EXPECT_EQ(buffer_.size(), 2);

  buffer_.add_message("topic1", current_time_ns, create_test_message(data));
  EXPECT_EQ(buffer_.size(), 3);
}

TEST_F(MessageBufferTest, AutoCleanupOldMessages) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  // Add old message (older than 1 second)
  uint64_t old_time_ns = current_time_ns - 2'000'000'000;  // 2 seconds ago
  buffer_.add_message("topic1", old_time_ns, create_test_message(data));

  // Add recent message (this should trigger cleanup)
  buffer_.add_message("topic2", current_time_ns, create_test_message(data));

  // The old message should have been cleaned up automatically
  EXPECT_EQ(buffer_.size(), 1);

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);
  EXPECT_EQ(messages.size(), 1);
  EXPECT_TRUE(messages.find("topic2") != messages.end());
  EXPECT_TRUE(messages.find("topic1") == messages.end());
}

TEST_F(MessageBufferTest, CleanupPreservesRecentMessages) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  // Add two recent messages (should not be cleaned up)
  buffer_.add_message("topic1", current_time_ns, create_test_message(data));

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  current_time_ns = get_current_time_ns();

  // Trigger cleanup by adding another message
  buffer_.add_message("topic2", current_time_ns, create_test_message(data));

  // Both messages should still be there (neither older than 1 second)
  EXPECT_EQ(buffer_.size(), 2);

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);
  EXPECT_EQ(messages.size(), 2);
}

TEST_F(MessageBufferTest, SharedPtrOwnership) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  auto msg = create_test_message(data);
  auto weak_ptr = std::weak_ptr<rclcpp::SerializedMessage>(msg);

  // Add message to buffer
  buffer_.add_message("topic1", current_time_ns, msg);

  // Original shared_ptr still valid
  EXPECT_FALSE(weak_ptr.expired());
  EXPECT_EQ(msg.use_count(), 2);  // Original + buffer

  // Reset original
  msg.reset();
  EXPECT_FALSE(weak_ptr.expired());  // Buffer still holds reference

  // Move messages out
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);

  EXPECT_FALSE(weak_ptr.expired());  // Moved to messages map
  EXPECT_EQ(weak_ptr.use_count(), 1);

  // Clear messages map
  messages.clear();
  EXPECT_TRUE(weak_ptr.expired());  // Now deallocated
}

TEST_F(MessageBufferTest, ThreadSafety) {
  std::vector<uint8_t> data = {1, 2, 3};
  const int num_threads = 10;
  const int messages_per_thread = 100;

  std::vector<std::thread> threads;
  uint64_t current_time_ns = get_current_time_ns();

  // Multiple threads adding messages
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([this, &data, i, messages_per_thread, current_time_ns]() {
      for (int j = 0; j < messages_per_thread; ++j) {
        std::string topic_name = "topic" + std::to_string(i);
        buffer_.add_message(topic_name, current_time_ns + j, create_test_message(data));
      }
    });
  }

  // One thread moving messages
  std::atomic<int> total_moved{0};
  threads.emplace_back([this, &total_moved]() {
    for (int i = 0; i < 10; ++i) {
      std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
      buffer_.move_messages(messages);
      for (const auto& [topic, msgs] : messages) {
        total_moved += msgs.size();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  for (auto& t : threads) {
    t.join();
  }

  // Get any remaining messages
  std::unordered_map<std::string, std::deque<BufferedMessage>> final_messages;
  buffer_.move_messages(final_messages);
  for (const auto& [topic, msgs] : final_messages) {
    total_moved += msgs.size();
  }

  // All messages should be accounted for
  EXPECT_EQ(total_moved.load(), num_threads * messages_per_thread);
}

TEST_F(MessageBufferTest, MoveMessagesSwap) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_message(data));

  // Prepare a non-empty map
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  messages["old_topic"].push_back(BufferedMessage{current_time_ns, create_test_message({9, 9, 9})});

  // move_messages should swap, so old data should be moved into buffer
  buffer_.move_messages(messages);

  // messages should now have topic1, not old_topic
  EXPECT_EQ(messages.size(), 1);
  EXPECT_TRUE(messages.find("topic1") != messages.end());
  EXPECT_TRUE(messages.find("old_topic") == messages.end());

  // Buffer should be empty after swap
  EXPECT_EQ(buffer_.size(), 0);
}
