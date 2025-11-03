# pj_ros_bridge API Protocol Specification

This document describes the client-server protocol for `pj_ros_bridge`. It is intended for developers implementing clients in any programming language.

## Table of Contents

- [Overview](#overview)
- [Connection Setup](#connection-setup)
- [API Commands (REQ-REP)](#api-commands-req-rep)
  - [get_topics](#get_topics)
  - [subscribe](#subscribe)
  - [heartbeat](#heartbeat)
- [Data Streaming (PUB-SUB)](#data-streaming-pub-sub)
- [Binary Serialization Format](#binary-serialization-format)
- [Error Handling](#error-handling)
- [Session Management](#session-management)
- [Client Implementation Guide](#client-implementation-guide)

---

## Overview

The `pj_ros_bridge` server uses two ZeroMQ communication patterns:

1. **REQ-REP** (Request-Reply): For API commands - client sends JSON requests, server responds with JSON
2. **PUB-SUB** (Publish-Subscribe): For data streaming - server publishes aggregated messages at 50 Hz

### Network Ports

- **REQ-REP Port**: 5555 (default, configurable)
- **PUB-SUB Port**: 5556 (default, configurable)

### Data Formats

- **API Messages**: JSON (UTF-8 encoded strings)
- **Data Stream**: Custom binary format with ZSTD compression

---

## Connection Setup

### Client Connection Sequence

1. **Connect to REQ socket** (`tcp://server:5555`)
2. **Discover topics** using `get_topics` command
3. **Subscribe to topics** using `subscribe` command
4. **Connect to SUB socket** (`tcp://server:5556`) and subscribe to all topics (`""`)
5. **Start heartbeat thread** to send heartbeat every 1 second
6. **Receive and process** aggregated messages from SUB socket

### ZeroMQ Socket Configuration

```python
import zmq

context = zmq.Context()

# REQ socket for API
req_socket = context.socket(zmq.REQ)
req_socket.connect("tcp://localhost:5555")

# SUB socket for data
sub_socket = context.socket(zmq.SUB)
sub_socket.connect("tcp://localhost:5556")
sub_socket.setsockopt(zmq.SUBSCRIBE, b"")  # Subscribe to all topics
```

---

## API Commands (REQ-REP)

All API commands follow the REQ-REP pattern:
1. Client sends JSON request
2. Server responds with JSON response
3. Requests and responses are always paired (one response per request)

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

#### Example

```python
import json

request = {"command": "get_topics"}
req_socket.send_string(json.dumps(request))
response = json.loads(req_socket.recv_string())

print(f"Found {len(response['topics'])} topics")
for topic in response['topics']:
    print(f"  {topic['name']} ({topic['type']})")
```

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

#### Example

```python
request = {
    "command": "subscribe",
    "topics": ["/imu/data", "/camera/image"]
}
req_socket.send_string(json.dumps(request))
response = json.loads(req_socket.recv_string())

if response['status'] == 'success':
    print(f"Subscribed to {len(response['schemas'])} topics")
    for topic, schema in response['schemas'].items():
        print(f"Schema for {topic}:")
        print(schema[:200] + "...")  # Print first 200 chars
elif response['status'] == 'partial_success':
    print(f"Subscribed to {len(response['schemas'])} topics")
    print(f"Failed subscriptions: {len(response['failures'])}")
    for failure in response['failures']:
        print(f"  {failure['topic']}: {failure['reason']}")
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

#### Example

```python
import threading
import time

def heartbeat_thread(req_socket):
    while running:
        request = {"command": "heartbeat"}
        req_socket.send_string(json.dumps(request))
        response = req_socket.recv_string()  # Wait for acknowledgment
        time.sleep(1.0)  # Send every second

# Start heartbeat in background thread
heartbeat = threading.Thread(target=heartbeat_thread, args=(req_socket,))
heartbeat.daemon = True
heartbeat.start()
```

**Important**: The REQ socket can only have one outstanding request at a time. If you're sending other commands, you must wait for their responses before sending the next heartbeat.

---

## Data Streaming (PUB-SUB)

After subscribing to topics, the server publishes aggregated messages at 50 Hz via the PUB socket.

### Message Flow

1. Server collects messages from all subscribed topics
2. Every 20ms (50 Hz), server aggregates all new messages
3. Aggregated message is serialized to binary format
4. Binary data is compressed using ZSTD
5. Compressed data is published via PUB socket

### Receiving Data

```python
# This is a blocking call - will wait for next message
compressed_data = sub_socket.recv()

# Decompress using ZSTD
import zstandard as zstd
dctx = zstd.ZstdDecompressor()
decompressed_data = dctx.decompress(compressed_data)

# Parse binary format (see next section)
messages = parse_aggregated_message(decompressed_data)
```

### Important Notes

- Messages are only sent if there is new data (no empty messages)
- If no topics are publishing, you may receive messages less frequently than 50 Hz
- Each aggregated message may contain multiple messages from different topics
- Messages from the same topic may appear multiple times if they were published between aggregation cycles

---

## Binary Serialization Format

The aggregated message uses a custom little-endian binary format.

### Message Structure

```
[Message Count: uint32_t]
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
| Message Count | `uint32_t` | 4 bytes | Number of messages in aggregation |
| **For each message:** | | | |
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

The message data is the raw CDR (Common Data Representation) serialized ROS2 message. This is the same serialization format used by ROS2 internally. You can deserialize it using ROS2 CDR libraries or parse it manually if you know the message structure.

### Python Deserialization Example

```python
import struct

def parse_aggregated_message(data):
    messages = []
    offset = 0

    # Read message count
    message_count = struct.unpack_from('<I', data, offset)[0]
    offset += 4

    for _ in range(message_count):
        # Read topic name length
        topic_name_len = struct.unpack_from('<H', data, offset)[0]
        offset += 2

        # Read topic name
        topic_name = data[offset:offset + topic_name_len].decode('utf-8')
        offset += topic_name_len

        # Read timestamp
        timestamp_ns = struct.unpack_from('<Q', data, offset)[0]
        offset += 8

        # Read message data length
        msg_data_len = struct.unpack_from('<I', data, offset)[0]
        offset += 4

        # Read message data
        msg_data = data[offset:offset + msg_data_len]
        offset += msg_data_len

        messages.append({
            'topic': topic_name,
            'timestamp_ns': timestamp_ns,
            'data': msg_data
        })

    return messages
```

### C++ Deserialization Example

```cpp
#include <cstdint>
#include <string>
#include <vector>

struct Message {
    std::string topic;
    uint64_t timestamp_ns;
    std::vector<uint8_t> data;
};

std::vector<Message> parse_aggregated_message(const std::vector<uint8_t>& data) {
    std::vector<Message> messages;
    size_t offset = 0;

    // Read message count
    uint32_t message_count;
    std::memcpy(&message_count, &data[offset], sizeof(uint32_t));
    offset += sizeof(uint32_t);

    for (uint32_t i = 0; i < message_count; ++i) {
        Message msg;

        // Read topic name length
        uint16_t topic_name_len;
        std::memcpy(&topic_name_len, &data[offset], sizeof(uint16_t));
        offset += sizeof(uint16_t);

        // Read topic name
        msg.topic = std::string(reinterpret_cast<const char*>(&data[offset]), topic_name_len);
        offset += topic_name_len;

        // Read timestamp
        std::memcpy(&msg.timestamp_ns, &data[offset], sizeof(uint64_t));
        offset += sizeof(uint64_t);

        // Read message data length
        uint32_t msg_data_len;
        std::memcpy(&msg_data_len, &data[offset], sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // Read message data
        msg.data = std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + msg_data_len);
        offset += msg_data_len;

        messages.push_back(msg);
    }

    return messages;
}
```

---

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
| `INVALID_CLIENT` | No client identity | ZMQ connection issue |
| `INTERNAL_ERROR` | Server internal error | Check server logs |

### Middleware Initialization Errors

If the server fails to start, you may see detailed error messages like:

- `"Failed to bind REP socket to port 5555: Address already in use (errno 98)"`
- `"Failed to create ZMQ context: ..."`

These errors are logged by the server and prevent startup.

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

The server identifies clients using ZeroMQ's built-in connection identity. Each client connection automatically gets a unique session.

### Session Lifecycle

1. **Creation**: Session created on first API request (get_topics, subscribe, or heartbeat)
2. **Active**: Session remains active as long as heartbeats are received
3. **Timeout**: After 10 seconds without heartbeat, session is destroyed
4. **Cleanup**: On timeout or disconnect, server:
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

## Client Implementation Guide

### Minimal Client (Python)

```python
import zmq
import json
import zstandard as zstd
import struct
import threading
import time

class BridgeClient:
    def __init__(self, req_addr="tcp://localhost:5555", pub_addr="tcp://localhost:5556"):
        self.context = zmq.Context()

        # REQ socket for API
        self.req_socket = self.context.socket(zmq.REQ)
        self.req_socket.connect(req_addr)

        # SUB socket for data
        self.sub_socket = self.context.socket(zmq.SUB)
        self.sub_socket.connect(pub_addr)
        self.sub_socket.setsockopt(zmq.SUBSCRIBE, b"")

        # Start heartbeat thread
        self.running = True
        self.heartbeat_thread = threading.Thread(target=self._heartbeat_loop)
        self.heartbeat_thread.daemon = True
        self.heartbeat_thread.start()

    def get_topics(self):
        """Get list of available topics"""
        request = {"command": "get_topics"}
        self.req_socket.send_string(json.dumps(request))
        response = json.loads(self.req_socket.recv_string())
        return response['topics']

    def subscribe(self, topics):
        """Subscribe to list of topics"""
        request = {"command": "subscribe", "topics": topics}
        self.req_socket.send_string(json.dumps(request))
        response = json.loads(self.req_socket.recv_string())
        return response

    def _heartbeat_loop(self):
        """Background thread sending heartbeats"""
        while self.running:
            try:
                request = {"command": "heartbeat"}
                self.req_socket.send_string(json.dumps(request))
                self.req_socket.recv_string()  # Wait for ack
            except:
                break
            time.sleep(1.0)

    def receive_messages(self):
        """Receive and decompress aggregated messages"""
        compressed_data = self.sub_socket.recv()

        # Decompress
        dctx = zstd.ZstdDecompressor()
        data = dctx.decompress(compressed_data)

        # Parse
        return self._parse_aggregated_message(data)

    def _parse_aggregated_message(self, data):
        """Parse binary aggregated message format"""
        messages = []
        offset = 0

        # Read message count
        message_count = struct.unpack_from('<I', data, offset)[0]
        offset += 4

        for _ in range(message_count):
            # Topic name
            topic_name_len = struct.unpack_from('<H', data, offset)[0]
            offset += 2
            topic_name = data[offset:offset + topic_name_len].decode('utf-8')
            offset += topic_name_len

            # Timestamp
            timestamp_ns = struct.unpack_from('<Q', data, offset)[0]
            offset += 8

            # Message data
            msg_data_len = struct.unpack_from('<I', data, offset)[0]
            offset += 4
            msg_data = data[offset:offset + msg_data_len]
            offset += msg_data_len

            messages.append({
                'topic': topic_name,
                'timestamp_ns': timestamp_ns,
                'data': msg_data
            })

        return messages

    def close(self):
        """Shutdown client"""
        self.running = False
        self.heartbeat_thread.join(timeout=2.0)
        self.req_socket.close()
        self.sub_socket.close()
        self.context.term()

# Usage example
client = BridgeClient()

# Get available topics
topics = client.get_topics()
print(f"Available topics: {[t['name'] for t in topics]}")

# Subscribe to topics
response = client.subscribe(["/imu/data", "/camera/image"])
if response['status'] == 'success':
    print(f"Subscribed to {len(response['schemas'])} topics")

# Receive messages
try:
    while True:
        messages = client.receive_messages()
        for msg in messages:
            print(f"Received {msg['topic']} at {msg['timestamp_ns']}")
except KeyboardInterrupt:
    client.close()
```

### Key Implementation Points

1. **Two Sockets**: Always use separate REQ and SUB sockets
2. **Heartbeat Thread**: Run heartbeat in background thread to avoid blocking
3. **Error Handling**: Check `status` field in all responses
4. **ZSTD Decompression**: Always decompress data from SUB socket before parsing
5. **Little-Endian**: Use `<` format character in struct.unpack for little-endian
6. **Session Cleanup**: Ensure heartbeat continues running while client is active

### Testing Your Client

Use the included Python test client as reference:
```bash
python3 tests/integration/test_client.py --subscribe /topic1 /topic2
```

The test client includes:
- Complete error handling
- Statistics tracking
- Latency measurement
- Command-line interface

---

## Summary

### Quick Reference

**REQ-REP Commands** (port 5555):
- `get_topics`: Discover topics
- `subscribe`: Subscribe to topics (absolute list)
- `heartbeat`: Keep session alive (every 1 second)

**PUB-SUB Data** (port 5556):
- 50 Hz aggregated messages
- ZSTD compressed
- Custom binary format (little-endian)

**Session Management**:
- Heartbeat every 1 second required
- 10 second timeout without heartbeat
- Automatic cleanup on timeout

**Error Handling**:
- Check `status` field in responses
- Handle `partial_success` for subscription failures
- See `failures` array for specific error reasons
