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

#include "pj_ros_bridge/middleware/websocket_middleware.hpp"

using namespace pj_ros_bridge;

class WebSocketMiddlewareTest : public ::testing::Test {
 protected:
  void SetUp() override {
    middleware_ = std::make_unique<WebSocketMiddleware>();
  }

  void TearDown() override {
    middleware_.reset();
  }

  std::unique_ptr<WebSocketMiddleware> middleware_;
};

TEST_F(WebSocketMiddlewareTest, InitializationSuccess) {
  EXPECT_FALSE(middleware_->is_ready());
  auto result = middleware_->initialize(18080);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(middleware_->is_ready());
}

TEST_F(WebSocketMiddlewareTest, InitializationTwiceFails) {
  auto r1 = middleware_->initialize(18081);
  EXPECT_TRUE(r1.has_value());
  auto r2 = middleware_->initialize(18081);
  EXPECT_FALSE(r2.has_value());
}

TEST_F(WebSocketMiddlewareTest, ShutdownWithoutInit) {
  middleware_->shutdown();
  EXPECT_FALSE(middleware_->is_ready());
}

TEST_F(WebSocketMiddlewareTest, ShutdownAfterInit) {
  auto result = middleware_->initialize(18082);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(middleware_->is_ready());
  middleware_->shutdown();
  EXPECT_FALSE(middleware_->is_ready());
}

TEST_F(WebSocketMiddlewareTest, ReceiveRequestWithoutInit) {
  std::vector<uint8_t> data;
  std::string id;
  EXPECT_FALSE(middleware_->receive_request(data, id));
}

TEST_F(WebSocketMiddlewareTest, PublishDataWithoutInit) {
  std::vector<uint8_t> data = {1, 2, 3};
  EXPECT_FALSE(middleware_->publish_data(data));
}

TEST_F(WebSocketMiddlewareTest, PublishDataNoClients) {
  auto result = middleware_->initialize(18083);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data = {1, 2, 3};
  // No clients connected, should return false
  EXPECT_FALSE(middleware_->publish_data(data));
}

TEST_F(WebSocketMiddlewareTest, ReceiveRequestTimeout) {
  auto result = middleware_->initialize(18084);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data;
  std::string client_id;

  // Should timeout since no client is sending data
  EXPECT_FALSE(middleware_->receive_request(data, client_id));
}

TEST_F(WebSocketMiddlewareTest, SendReplyToUnknownClient) {
  auto result = middleware_->initialize(18085);
  ASSERT_TRUE(result.has_value());

  std::vector<uint8_t> data = {1, 2, 3};
  EXPECT_FALSE(middleware_->send_reply("nonexistent_client", data));
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
