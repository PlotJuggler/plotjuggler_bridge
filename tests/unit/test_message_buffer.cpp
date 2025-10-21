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
};

TEST_F(MessageBufferTest, AddAndGetMessages) {
  std::vector<uint8_t> data1 = {1, 2, 3};
  std::vector<uint8_t> data2 = {4, 5, 6};

  // Get current time
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  buffer_.add_message("topic1", current_time_ns, current_time_ns, data1);
  buffer_.add_message("topic2", current_time_ns, current_time_ns, data2);

  auto messages = buffer_.get_new_messages();

  EXPECT_EQ(messages.size(), 2);

  // Check that both messages are present (order not guaranteed)
  bool found_topic1 = false;
  bool found_topic2 = false;
  for (const auto& msg : messages) {
    if (msg.topic_name == "topic1" && msg.data == data1) {
      found_topic1 = true;
    }
    if (msg.topic_name == "topic2" && msg.data == data2) {
      found_topic2 = true;
    }
  }
  EXPECT_TRUE(found_topic1);
  EXPECT_TRUE(found_topic2);
}

TEST_F(MessageBufferTest, GetNewMessagesOnlyReturnsNew) {
  std::vector<uint8_t> data1 = {1, 2, 3};
  std::vector<uint8_t> data2 = {4, 5, 6};

  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  buffer_.add_message("topic1", current_time_ns, current_time_ns, data1);

  // First read
  auto messages1 = buffer_.get_new_messages();
  EXPECT_EQ(messages1.size(), 1);

  // Sleep to ensure time passes
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Get new timestamp for the second message
  now = std::chrono::system_clock::now();
  duration = now.time_since_epoch();
  current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  // Add another message
  buffer_.add_message("topic2", current_time_ns, current_time_ns, data2);

  // Second read should only return the new message
  auto messages2 = buffer_.get_new_messages();
  EXPECT_EQ(messages2.size(), 1);
  EXPECT_EQ(messages2[0].topic_name, "topic2");
}

TEST_F(MessageBufferTest, GetNewMessagesByTopic) {
  std::vector<uint8_t> data1 = {1, 2, 3};
  std::vector<uint8_t> data2 = {4, 5, 6};
  std::vector<uint8_t> data3 = {7, 8, 9};

  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  buffer_.add_message("topic1", current_time_ns, current_time_ns, data1);
  buffer_.add_message("topic2", current_time_ns, current_time_ns, data2);
  buffer_.add_message("topic1", current_time_ns + 1, current_time_ns + 1, data3);

  auto messages = buffer_.get_new_messages("topic1");

  EXPECT_EQ(messages.size(), 2);
  // Verify all messages are from topic1
  for (const auto& msg : messages) {
    EXPECT_EQ(msg.topic_name, "topic1");
  }
  // Verify both data1 and data3 are present
  bool found_data1 = false;
  bool found_data3 = false;
  for (const auto& msg : messages) {
    if (msg.data == data1) {
      found_data1 = true;
    }
    if (msg.data == data3) {
      found_data3 = true;
    }
  }
  EXPECT_TRUE(found_data1);
  EXPECT_TRUE(found_data3);
}

TEST_F(MessageBufferTest, Clear) {
  std::vector<uint8_t> data = {1, 2, 3};

  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  buffer_.add_message("topic1", current_time_ns, current_time_ns, data);

  EXPECT_EQ(buffer_.size(), 1);

  buffer_.clear();

  EXPECT_EQ(buffer_.size(), 0);

  auto messages = buffer_.get_new_messages();
  EXPECT_EQ(messages.size(), 0);
}

TEST_F(MessageBufferTest, Size) {
  std::vector<uint8_t> data = {1, 2, 3};

  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  EXPECT_EQ(buffer_.size(), 0);

  buffer_.add_message("topic1", current_time_ns, current_time_ns, data);
  EXPECT_EQ(buffer_.size(), 1);

  buffer_.add_message("topic2", current_time_ns, current_time_ns, data);
  EXPECT_EQ(buffer_.size(), 2);

  buffer_.add_message("topic1", current_time_ns, current_time_ns, data);
  EXPECT_EQ(buffer_.size(), 3);
}

TEST_F(MessageBufferTest, AutoCleanupOldMessages) {
  std::vector<uint8_t> data = {1, 2, 3};

  // Get current time
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  // Add old message (older than 1 second)
  uint64_t old_time_ns = current_time_ns - 2'000'000'000;  // 2 seconds ago
  buffer_.add_message("topic1", old_time_ns, old_time_ns, data);

  // Add recent message
  buffer_.add_message("topic2", current_time_ns, current_time_ns, data);

  // The old message should have been cleaned up automatically
  EXPECT_EQ(buffer_.size(), 1);

  auto messages = buffer_.get_new_messages();
  EXPECT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].topic_name, "topic2");
}

TEST_F(MessageBufferTest, CleanupPreservesRecentMessages) {
  std::vector<uint8_t> data = {1, 2, 3};

  // Get current time
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  // Add two recent messages (should not be cleaned up)
  buffer_.add_message("topic1", current_time_ns, current_time_ns, data);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  now = std::chrono::system_clock::now();
  duration = now.time_since_epoch();
  current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  // Trigger cleanup by adding another message
  buffer_.add_message("topic2", current_time_ns, current_time_ns, data);

  // Both messages should still be there (neither older than 1 second)
  EXPECT_EQ(buffer_.size(), 2);

  auto messages = buffer_.get_new_messages();
  EXPECT_EQ(messages.size(), 2);
}

TEST_F(MessageBufferTest, ThreadSafety) {
  std::vector<uint8_t> data = {1, 2, 3};
  const int num_threads = 10;
  const int messages_per_thread = 100;

  std::vector<std::thread> threads;

  // Get current time for all messages
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  uint64_t current_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  // Multiple threads adding messages
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([this, &data, i, messages_per_thread, current_time_ns]() {
      for (int j = 0; j < messages_per_thread; ++j) {
        std::string topic_name = "topic" + std::to_string(i);
        buffer_.add_message(topic_name, current_time_ns, current_time_ns, data);
      }
    });
  }

  // One thread reading messages
  threads.emplace_back([this]() {
    for (int i = 0; i < 10; ++i) {
      buffer_.get_new_messages();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  for (auto& t : threads) {
    t.join();
  }

  // No crash means thread safety is working
  EXPECT_GT(buffer_.size(), 0);
}
