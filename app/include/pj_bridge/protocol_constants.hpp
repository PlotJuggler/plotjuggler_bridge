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

namespace pj_bridge {

/// Protocol version number, included in all JSON responses
static constexpr int kProtocolVersion = 1;

/// Magic bytes for binary frame header: "PJRB" in little-endian
static constexpr uint32_t kBinaryFrameMagic = 0x42524A50;

/// Size of the binary frame header in bytes
static constexpr size_t kBinaryHeaderSize = 16;

/// Binary frame header flag bit (offset 12 of the 16-byte header) reserved for a
/// future "heavy" (isolated large/size-class message) marker. NOT currently
/// emitted: existing PlotJuggler plugins reject any frame with flags != 0, so
/// heavy frames ship unflagged (flags == 0) and heaviness is conveyed
/// server-side via FramePriority instead. Reserved here for a future
/// capability-negotiated rollout (see docs/API.md).
static constexpr uint32_t kFrameFlagHeavy = 0x1;

/// Default per-message byte threshold at or above which a topic's message is
/// isolated into its own "heavy" size-class frame instead of being aggregated
/// with light topics (see docs/API.md). Chosen comfortably below the 1 MiB
/// socket high-watermark and well above typical scalar/odom/tf frames. A
/// threshold of 0 disables splitting (single aggregated frame, legacy behavior).
static constexpr size_t kDefaultHeavyFrameThresholdBytes = 256 * 1024;  // 256 KiB

/// Schema encoding identifier for ROS2 message definitions
inline constexpr const char* kSchemaEncodingRos2Msg = "ros2msg";

/// Schema encoding identifier for OMG IDL type definitions
inline constexpr const char* kSchemaEncodingOmgIdl = "omgidl";

/// Capability names advertised in the get_topics response's `server` object.
/// Clients feature-detect by NAME (never by comparing version strings): a
/// missing capability degrades that one feature with a warning, while a
/// protocol_version above what the client speaks is the only hard-fail.
inline constexpr const char* kServerCapabilities[] = {
    "include_schemas",       // get_topics/subscribe_topic_updates opt-in schemas
    "latched_badge",         // `latched: true` on transient-local topic entries
    "latched_replay",        // retained samples replayed after subscribe/resume
    "topics_changed",        // pushed topic advertisement (subscribe_topic_updates)
    "per_topic_rate_limit",  // subscribe entries accept {name, max_rate_hz}
    "size_class_frames",     // large topics isolated into own frames (header flag bit0 = heavy)
};

}  // namespace pj_bridge
