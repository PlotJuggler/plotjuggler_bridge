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

#include "pj_ros_bridge/middleware/zmq_middleware.hpp"

using namespace pj_ros_bridge;

class MiddlewareTest : public ::testing::Test {
 protected:
  void SetUp() override {
    middleware_ = std::make_unique<ZmqMiddleware>();
  }

  void TearDown() override {
    middleware_.reset();
  }

  std::unique_ptr<ZmqMiddleware> middleware_;
};

TEST_F(MiddlewareTest, InitializationSuccess) {
  EXPECT_FALSE(middleware_->is_ready());
  EXPECT_TRUE(middleware_->initialize(15555, 15556));
  EXPECT_TRUE(middleware_->is_ready());
}

TEST_F(MiddlewareTest, InitializationTwiceFails) {
  EXPECT_TRUE(middleware_->initialize(15555, 15556));
  EXPECT_FALSE(middleware_->initialize(15555, 15556));  // Second init should fail
}

TEST_F(MiddlewareTest, ShutdownWithoutInitialization) {
  // Should not crash
  middleware_->shutdown();
  EXPECT_FALSE(middleware_->is_ready());
}

TEST_F(MiddlewareTest, ShutdownAfterInitialization) {
  EXPECT_TRUE(middleware_->initialize(15555, 15556));
  EXPECT_TRUE(middleware_->is_ready());
  middleware_->shutdown();
  EXPECT_FALSE(middleware_->is_ready());
}

TEST_F(MiddlewareTest, ReceiveRequestWithoutInitialization) {
  std::vector<uint8_t> data;
  std::string client_id;
  EXPECT_FALSE(middleware_->receive_request(data, client_id));
}

TEST_F(MiddlewareTest, SendReplyWithoutInitialization) {
  std::vector<uint8_t> data = {1, 2, 3};
  EXPECT_FALSE(middleware_->send_reply(data));
}

TEST_F(MiddlewareTest, PublishDataWithoutInitialization) {
  std::vector<uint8_t> data = {1, 2, 3};
  EXPECT_FALSE(middleware_->publish_data(data));
}

TEST_F(MiddlewareTest, PublishDataAfterInitialization) {
  ASSERT_TRUE(middleware_->initialize(15555, 15556));

  // Give sockets time to bind
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  EXPECT_TRUE(middleware_->publish_data(data));
}

TEST_F(MiddlewareTest, ReceiveRequestTimeout) {
  ASSERT_TRUE(middleware_->initialize(15555, 15556));

  // Give sockets time to bind
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::vector<uint8_t> data;
  std::string client_id;

  // Should timeout since no client is sending data
  EXPECT_FALSE(middleware_->receive_request(data, client_id));
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
