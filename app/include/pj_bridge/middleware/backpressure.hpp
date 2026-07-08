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
#include <optional>
#include <vector>

#include "pj_bridge/middleware/middleware_interface.hpp"

namespace pj_bridge {

/// Outcome of run_backpressure() for one outgoing frame. The disposition is a
/// single discriminant (no contradictory flag combinations are representable);
/// the counters are companion data.
struct SendOutcome {
  SendResult result = SendResult::kClientGone;  ///< how the current frame was handled
  size_t frames_flushed = 0;                    ///< backlog frames flushed to the socket this call
  size_t dropped = 0;                           ///< backlog frames evicted on overflow (only when result == kQueued)
};

/// Socket-agnostic backpressure policy shared by the send path. The caller
/// injects the socket/queue primitives (as callables — templated to avoid
/// std::function type-erasure on the hot path) so the policy is unit-testable
/// without a real connection:
///   - @p buffered_amount : `size_t()`                    — current socket buffer bytes
///   - @p pop_pending     : `std::optional<vector<uint8_t>>()` — pop oldest queued frame (nullopt if empty)
///   - @p send            : `bool(const vector<uint8_t>&)` — transmit a frame; false = client gone
///   - @p queue_pending   : `std::optional<size_t>(const vector<uint8_t>&)` — enqueue (drop-oldest),
///                          returns #dropped, or nullopt if the client is gone
///
/// Policy: first flush the backlog while the socket has room (re-checking the
/// watermark each iteration so an already-congested socket is never fed
/// further, and never flushing more than @p max_flush frames so a concurrent
/// producer cannot make one call flush forever); then handle the current
/// frame — send it if there is room (kDelivered), else drop a kHeavy frame
/// before transmit (kShed) or enqueue a kNormal frame (kQueued). A vanished
/// client surfaces as kClientGone.
///
/// @param max_flush upper bound on frames flushed this call — pass the backlog
///        size observed at call start.
template <class BufferedAmount, class PopPending, class Send, class QueuePending>
SendOutcome run_backpressure(
    FramePriority priority, const std::vector<uint8_t>& frame, size_t watermark, size_t max_flush,
    const BufferedAmount& buffered_amount, const PopPending& pop_pending, const Send& send,
    const QueuePending& queue_pending) {
  SendOutcome out;

  // Flush queued frames to the socket, re-checking the watermark each iteration
  // so a socket that fills up mid-flush is never fed further (the flush-recheck
  // fix: the old loop computed the flush count once and could dump a burst of
  // stale frames onto an already-congested socket). Bounded by max_flush so a
  // producer enqueueing concurrently cannot keep this single call flushing.
  while (out.frames_flushed < max_flush && buffered_amount() < watermark) {
    std::optional<std::vector<uint8_t>> queued = pop_pending();
    if (!queued) {
      break;
    }
    if (!send(*queued)) {
      out.result = SendResult::kClientGone;
      return out;
    }
    out.frames_flushed++;
  }

  // Handle the current frame against the live socket buffer.
  if (buffered_amount() < watermark) {
    out.result = send(frame) ? SendResult::kDelivered : SendResult::kClientGone;
    return out;
  }

  // Socket congested: shed heavy frames before transmit; queue normal frames.
  if (priority == FramePriority::kHeavy) {
    out.result = SendResult::kShed;
    return out;
  }

  std::optional<size_t> dropped = queue_pending(frame);
  if (!dropped) {
    out.result = SendResult::kClientGone;
    return out;
  }
  out.dropped = *dropped;
  out.result = SendResult::kQueued;
  return out;
}

}  // namespace pj_bridge
