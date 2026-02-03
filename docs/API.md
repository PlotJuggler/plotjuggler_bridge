# API Protocol

`pj_ros_bridge` uses a single WebSocket port (default 8080). Text frames carry JSON API requests/responses; binary frames carry ZSTD-compressed aggregated message data.

## Request IDs and Protocol Version

All requests may include an optional `"id"` field (string). All responses include:
- `"protocol_version": 1` - Always present
- `"id"` - Echoed if provided in request

Example:
```json
// Request
{"command": "heartbeat", "id": "req-42"}

// Response
{"status": "ok", "id": "req-42", "protocol_version": 1}
```

## Communication Overview

The diagram below shows the typical client-server interaction:

1. **Connection**: Client connects via WebSocket
2. **Discovery**: Client queries available topics
3. **Subscription**: Client subscribes to topics of interest, receives schemas
4. **Data streaming**: Server pushes aggregated binary data at 50 Hz
5. **Heartbeat**: Client sends periodic heartbeats to maintain the session

```mermaid
sequenceDiagram
    participant C as Client
    participant S as Server
    participant R as ROS2

    C->>S: WebSocket Connect
    activate S
    S-->>C: Connection Established

    Note over C,S: Discovery Phase (Text Frames)
    C->>S: {"command": "get_topics"}
    S->>R: Query topic graph
    R-->>S: Topic list
    S-->>C: {"status": "success", "topics": [...], "protocol_version": 1}

    Note over C,S: Subscription Phase (Text Frames)
    C->>S: {"command": "subscribe", "topics": [...]}
    S->>R: Create GenericSubscription
    S-->>C: {"status": "success", "schemas": {...}, "protocol_version": 1}

    Note over C,S: Data Streaming (Binary Frames)
    loop Every 20ms (50 Hz)
        R-->>S: New messages arrive
        S->>S: Buffer & aggregate
        S-->>C: ZSTD-compressed binary frame
    end

    Note over C,S: Session Maintenance
    loop Every ~1 second
        C->>S: {"command": "heartbeat"}
        S-->>C: {"status": "ok", "protocol_version": 1}
    end

    Note over C,S: Timeout after 10s without heartbeat
    C--xS: Disconnect / Timeout
    S->>R: Cleanup subscriptions
    deactivate S
```

## Get Topics

Discover available ROS2 topics.

**Request:**
```json
{"command": "get_topics", "id": "gt1"}
```

**Response:**
```json
{
  "status": "success",
  "id": "gt1",
  "protocol_version": 1,
  "topics": [
    {"name": "/topic_name", "type": "package_name/msg/MessageType"}
  ]
}
```

## Subscribe

Subscribe to one or more topics. **Breaking change:** Subscribe now uses an additive model - it only adds topics without removing existing subscriptions. Use the `unsubscribe` command to remove topics.

Each topic in the array can be either a plain string (unlimited rate) or an object with a `max_rate_hz` field for per-topic rate limiting. Both formats can be mixed in the same request.

When `max_rate_hz` is set, the server decimates messages for that topic, sending at most one message per rate interval (the latest available). A value of `0` or omitting the field means unlimited (all messages forwarded).

**Request (string-only, backward compatible):**
```json
{
  "command": "subscribe",
  "id": "s1",
  "topics": ["/topic1", "/topic2"]
}
```

**Request (mixed format with rate limiting):**
```json
{
  "command": "subscribe",
  "id": "s2",
  "topics": [
    "/topic_unlimited",
    {"name": "/topic_limited", "max_rate_hz": 10.0}
  ]
}
```

**Response (success):**
```json
{
  "status": "success",
  "id": "s1",
  "protocol_version": 1,
  "schemas": {
    "/topic_unlimited": {"encoding": "ros2msg", "definition": "message definition text"},
    "/topic_limited": {"encoding": "ros2msg", "definition": "message definition text"}
  },
  "rate_limits": {
    "/topic_limited": 10.0
  }
}
```

The `rate_limits` field is only present when at least one topic has a non-zero rate limit. It maps topic names to their configured `max_rate_hz`.

**Response (partial success):**
```json
{
  "status": "partial_success",
  "id": "s1",
  "protocol_version": 1,
  "message": "Some subscriptions failed",
  "schemas": {"/topic1": {"encoding": "ros2msg", "definition": "..."}},
  "failures": [
    {"topic": "/topic2", "reason": "Topic does not exist"}
  ]
}
```

## Unsubscribe

Remove topics from subscription. Only removes specified topics; other subscriptions are preserved.

**Request:**
```json
{"command": "unsubscribe", "id": "u1", "topics": ["/topic1", "/topic2"]}
```

**Response:**
```json
{
  "status": "success",
  "id": "u1",
  "protocol_version": 1,
  "removed": ["/topic1", "/topic2"]
}
```

Topics not currently subscribed are silently ignored.

## Pause / Resume

Pause stops binary frame delivery to the client. Subscriptions and rate limits are preserved.
Resume restarts binary frame delivery.

**Pause Request:**
```json
{"command": "pause", "id": "p1"}
```

**Pause Response:**
```json
{"status": "ok", "id": "p1", "protocol_version": 1, "paused": true}
```

**Resume Request:**
```json
{"command": "resume", "id": "r1"}
```

**Resume Response:**
```json
{"status": "ok", "id": "r1", "protocol_version": 1, "paused": false}
```

Both commands are idempotent. Smart ROS2 management: when all clients interested in a topic are paused, the ROS2 subscription is released.

## Heartbeat

Clients must send a heartbeat at least once per second. The default timeout is 10 seconds.

**Request:**
```json
{"command": "heartbeat", "id": "hb1"}
```

**Response:**
```json
{"status": "ok", "id": "hb1", "protocol_version": 1}
```

## Error Response

All commands may return an error:

```json
{
  "status": "error",
  "id": "req-id",
  "protocol_version": 1,
  "error_code": "ERROR_CODE",
  "message": "Human readable error message"
}
```

Error codes: `INVALID_REQUEST`, `INVALID_JSON`, `UNKNOWN_COMMAND`, `ALL_SUBSCRIPTIONS_FAILED`, `INTERNAL_ERROR`.

## Binary Message Format

Binary frames consist of a fixed 16-byte header followed by ZSTD-compressed payload.

### Header (16 bytes, little-endian, uncompressed)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | `0x42524A50` ("PJRB") |
| 4 | 4 | message_count | Number of messages in frame |
| 8 | 4 | uncompressed_size | Payload size before compression |
| 12 | 4 | flags | Reserved (must be 0) |

### Payload (ZSTD-compressed)

The compressed payload contains messages in sequence:

```
For each message:
  - Topic name length  (uint16_t, little-endian)
  - Topic name         (N bytes, UTF-8)
  - Timestamp          (uint64_t, nanoseconds since epoch, little-endian)
  - Message data length (uint32_t, little-endian)
  - Message data       (N bytes, CDR-serialized from ROS2)
```

The magic bytes allow clients to validate frame integrity before decompression.
