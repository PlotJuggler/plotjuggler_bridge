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

#include <chrono>
#include <future>
#include <string>
#include <vector>

#include "pj_bridge/middleware/outbound_queue.hpp"

using pj_bridge::OutboundQueue;

namespace {

std::vector<std::string> drain(OutboundQueue& queue, size_t count) {
  std::vector<std::string> out;
  for (size_t i = 0; i < count; ++i) {
    auto item = queue.pop();
    if (!item) {
      break;
    }
    out.push_back(item->data);
  }
  return out;
}

}  // namespace

// The asynchronous (Beast) writer polls with try_pop(): it must never block,
// and must return nothing once the queue is closed.
TEST(OutboundQueueTest, TryPopNeverBlocks) {
  OutboundQueue queue(10);
  EXPECT_FALSE(queue.try_pop().has_value());
  queue.push_normal("a");
  auto item = queue.try_pop();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->data, "a");
  EXPECT_FALSE(queue.try_pop().has_value());
  queue.push_normal("b");
  queue.close();
  EXPECT_FALSE(queue.try_pop().has_value());
}

TEST(OutboundQueueTest, KeepsRepliesAndNormalFramesInOrder) {
  OutboundQueue queue(10);
  queue.push_text("reply");
  queue.push_normal("frame");
  auto reply = queue.pop();
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->data, "reply");
  EXPECT_FALSE(reply->binary);
  auto frame = queue.pop();
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->data, "frame");
  EXPECT_TRUE(frame->binary);
}

TEST(OutboundQueueTest, OverflowDropsOldestFrameButNeverReplies) {
  OutboundQueue queue(2);
  queue.push_text("r1");
  queue.push_normal("f1");
  queue.push_normal("f2");
  queue.push_normal("f3");  // drops f1
  queue.push_text("r2");
  EXPECT_EQ(queue.dropped_total(), 1u);
  EXPECT_EQ(drain(queue, 4), (std::vector<std::string>{"r1", "f2", "f3", "r2"}));
}

TEST(OutboundQueueTest, HeavyFramesCoalescePerKeyAndKeepTheNewest) {
  OutboundQueue queue(10);
  queue.push_heavy("/cloud", "cloud1");
  queue.push_heavy("/image", "image1");
  queue.push_heavy("/cloud", "cloud2");  // replaces cloud1 in place: keeps its turn
  EXPECT_EQ(queue.shed_total(), 1u);
  // Re-queued at the back instead, a fast topic could be overtaken forever.
  EXPECT_EQ(drain(queue, 2), (std::vector<std::string>{"cloud2", "image1"}));
}

TEST(OutboundQueueTest, NormalFramesGoBeforeHeavyFrames) {
  OutboundQueue queue(10);
  queue.push_heavy("/cloud", "cloud");
  queue.push_normal("odom");
  queue.push_text("reply");
  EXPECT_EQ(drain(queue, 3), (std::vector<std::string>{"odom", "reply", "cloud"}));
}

TEST(OutboundQueueTest, DropFramesKeepsReplies) {
  OutboundQueue queue(10);
  queue.push_normal("f1");
  queue.push_text("reply");
  queue.push_heavy("/cloud", "cloud");
  queue.drop_frames();
  EXPECT_EQ(queue.dropped_total(), 2u);
  queue.push_normal("f2");
  EXPECT_EQ(drain(queue, 2), (std::vector<std::string>{"reply", "f2"}));
}

TEST(OutboundQueueTest, CloseWakesABlockedPopAndRejectsPushes) {
  OutboundQueue queue(10);
  auto popped = std::async(std::launch::async, [&] { return queue.pop(); });
  EXPECT_EQ(popped.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  queue.close();
  ASSERT_EQ(popped.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_FALSE(popped.get().has_value());
  EXPECT_FALSE(queue.push_text("late"));
  EXPECT_FALSE(queue.push_normal("late"));
  EXPECT_FALSE(queue.push_heavy("/cloud", "late"));
}

TEST(OutboundQueueTest, HeavyFrameIsServedAfterABacklogOfNormalFrames) {
  // Normal traffic that never lets the FIFO lane run empty must not starve
  // the heavy lane: a waiting heavy frame goes out after at most
  // max_normal_frames normal frames.
  OutboundQueue queue(2);
  queue.push_heavy("/cloud", "cloud");
  queue.push_normal("n1");
  queue.push_normal("n2");
  EXPECT_EQ(drain(queue, 2), (std::vector<std::string>{"n1", "n2"}));
  queue.push_normal("n3");
  EXPECT_EQ(drain(queue, 2), (std::vector<std::string>{"cloud", "n3"}));
}

TEST(OutboundQueueTest, ReplyBudgetOverflowClosesTheQueue) {
  // A client that sends requests but never reads their replies must not grow
  // the bridge's memory without bound.
  OutboundQueue queue(10, /*max_reply_bytes=*/8);
  EXPECT_TRUE(queue.push_text("12345"));
  EXPECT_TRUE(queue.push_text("67890")) << "accepted: the waiting replies were under budget";
  EXPECT_FALSE(queue.push_text("x")) << "over the reply budget";
  EXPECT_TRUE(queue.reply_budget_exceeded());
  EXPECT_FALSE(queue.push_normal("frame")) << "the queue is closed";
  EXPECT_FALSE(queue.pop().has_value());
}

TEST(OutboundQueueTest, PoppedRepliesFreeTheirBudget) {
  OutboundQueue queue(10, /*max_reply_bytes=*/8);
  EXPECT_TRUE(queue.push_text("12345"));
  ASSERT_TRUE(queue.pop().has_value());
  EXPECT_TRUE(queue.push_text("67890"));
}

TEST(OutboundQueueTest, HeavyFrameNeverOvertakesAnEarlierReply) {
  // A heavy frame of a just-subscribed topic must not reach the client before
  // the subscribe response that carries its schema.
  OutboundQueue queue(1);
  queue.push_text("heartbeat-reply");
  queue.push_text("subscribe-reply");
  queue.push_heavy("/cloud", "cloud");
  EXPECT_EQ(drain(queue, 3), (std::vector<std::string>{"heartbeat-reply", "subscribe-reply", "cloud"}));
}

TEST(OutboundQueueTest, NormalLaneIsAlsoBoundedByBytes) {
  // The frame limit is sized for many small frames (a slow link must keep
  // seconds of small-topic data while a point cloud is in flight); the byte
  // limit bounds memory when the frames are large.
  OutboundQueue queue(100, OutboundQueue::kDefaultMaxReplyBytes, /*max_normal_bytes=*/10);
  queue.push_normal("aaaa");
  queue.push_normal("bbbb");
  queue.push_normal("cccc");  // 12 bytes: drops "aaaa"
  EXPECT_EQ(queue.dropped_total(), 1u);
  EXPECT_EQ(drain(queue, 2), (std::vector<std::string>{"bbbb", "cccc"}));
}

TEST(OutboundQueueTest, OversizedNormalFrameIsKeptAlone) {
  // A single frame larger than the byte limit still goes out, alone.
  OutboundQueue queue(100, OutboundQueue::kDefaultMaxReplyBytes, /*max_normal_bytes=*/4);
  queue.push_normal("aa");
  queue.push_normal("bbbbbbbb");
  EXPECT_EQ(queue.dropped_total(), 1u);
  EXPECT_EQ(drain(queue, 1), (std::vector<std::string>{"bbbbbbbb"}));
}

TEST(OutboundQueueTest, DroppingFramesRestartsTheHeavyTurnCount) {
  // After a pause or timeout drops the backlog, the count of normal frames
  // served while a heavy frame waited starts over: a heavy frame queued
  // after the drop must not jump ahead of a normal frame queued before it.
  OutboundQueue queue(2);
  queue.push_heavy("/cloud", "old-cloud");
  queue.push_normal("n1");
  queue.push_normal("n2");
  ASSERT_EQ(drain(queue, 2), (std::vector<std::string>{"n1", "n2"}));
  queue.drop_frames();
  queue.push_normal("n3");
  queue.push_heavy("/cloud", "new-cloud");
  EXPECT_EQ(drain(queue, 2), (std::vector<std::string>{"n3", "new-cloud"}));
}

TEST(OutboundQueueTest, OneReplyLargerThanTheBudgetIsStillSent) {
  // The budget bounds replies that pile up unread; a single large reply (a
  // big get_topics with schemas) on an idle queue must not close the client.
  OutboundQueue queue(10, /*max_reply_bytes=*/8);
  EXPECT_TRUE(queue.push_text("0123456789"));
  ASSERT_TRUE(queue.pop().has_value());
  EXPECT_FALSE(queue.reply_budget_exceeded());
}

TEST(OutboundQueueTest, KeptFrameSurvivesOverflow) {
  // A latched-topic replay (/tf_static, /map) is published once: if the
  // backlog dropped it, the client would never get it.
  OutboundQueue queue(2);
  queue.push_text("subscribe-reply");
  queue.push_kept("latched");
  queue.push_normal("n1");
  queue.push_normal("n2");
  queue.push_normal("n3");
  EXPECT_EQ(queue.dropped_total(), 1u);
  EXPECT_EQ(drain(queue, 4), (std::vector<std::string>{"subscribe-reply", "latched", "n2", "n3"}));
}

TEST(OutboundQueueTest, KeptFrameLargerThanTheByteLimitSurvives) {
  OutboundQueue queue(100, OutboundQueue::kDefaultMaxReplyBytes, /*max_normal_bytes=*/4);
  queue.push_kept("large-latched-map");
  queue.push_normal("n1");
  auto first = queue.pop();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->data, "large-latched-map");
  EXPECT_TRUE(first->binary);
}

TEST(OutboundQueueTest, HeavyFrameNeverOvertakesAnEarlierKeptFrame) {
  // A latched replay must arrive before a newer live sample of the topic.
  OutboundQueue queue(1);
  queue.push_heavy("/map", "old");
  queue.push_normal("n1");
  ASSERT_EQ(drain(queue, 1), (std::vector<std::string>{"n1"}));  // the heavy lane is now due a turn
  queue.push_kept("latched-replay");
  queue.push_heavy("/map", "live");  // replaces "old"
  EXPECT_EQ(drain(queue, 2), (std::vector<std::string>{"latched-replay", "live"}));
}

TEST(OutboundQueueTest, DropFramesAlsoDropsKeptFramesAndFreesTheirBudget) {
  OutboundQueue queue(10, /*max_reply_bytes=*/8);
  queue.push_kept("12345678");
  queue.drop_frames();
  EXPECT_EQ(queue.dropped_total(), 1u);
  EXPECT_TRUE(queue.push_text("12345678")) << "the dropped frame's budget was freed";
}
