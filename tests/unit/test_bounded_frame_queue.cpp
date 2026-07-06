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

#include "pj_bridge/middleware/bounded_frame_queue.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace pj_bridge;

namespace {
std::vector<uint8_t> make_frame(uint8_t value) {
  return {value};
}
}  // namespace

TEST(BoundedFrameQueueTest, PushUnderCapacityReturnsZeroAndGrows) {
  BoundedFrameQueue queue(2);

  EXPECT_EQ(queue.push(make_frame(1)), 0u);
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_FALSE(queue.empty());

  EXPECT_EQ(queue.push(make_frame(2)), 0u);
  EXPECT_EQ(queue.size(), 2u);
}

TEST(BoundedFrameQueueTest, PushPastCapacityDropsOldest) {
  BoundedFrameQueue queue(2);

  EXPECT_EQ(queue.push(make_frame(1)), 0u);
  EXPECT_EQ(queue.push(make_frame(2)), 0u);
  // Queue is now {1, 2}; pushing {3} should drop the OLDEST frame (1), not
  // the newest, leaving {2, 3}.
  EXPECT_EQ(queue.push(make_frame(3)), 1u);
  EXPECT_EQ(queue.size(), 2u);

  auto first = queue.pop_front();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, make_frame(2));

  auto second = queue.pop_front();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, make_frame(3));

  EXPECT_TRUE(queue.empty());
}

TEST(BoundedFrameQueueTest, PopFrontOnEmptyReturnsNullopt) {
  BoundedFrameQueue queue(2);
  EXPECT_EQ(queue.pop_front(), std::nullopt);
}

TEST(BoundedFrameQueueTest, DroppedTotalAccumulatesAcrossPushes) {
  BoundedFrameQueue queue(1);

  EXPECT_EQ(queue.dropped_total(), 0u);
  EXPECT_EQ(queue.push(make_frame(1)), 0u);
  EXPECT_EQ(queue.dropped_total(), 0u);

  EXPECT_EQ(queue.push(make_frame(2)), 1u);
  EXPECT_EQ(queue.dropped_total(), 1u);

  EXPECT_EQ(queue.push(make_frame(3)), 1u);
  EXPECT_EQ(queue.dropped_total(), 2u);
}

TEST(BoundedFrameQueueTest, ZeroCapacityDropsEveryPushAndStaysEmpty) {
  BoundedFrameQueue queue(0);

  EXPECT_EQ(queue.push(make_frame(1)), 1u);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.dropped_total(), 1u);

  EXPECT_EQ(queue.push(make_frame(2)), 1u);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.dropped_total(), 2u);

  EXPECT_EQ(queue.pop_front(), std::nullopt);
}
