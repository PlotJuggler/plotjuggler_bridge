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

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace pj_bridge {

/// A FIFO queue of binary frames bounded to at most `max_frames` entries.
///
/// When a push would exceed the capacity, the OLDEST queued frame is dropped
/// to make room for the new one (never the newest), matching foxglove_bridge's
/// slow-client backpressure policy: recent data is prioritized over stale
/// backlog for interactive plotting/visualization use cases.
///
/// NOT thread-safe by design: this is a plain data structure. Callers are
/// expected to hold their own lock (e.g. WebSocketMiddleware's
/// `clients_mutex_`) around every call.
class BoundedFrameQueue {
 public:
  explicit BoundedFrameQueue(size_t max_frames) : max_frames_(max_frames) {}

  /// Pushes a frame, dropping the oldest queued frame first if the queue is
  /// already at capacity. Returns the number of frames dropped as a result of
  /// this call (0 or 1). A queue with `max_frames == 0` drops every push
  /// immediately and never grows.
  size_t push(std::vector<uint8_t> frame) {
    if (max_frames_ == 0) {
      ++dropped_total_;
      return 1;
    }

    size_t dropped = 0;
    if (frames_.size() >= max_frames_) {
      frames_.pop_front();
      ++dropped_total_;
      dropped = 1;
    }
    frames_.push_back(std::move(frame));
    return dropped;
  }

  /// Removes and returns the oldest frame, or nullopt if the queue is empty.
  std::optional<std::vector<uint8_t>> pop_front() {
    if (frames_.empty()) {
      return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> front = std::move(frames_.front());
    frames_.pop_front();
    return front;
  }

  bool empty() const {
    return frames_.empty();
  }

  size_t size() const {
    return frames_.size();
  }

  /// Cumulative number of frames dropped over the lifetime of this queue.
  uint64_t dropped_total() const {
    return dropped_total_;
  }

 private:
  size_t max_frames_;
  std::deque<std::vector<uint8_t>> frames_;
  uint64_t dropped_total_{0};
};

}  // namespace pj_bridge
