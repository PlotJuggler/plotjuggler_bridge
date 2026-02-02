# pj_ros_bridge API Protocol Specification

This document describes the client-server protocol for `pj_ros_bridge`. It is intended for developers implementing clients in any programming language.

## Table of Contents

- [Overview](#overview)
- [Connection Setup](#connection-setup)
- [API Commands](#api-commands)
  - [get_topics](#get_topics)
  - [subscribe](#subscribe)
  - [heartbeat](#heartbeat)
- [Data Streaming](#data-streaming)
- [Binary Serialization Format](#binary-serialization-format)
- [Error Handling](#error-handling)
- [Session Management](#session-management)

---

## Overview

The `pj_ros_bridge` server uses a single **WebSocket** connection per client:

- **Text frames**: JSON API commands and responses
- **Binary frames**: ZSTD-compressed aggregated message data at 50 Hz

### Network Port

- **WebSocket Port**: 8080 (default, configurable via ROS2 `port` parameter)

### Data Formats

- **API Messages**: JSON (UTF-8 text frames)
- **Data Stream**: Custom binary format with ZSTD compression (binary frames)

---

## Connection Setup

### Client Connection Sequence

1. **Connect via WebSocket** (`ws://server:8080`)
2. **Discover topics** using `get_topics` command (text frame)
3. **Subscribe to topics** using `subscribe` command (text frame)
4. **Start heartbeat** sending heartbeat every 1 second (text frame)
5. **Receive data** as binary frames pushed by the server

### WebSocket Configuration

- **URL**: `ws://server:8080` (default port)
- **Protocol**: Standard WebSocket (RFC 6455)
- **Text frames**: JSON API commands and responses
- **Binary frames**: ZSTD-compressed aggregated message data

---

## API Commands

All API commands are sent as JSON text frames. The server responds with a JSON text frame.

### get_topics

Discover all available ROS2 topics.

#### Request Format

```json
{
  "command": "get_topics"
}
```

#### Response Format (Success)

```json
{
  "status": "success",
  "topics": [
    {
      "name": "/camera/image",
      "type": "sensor_msgs/msg/Image"
    },
    {
      "name": "/imu/data",
      "type": "sensor_msgs/msg/Imu"
    }
  ]
}
```

#### Response Fields

- `status` (string): Always `"success"` for this command
- `topics` (array): List of available topics
  - `name` (string): Topic name (with leading `/`)
  - `type` (string): ROS2 message type (format: `package_name/msg/MessageType`)

---

### subscribe

Subscribe to one or more topics. The server will provide message schemas for all successfully subscribed topics.

#### Request Format

```json
{
  "command": "subscribe",
  "topics": ["/camera/image", "/imu/data"]
}
```

#### Request Fields

- `command` (string): Must be `"subscribe"`
- `topics` (array of strings): List of topic names to subscribe to

**Note**: The topic list is **absolute**, not incremental. If you want to unsubscribe from a topic, simply send a new subscribe request without that topic.

#### Response Format (Full Success)

```json
{
  "status": "success",
  "schemas": {
    "/camera/image": "uint32 height\nuint32 width\n...",
    "/imu/data": "std_msgs/Header header\n...\n================================================================================\nMSG: std_msgs/Header\n..."
  }
}
```

#### Response Format (Partial Success)

```json
{
  "status": "partial_success",
  "message": "Some subscriptions failed",
  "schemas": {
    "/camera/image": "uint32 height\nuint32 width\n..."
  },
  "failures": [
    {
      "topic": "/invalid_topic",
      "reason": "Topic does not exist"
    },
    {
      "topic": "/unknown_type",
      "reason": "Schema extraction failed: Package not found"
    }
  ]
}
```

#### Response Format (All Failed)

```json
{
  "status": "error",
  "error_code": "ALL_SUBSCRIPTIONS_FAILED",
  "message": "Failed to subscribe to all requested topics",
  "failures": [
    {
      "topic": "/invalid_topic",
      "reason": "Topic does not exist"
    }
  ]
}
```

#### Response Fields

- `status` (string): `"success"`, `"partial_success"`, or `"error"`
- `schemas` (object): Map of topic name to schema string (only for successful subscriptions)
- `message` (string): Human-readable message (only for partial_success/error)
- `failures` (array): List of failed subscriptions (only for partial_success/error)
  - `topic` (string): Topic name that failed
  - `reason` (string): Specific reason for failure

#### Schema Format

Schemas are returned as text strings in ROS2 .msg format with embedded nested type definitions:
- Base message definition comes first
- Nested types are separated by `================================================================================`
- Each nested type starts with `MSG: package_name/TypeName`
- Format matches rosbag2 MCAP storage

Example schema:
```
std_msgs/Header header
geometry_msgs/Quaternion orientation
float64[9] orientation_covariance
...

================================================================================
MSG: std_msgs/Header
uint32 seq
time stamp
string frame_id

================================================================================
MSG: geometry_msgs/Quaternion
float64 x
float64 y
float64 z
float64 w
```

---

### heartbeat

Send a heartbeat to keep the session alive. **Required every 1 second.**

#### Request Format

```json
{
  "command": "heartbeat"
}
```

#### Response Format

```json
{
  "status": "ok"
}
```

#### Session Timeout

- Heartbeats must be sent **at least once per second**
- If no heartbeat is received for **10 seconds** (default, configurable), the session times out
- On timeout, the server automatically:
  - Unsubscribes from all topics for that client
  - Removes the client session
  - Stops sending data to that client

#### Implementation Notes

With WebSocket, both API commands and data stream share a single connection. The heartbeat can be sent from a background thread (using a send lock) while the main thread receives frames. Heartbeat responses arrive as text frames and can be distinguished from binary data frames.

---

## Data Streaming

After subscribing to topics, the server broadcasts aggregated messages at 50 Hz as WebSocket binary frames.

### Message Flow

1. Server collects messages from all subscribed topics
2. Every 20ms (50 Hz), server aggregates all new messages
3. Aggregated message is serialized to binary format
4. Binary data is compressed using ZSTD
5. Compressed data is sent as a WebSocket binary frame to all connected clients

### Receiving Data

1. Receive binary frame from WebSocket connection
2. Decompress using ZSTD decompression library
3. Parse the decompressed binary data according to format below

### Important Notes

- Messages are only sent if there is new data (no empty messages)
- If no topics are publishing, you may receive messages less frequently than 50 Hz
- Each aggregated message may contain multiple messages from different topics
- Messages from the same topic may appear multiple times if they were published between aggregation cycles
- Text frames received during data streaming are API responses (e.g., heartbeat acknowledgements) and should be handled separately

---

## Binary Serialization Format

The aggregated message uses a custom little-endian binary format.

### Message Structure

Messages are serialized in a streaming format with no header or message count. Parse sequentially until the buffer is consumed.

```
[Message 1]
[Message 2]
...
[Message N]
```

### Individual Message Structure

```
[Topic Name Length: uint16_t]
[Topic Name: N bytes UTF-8]
[Timestamp: uint64_t nanoseconds since epoch]
[Message Data Length: uint32_t]
[Message Data: N bytes CDR serialized]
```

### Detailed Format

| Field | Type | Size | Description |
|-------|------|------|-------------|
| **For each message (repeat until end of buffer):** | | | |
| Topic Name Length | `uint16_t` | 2 bytes | Length of topic name string |
| Topic Name | `char[]` | N bytes | UTF-8 encoded topic name |
| Timestamp | `uint64_t` | 8 bytes | Nanoseconds since Unix epoch |
| Message Data Length | `uint32_t` | 4 bytes | Length of CDR serialized data |
| Message Data | `uint8_t[]` | N bytes | ROS2 CDR serialized message |

### Endianness

**All multi-byte integers are little-endian.**

### Timestamp

The timestamp is the time when the message was **received** by the bridge server (not the original publish time from the message header). This represents when the bridge received the message from the ROS2 topic.

### Message Data

The message data is the raw CDR (Common Data Representation) serialized ROS2 message. This is the same serialization format used by ROS2 internally.

You can deserialize it using ROS2 CDR libraries or parse it manually if you know the message structure.

## Error Handling

### Error Response Format

All API errors follow this format:

```json
{
  "status": "error",
  "error_code": "ERROR_CODE",
  "message": "Human readable error message"
}
```

### Common Error Codes

| Error Code | Cause | Solution |
|------------|-------|----------|
| `INVALID_JSON` | Malformed JSON request | Check JSON syntax |
| `INVALID_REQUEST` | Missing required field | Check request format |
| `UNKNOWN_COMMAND` | Invalid command name | Use valid command (get_topics, subscribe, heartbeat) |
| `TOPIC_NOT_FOUND` | Topic does not exist | Check available topics with get_topics |
| `ALL_SUBSCRIPTIONS_FAILED` | All requested topics failed | Check failures array for specific reasons |
| `INTERNAL_ERROR` | Server internal error | Check server logs |

### Subscription Failures

Individual topic subscriptions may fail for various reasons:

```json
{
  "topic": "/camera/image",
  "reason": "Topic does not exist"
}
```

```json
{
  "topic": "/custom_topic",
  "reason": "Schema extraction failed: Package 'custom_msgs' not found"
}
```

Common reasons:
- Topic does not exist (not being published)
- Message package not installed
- Schema extraction failed (corrupted .msg file)
- Subscription manager failed (internal error)

---

## Session Management

### Client Identity

The server identifies clients using the WebSocket connection's unique ID (`connectionState->getId()`). Each WebSocket connection automatically gets a unique session.

### Session Lifecycle

1. **Creation**: Session created on first API request (get_topics, subscribe, or heartbeat)
2. **Active**: Session remains active as long as heartbeats are received
3. **Timeout**: After 10 seconds without heartbeat, session is destroyed
4. **Disconnect**: When WebSocket connection closes, session is immediately cleaned up
5. **Cleanup**: On timeout or disconnect, server:
   - Unsubscribes from all client's topics
   - Removes topic subscriptions that no longer have any clients
   - Deletes session data

### Heartbeat Requirements

- **Frequency**: Send heartbeat every 1 second
- **Timeout**: 10 seconds (default, configurable via `session_timeout` parameter)
- **Recommendation**: Send heartbeat every 1 second to have a 10-second buffer

### Shared Subscriptions

When multiple clients subscribe to the same topic:
- Server creates only **one** ROS2 subscription
- All clients receive the same data
- When last client unsubscribes, ROS2 subscription is removed

This optimization reduces resource usage and improves performance.

---

## Summary

### Quick Reference

**WebSocket** (port 8080):
- Text frames: `get_topics`, `subscribe`, `heartbeat` (JSON)
- Binary frames: 50 Hz aggregated messages (ZSTD compressed)

**Session Management**:
- Heartbeat every 1 second required
- 10 second timeout without heartbeat
- Automatic cleanup on timeout or disconnect

**Error Handling**:
- Check `status` field in responses
- Handle `partial_success` for subscription failures
- See `failures` array for specific error reasons
