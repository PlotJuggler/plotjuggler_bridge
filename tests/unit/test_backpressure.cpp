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

#include <deque>
#include <optional>
#include <vector>

#include "pj_bridge/middleware/backpressure.hpp"

using namespace pj_bridge;

namespace {
std::vector<uint8_t> frame_of(size_t n) {
  return std::vector<uint8_t>(n, 0x7F);
}
}  // namespace

// Drives run_backpressure() against an in-memory fake socket whose buffered
// amount grows by each frame's size as it is "sent" (sockets drain over time,
// but within one synchronous call the buffer only grows). No real connection.
class BackpressureTest : public ::testing::Test {
 protected:
  size_t watermark_ = 1000;
  size_t buffered_ = 0;
  std::deque<std::vector<uint8_t>> backlog_;
  std::vector<std::vector<uint8_t>> sent_;
  size_t backlog_cap_ = 100;

  std::function<size_t()> buffered_amount() {
    return [this] { return buffered_; };
  }
  std::function<std::optional<std::vector<uint8_t>>()> pop_pending() {
    return [this]() -> std::optional<std::vector<uint8_t>> {
      if (backlog_.empty()) {
        return std::nullopt;
      }
      std::vector<uint8_t> f = std::move(backlog_.front());
      backlog_.pop_front();
      return f;
    };
  }
  std::function<bool(const std::vector<uint8_t>&)> send() {
    return [this](const std::vector<uint8_t>& f) {
      sent_.push_back(f);
      buffered_ += f.size();
      return true;
    };
  }
  std::function<std::optional<size_t>(const std::vector<uint8_t>&)> queue_pending() {
    return [this](const std::vector<uint8_t>& f) -> std::optional<size_t> {
      size_t dropped = 0;
      if (backlog_.size() >= backlog_cap_) {
        backlog_.pop_front();
        dropped = 1;
      }
      backlog_.push_back(f);
      return dropped;
    };
  }
  SendOutcome run(FramePriority prio, const std::vector<uint8_t>& frame) {
    return run_backpressure(
        prio, frame, watermark_, backlog_.size(), buffered_amount(), pop_pending(), send(), queue_pending());
  }
};

TEST_F(BackpressureTest, SendsImmediatelyWhenSocketHasRoom) {
  auto out = run(FramePriority::kNormal, frame_of(100));
  EXPECT_EQ(out.result, SendResult::kDelivered);
  EXPECT_EQ(out.frames_flushed, 0u);
  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(sent_[0].size(), 100u);
}

TEST_F(BackpressureTest, FlushStopsWhenWatermarkReachedMidFlush) {
  // Five queued 600-byte frames, watermark 1000. Each send adds 600 to the
  // socket buffer, so only two may flush before the socket is congested again.
  for (int i = 0; i < 5; ++i) {
    backlog_.push_back(frame_of(600));
  }
  auto out = run(FramePriority::kNormal, frame_of(600));
  EXPECT_EQ(out.frames_flushed, 2u) << "flush must recheck the watermark and stop early";
  EXPECT_EQ(out.result, SendResult::kQueued);
  EXPECT_EQ(sent_.size(), 2u);
  // Three unflushed backlog frames remain, plus the current frame gets queued.
  EXPECT_EQ(backlog_.size(), 4u);
}

TEST_F(BackpressureTest, HeavyFrameShedWhenCongested) {
  buffered_ = watermark_;  // already congested
  auto out = run(FramePriority::kHeavy, frame_of(5000));
  EXPECT_EQ(out.result, SendResult::kShed);
  EXPECT_TRUE(sent_.empty());
  EXPECT_TRUE(backlog_.empty()) << "a heavy frame must be shed, never queued";
}

TEST_F(BackpressureTest, NormalFrameQueuedWhenCongested) {
  buffered_ = watermark_;  // congested
  auto out = run(FramePriority::kNormal, frame_of(100));
  EXPECT_EQ(out.result, SendResult::kQueued);
  EXPECT_TRUE(sent_.empty());
  ASSERT_EQ(backlog_.size(), 1u);
}

TEST_F(BackpressureTest, SendFailureDuringFlushIsNotAccepted) {
  backlog_.push_back(frame_of(100));
  auto out = run_backpressure(
      FramePriority::kNormal, frame_of(100), watermark_, /*max_flush=*/1, buffered_amount(), pop_pending(),
      [](const std::vector<uint8_t>&) { return false; },  // send fails (client gone)
      queue_pending());
  EXPECT_EQ(out.result, SendResult::kClientGone);
}

TEST_F(BackpressureTest, FlushDrainsFullyThenSendsCurrentWhenRoom) {
  // Backlog fits comfortably under the watermark: all of it flushes, then the
  // current frame is sent too.
  for (int i = 0; i < 3; ++i) {
    backlog_.push_back(frame_of(10));
  }
  auto out = run(FramePriority::kNormal, frame_of(10));
  EXPECT_EQ(out.result, SendResult::kDelivered);
  EXPECT_EQ(out.frames_flushed, 3u);
  EXPECT_TRUE(backlog_.empty());
  EXPECT_EQ(sent_.size(), 4u);  // 3 flushed + current
}

TEST_F(BackpressureTest, HeavyFrameSentWhenRoomAvailable) {
  // A heavy frame is only shed under congestion; with room it is sent normally.
  auto out = run(FramePriority::kHeavy, frame_of(5000));
  EXPECT_EQ(out.result, SendResult::kDelivered);
  ASSERT_EQ(sent_.size(), 1u);
  EXPECT_EQ(sent_[0].size(), 5000u);
}

TEST_F(BackpressureTest, QueueClientGoneReturnsNotAccepted) {
  // queue_pending() reporting the client vanished (nullopt) must surface as a
  // failed send (matches the middleware disconnect-race handling).
  buffered_ = watermark_;  // congested -> normal frame takes the queue path
  auto out = run_backpressure(
      FramePriority::kNormal, frame_of(100), watermark_, /*max_flush=*/0, buffered_amount(), pop_pending(), send(),
      [](const std::vector<uint8_t>&) -> std::optional<size_t> { return std::nullopt; });
  EXPECT_EQ(out.result, SendResult::kClientGone);
}

TEST_F(BackpressureTest, FlushBoundedByMaxFlush) {
  // Even with room for all, at most max_flush queued frames flush per call, so a
  // concurrent producer cannot make one call flush forever.
  for (int i = 0; i < 5; ++i) {
    backlog_.push_back(frame_of(10));
  }
  auto out = run_backpressure(
      FramePriority::kNormal, frame_of(10), watermark_, /*max_flush=*/3, buffered_amount(), pop_pending(), send(),
      queue_pending());
  EXPECT_EQ(out.frames_flushed, 3u);
  EXPECT_EQ(backlog_.size(), 2u);
}

TEST_F(BackpressureTest, FlushTerminatesDespiteConcurrentEnqueue) {
  // A producer that enqueues a fresh frame on every pop (socket has endless
  // room). Without the max_flush bound this would flush forever; with it,
  // exactly max_flush frames flush and the call terminates. This is the
  // concurrency property max_flush exists to guarantee.
  backlog_.push_back(frame_of(1));
  auto refilling_pop = [this]() -> std::optional<std::vector<uint8_t>> {
    if (backlog_.empty()) {
      return std::nullopt;
    }
    std::vector<uint8_t> f = std::move(backlog_.front());
    backlog_.pop_front();
    backlog_.push_back(frame_of(1));  // producer keeps the backlog non-empty
    return f;
  };
  auto out = run_backpressure(
      FramePriority::kNormal, frame_of(1), watermark_, /*max_flush=*/3, buffered_amount(), refilling_pop, send(),
      queue_pending());
  EXPECT_EQ(out.frames_flushed, 3u) << "max_flush must bound the flush despite continual refills";
}
