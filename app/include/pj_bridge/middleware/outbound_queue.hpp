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

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace pj_bridge {

/// Outbound queue of one client, filled by the bridge and drained by that
/// client's sender thread. Pushing never blocks, so a slow client can only
/// fall behind; it can never stall the bridge or the other clients.
///
/// Policy, chosen for live plotting over a slow link (fresh data beats complete data):
///   - Text replies and normal binary frames share one FIFO lane, so a reply
///     still precedes the frames sent after it (e.g. latched replay). Replies
///     are never dropped; when more than `max_normal_frames` binary frames are
///     queued, the oldest binary frame is dropped.
///   - Heavy frames are coalesced by key (the topic): a new heavy frame
///     replaces a queued one with the same key, so a slow client receives the
///     newest point cloud instead of a growing backlog of old ones.
///   - pop() serves the FIFO lane first, then the oldest heavy frame, so small
///     topics are not stuck behind large ones.
///
/// Thread-safe: every method may be called from any thread.
class OutboundQueue {
 public:
  struct Item {
    std::string data;
    bool binary = true;
  };

  explicit OutboundQueue(size_t max_normal_frames) : max_normal_frames_(max_normal_frames) {}

  /// @return false if the queue is closed.
  bool push_text(std::string data) {
    return push_fifo(Item{std::move(data), false});
  }

  /// @return false if the queue is closed.
  bool push_normal(std::string frame) {
    return push_fifo(Item{std::move(frame), true});
  }

  /// @return false if the queue is closed.
  bool push_heavy(const std::string& key, std::string frame) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      auto same_key = std::find_if(heavy_.begin(), heavy_.end(), [&](const auto& entry) { return entry.first == key; });
      if (same_key != heavy_.end()) {
        heavy_.erase(same_key);
        ++shed_total_;
      }
      heavy_.emplace_back(key, std::move(frame));
    }
    cv_.notify_one();
    return true;
  }

  /// Block until an item is available; nullopt once the queue is closed.
  std::optional<Item> pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return closed_ || !fifo_.empty() || !heavy_.empty(); });
    if (closed_) {
      return std::nullopt;
    }
    Item item;
    if (!fifo_.empty()) {
      item = std::move(fifo_.front());
      fifo_.pop_front();
      if (item.binary) {
        --normal_frames_;
      }
    } else {
      item.data = std::move(heavy_.front().second);
      heavy_.pop_front();
    }
    return item;
  }

  /// Drop every queued binary frame, keeping text replies (the client's
  /// session ended but its socket stays open).
  void drop_frames() {
    std::lock_guard<std::mutex> lock(mutex_);
    dropped_total_ += normal_frames_ + heavy_.size();
    fifo_.erase(std::remove_if(fifo_.begin(), fifo_.end(), [](const Item& item) { return item.binary; }), fifo_.end());
    normal_frames_ = 0;
    heavy_.clear();
  }

  /// Discard everything and wake the sender; later pushes fail.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      fifo_.clear();
      heavy_.clear();
      normal_frames_ = 0;
    }
    cv_.notify_all();
  }

  /// Normal frames dropped on overflow, plus frames discarded by drop_frames().
  uint64_t dropped_total() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_total_;
  }

  /// Heavy frames replaced by a newer frame with the same key.
  uint64_t shed_total() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shed_total_;
  }

 private:
  bool push_fifo(Item item) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      if (item.binary) {
        if (normal_frames_ >= max_normal_frames_) {
          auto oldest = std::find_if(fifo_.begin(), fifo_.end(), [](const Item& queued) { return queued.binary; });
          if (oldest != fifo_.end()) {
            fifo_.erase(oldest);
            --normal_frames_;
          }
          ++dropped_total_;
        }
        if (max_normal_frames_ == 0) {
          return true;  // nothing may be queued: the new frame is the one dropped
        }
        ++normal_frames_;
      }
      fifo_.push_back(std::move(item));
    }
    cv_.notify_one();
    return true;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> fifo_;
  std::deque<std::pair<std::string, std::string>> heavy_;  // (key, frame), oldest first
  size_t normal_frames_ = 0;
  size_t max_normal_frames_;
  bool closed_ = false;
  uint64_t dropped_total_ = 0;
  uint64_t shed_total_ = 0;
};

}  // namespace pj_bridge
