# API v2 Improvements Design

**Date:** 2026-02-03
**Branch:** `feature/api-v2-improvements`
**Status:** Approved

## Overview

Breaking API changes to improve protocol robustness and client usability:
- Request IDs for async correlation
- Protocol version in all responses
- Per-topic schema encoding field
- Explicit unsubscribe command (additive subscription model)
- Binary frame header with magic, count, size, flags
- Pause/resume commands with smart ROS2 subscription management

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Request ID format | Opaque string | Maximum flexibility, no validation overhead |
| Protocol version exposure | Every response | No extra roundtrip, always visible |
| Schema encoding | Per-topic object | Allows mixed encodings in future |
| Subscribe model | Additive (breaking) | Clearer semantics, no ambiguity |
| Binary header size | 16 bytes fixed | Aligned, includes magic for validation |
| Pause scope | Per-client | Fits session model, simpler |
| ROS2 sub on pause | Smart management | Unsubscribe when no active clients need topic |

## Request IDs & Protocol Version

All requests may include optional `"id": "<string>"`. All responses:
- Echo `id` verbatim if present in request
- Include `"protocol_version": 1` always

```json
// Request
{"command": "heartbeat", "id": "req-42"}

// Response
{"status": "ok", "id": "req-42", "protocol_version": 1}
```

## Schema Encoding

Subscribe response schema format changes from string to object:

```json
{
  "status": "success",
  "id": "sub-1",
  "protocol_version": 1,
  "schemas": {
    "/imu": {"encoding": "ros2msg", "definition": "std_msgs/Header header\n..."},
    "/odom": {"encoding": "ros2msg", "definition": "..."}
  },
  "rate_limits": {"/imu": 10.0}
}
```

## Subscribe & Unsubscribe Commands

**Breaking change:** Additive model replaces "set" model.

- `subscribe` only **adds** topics (ignores already-subscribed)
- `unsubscribe` only **removes** topics
- To change rate: unsubscribe then resubscribe

**Subscribe:**
```json
{"command": "subscribe", "id": "s1", "topics": ["/a", {"name": "/b", "max_rate_hz": 10.0}]}
```

**Unsubscribe:**
```json
{"command": "unsubscribe", "id": "u1", "topics": ["/a", "/b"]}
```

**Unsubscribe response:**
```json
{
  "status": "success",
  "id": "u1",
  "protocol_version": 1,
  "removed": ["/a", "/b"]
}
```

## Pause & Resume Commands

Per-client pause stops binary frame delivery. Subscriptions and rates preserved.

**Smart ROS2 management:**
- Track "active interest" = subscribed AND not paused
- When active interest drops to 0, unsubscribe from ROS2
- On resume, resubscribe to ROS2
- Buffered messages discarded on pause (fresh data on resume)

**Pause:**
```json
{"command": "pause", "id": "p1"}
// Response
{"status": "ok", "id": "p1", "protocol_version": 1, "paused": true}
```

**Resume:**
```json
{"command": "resume", "id": "r1"}
// Response
{"status": "ok", "id": "r1", "protocol_version": 1, "paused": false}
```

## Binary Frame Header

Fixed 16-byte header (little-endian), outside compression:

| Offset | Size | Field | Value/Description |
|--------|------|-------|-------------------|
| 0 | 4 | `magic` | `0x504A5242` ("PJRB") |
| 4 | 4 | `message_count` | Number of messages |
| 8 | 4 | `uncompressed_size` | Payload size before ZSTD |
| 12 | 4 | `flags` | Reserved, must be 0 |

```
[16-byte header][ZSTD-compressed payload]
```

Benefits:
- Magic validates frame integrity
- Count/size readable before decompression
- Flags reserved for future (compression algorithm, etc.)

## Files Modified

| File | Changes |
|------|---------|
| `session_manager.hpp` | Add `bool paused` to `Session` |
| `session_manager.cpp` | Init paused, add `set_paused()` / `is_paused()` |
| `bridge_server.hpp` | Declare `handle_unsubscribe`, `handle_pause`, `handle_resume` |
| `bridge_server.cpp` | New handlers, additive subscribe, response injection, skip paused clients |
| `message_serializer.hpp` | Add `kMagic`, `kHeaderSize` constants |
| `message_serializer.cpp` | Prepend 16-byte header |
| `generic_subscription_manager.cpp` | Pause-aware ref counting |
| `docs/API.md` | Full documentation update |
| `tests/unit/test_*.cpp` | New response format, new command tests |
| `tests/integration/test_client.py` | Parse new binary header |

## Implementation Order

1. **Request IDs + protocol version** — Touches all responses
2. **Schema encoding change** — Subscribe response only
3. **Binary frame header** — Serializer + test client
4. **Unsubscribe command** — New handler + additive model
5. **Pause/resume commands** — Session state + smart ROS2 management

## Verification

```bash
# Build
cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && \
  colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release

# Test
colcon test --packages-select pj_ros_bridge && colcon test-result --verbose

# Format
cd ~/ws_plotjuggler/src/pj_ros_bridge && pre-commit run -a
```
