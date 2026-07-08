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
#include <functional>
#include <optional>
#include <vector>

#include "pj_bridge/middleware/middleware_interface.hpp"

namespace pj_bridge {

/// Outcome of run_backpressure() for one outgoing frame.
struct SendOutcome {
  bool accepted = false;      ///< value send_binary() should return to its caller
  size_t frames_flushed = 0;  ///< queued frames flushed to the socket this call
  bool shed_heavy = false;    ///< a kHeavy frame was dropped before transmit
  size_t dropped = 0;         ///< frames evicted from the backlog on queue overflow
  bool send_failed = false;   ///< a send() returned false (client gone)
};

/// Socket-agnostic backpressure policy shared by the send path. The caller
/// injects the socket/queue primitives so the policy is unit-testable without a
/// real connection:
///   - @p buffered_amount : current outgoing socket buffer size, in bytes
///   - @p pop_pending     : remove + return the oldest queued frame (nullopt if empty)
///   - @p send            : transmit a frame; returns success (false = client gone)
///   - @p queue_pending   : enqueue a frame with drop-oldest overflow; returns the
///                          number dropped, or nullopt if the client is gone
///
/// Policy: first flush the backlog while the socket has room (re-checking the
/// watermark each iteration so an already-congested socket is never fed
/// further, and never flushing more than @p max_flush frames so a concurrent
/// producer cannot make one call flush forever); then handle the current
/// frame — send it if there is room, else drop a kHeavy frame before transmit
/// (shed) or enqueue a kNormal frame.
///
/// @param max_flush upper bound on frames flushed this call — pass the backlog
///        size observed at call start.
inline SendOutcome run_backpressure(
    FramePriority priority, const std::vector<uint8_t>& frame, size_t watermark, size_t max_flush,
    const std::function<size_t()>& buffered_amount,
    const std::function<std::optional<std::vector<uint8_t>>()>& pop_pending,
    const std::function<bool(const std::vector<uint8_t>&)>& send,
    const std::function<std::optional<size_t>(const std::vector<uint8_t>&)>& queue_pending) {
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
      out.send_failed = true;
      out.accepted = false;
      return out;
    }
    out.frames_flushed++;
  }

  // Handle the current frame against the live socket buffer.
  if (buffered_amount() < watermark) {
    out.accepted = send(frame);
    out.send_failed = !out.accepted;
    return out;
  }

  // Socket congested: shed heavy frames before transmit; queue normal frames.
  if (priority == FramePriority::kHeavy) {
    out.shed_heavy = true;
    out.accepted = true;  // accepted-for-later semantics, same as a queued normal frame
    return out;
  }

  std::optional<size_t> dropped = queue_pending(frame);
  if (!dropped) {
    out.accepted = false;
    out.send_failed = true;
    return out;
  }
  out.dropped = *dropped;
  out.accepted = true;
  return out;
}

}  // namespace pj_bridge
