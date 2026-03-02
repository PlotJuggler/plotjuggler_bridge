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

#include <atomic>
#include <chrono>
#include <thread>

#include "pj_bridge/message_buffer.hpp"

using namespace pj_bridge;

class MessageBufferTest : public ::testing::Test {
 protected:
  MessageBuffer buffer_;

  // Helper function to create test data as shared_ptr<vector<byte>>
  std::shared_ptr<std::vector<std::byte>> create_test_data(const std::vector<uint8_t>& data) {
    auto vec = std::make_shared<std::vector<std::byte>>(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
      (*vec)[i] = static_cast<std::byte>(data[i]);
    }
    return vec;
  }

  // Helper to extract data from shared_ptr<vector<byte>> for comparison
  std::vector<uint8_t> extract_data(const std::shared_ptr<std::vector<std::byte>>& data) {
    std::vector<uint8_t> result(data->size());
    for (size_t i = 0; i < data->size(); ++i) {
      result[i] = static_cast<uint8_t>((*data)[i]);
    }
    return result;
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

  auto msg1 = create_test_data(data1);
  auto msg2 = create_test_data(data2);

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
  EXPECT_EQ(extract_data(messages["topic1"][0].data), data1);
  EXPECT_EQ(extract_data(messages["topic2"][0].data), data2);

  // Verify timestamps
  EXPECT_EQ(messages["topic1"][0].timestamp_ns, current_time_ns);
  EXPECT_EQ(messages["topic2"][0].timestamp_ns, current_time_ns);
}

TEST_F(MessageBufferTest, MoveMessagesEmptiesBuffer) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_data(data));
  buffer_.add_message("topic1", current_time_ns + 1, create_test_data(data));

  EXPECT_EQ(buffer_.size(), 2);

  // First move
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages1;
  buffer_.move_messages(messages1);
  EXPECT_EQ(buffer_.size(), 0);
  EXPECT_EQ(messages1["topic1"].size(), 2);

  // Add new message
  buffer_.add_message("topic2", current_time_ns + 2, create_test_data(data));
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

  buffer_.add_message("topic1", current_time_ns, create_test_data(data1));
  buffer_.add_message("topic2", current_time_ns + 1, create_test_data(data2));
  buffer_.add_message("topic1", current_time_ns + 2, create_test_data(data3));

  EXPECT_EQ(buffer_.size(), 3);

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);

  EXPECT_EQ(messages.size(), 2);
  EXPECT_EQ(messages["topic1"].size(), 2);
  EXPECT_EQ(messages["topic2"].size(), 1);

  // Verify topic1 has data1 and data3 in order
  EXPECT_EQ(extract_data(messages["topic1"][0].data), data1);
  EXPECT_EQ(extract_data(messages["topic1"][1].data), data3);
  EXPECT_EQ(messages["topic1"][0].timestamp_ns, current_time_ns);
  EXPECT_EQ(messages["topic1"][1].timestamp_ns, current_time_ns + 2);

  // Verify topic2 has data2
  EXPECT_EQ(extract_data(messages["topic2"][0].data), data2);
  EXPECT_EQ(messages["topic2"][0].timestamp_ns, current_time_ns + 1);
}

TEST_F(MessageBufferTest, Clear) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_data(data));
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

  buffer_.add_message("topic1", current_time_ns, create_test_data(data));
  EXPECT_EQ(buffer_.size(), 1);

  buffer_.add_message("topic2", current_time_ns, create_test_data(data));
  EXPECT_EQ(buffer_.size(), 2);

  buffer_.add_message("topic1", current_time_ns, create_test_data(data));
  EXPECT_EQ(buffer_.size(), 3);
}

TEST_F(MessageBufferTest, AutoCleanupOldMessages) {
  // Use a buffer with a short max age (20ms) so we can test cleanup quickly.
  // Cleanup is based on received_at_ns (wall-clock time when add_message is called),
  // not the user-provided timestamp_ns.
  MessageBuffer short_lived_buffer(20'000'000);  // 20ms max age

  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  // Add first message
  short_lived_buffer.add_message("topic1", current_time_ns, create_test_data(data));

  // Wait long enough for the first message to become "old" by received_at_ns
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  // Add second message — this triggers cleanup, which should evict topic1
  short_lived_buffer.add_message("topic2", current_time_ns, create_test_data(data));

  // The old message should have been cleaned up automatically
  EXPECT_EQ(short_lived_buffer.size(), 1);

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  short_lived_buffer.move_messages(messages);
  EXPECT_EQ(messages.size(), 1);
  EXPECT_TRUE(messages.find("topic2") != messages.end());
  EXPECT_TRUE(messages.find("topic1") == messages.end());
}

TEST_F(MessageBufferTest, CleanupPreservesRecentMessages) {
  // Use a short max age to verify that messages within the window survive
  MessageBuffer short_lived_buffer(100'000'000);  // 100ms max age

  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  // Add two messages in quick succession (both within 100ms window)
  short_lived_buffer.add_message("topic1", current_time_ns, create_test_data(data));

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  current_time_ns = get_current_time_ns();

  // Trigger cleanup by adding another message
  short_lived_buffer.add_message("topic2", current_time_ns, create_test_data(data));

  // Both messages should still be there (neither older than 100ms)
  EXPECT_EQ(short_lived_buffer.size(), 2);

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  short_lived_buffer.move_messages(messages);
  EXPECT_EQ(messages.size(), 2);
}

TEST_F(MessageBufferTest, SharedPtrOwnership) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  auto msg = create_test_data(data);
  auto weak_ptr = std::weak_ptr<std::vector<std::byte>>(msg);

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
        buffer_.add_message(topic_name, current_time_ns + j, create_test_data(data));
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

TEST_F(MessageBufferTest, MoveMessagesReplace) {
  // Verify move_messages replaces the output map contents (not swap)
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_data(data));

  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  buffer_.move_messages(messages);

  // messages should have topic1
  EXPECT_EQ(messages.size(), 1);
  EXPECT_TRUE(messages.find("topic1") != messages.end());

  // Buffer should be empty after move
  EXPECT_EQ(buffer_.size(), 0);
}

TEST_F(MessageBufferTest, MoveMessagesOverwritesExistingOutput) {
  std::vector<uint8_t> data = {1, 2, 3};
  uint64_t current_time_ns = get_current_time_ns();

  buffer_.add_message("topic1", current_time_ns, create_test_data(data));

  // Prepare a non-empty map with pre-existing data
  std::unordered_map<std::string, std::deque<BufferedMessage>> messages;
  messages["old_topic"].push_back(BufferedMessage{current_time_ns, current_time_ns, create_test_data({9, 9, 9})});

  // move_messages should overwrite the output map
  buffer_.move_messages(messages);

  // messages should now have topic1, not old_topic
  EXPECT_EQ(messages.size(), 1);
  EXPECT_TRUE(messages.find("topic1") != messages.end());
  EXPECT_TRUE(messages.find("old_topic") == messages.end());

  // Buffer should be empty after move
  EXPECT_EQ(buffer_.size(), 0);
}
