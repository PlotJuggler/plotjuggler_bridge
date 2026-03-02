# Architecture

## Overview

pj_bridge is a multi-backend bridge server that forwards middleware topic data over WebSocket to PlotJuggler clients. A backend-agnostic core library (`app/`) is shared by three backend-specific adapters:

- **ROS2 backend** (`ros2/`) — uses `rclcpp`, schema from `.msg` files
- **RTI backend** (`rti/`) — uses RTI Connext DDS, schema from OMG IDL (build disabled, code preserved)
- **FastDDS backend** (`fastdds/`) — uses eProsima Fast DDS 3.4 (via Conan), schema from OMG IDL

```
┌──────────────────────────────────────────────────┐
│                  app/ (core)                      │
│  BridgeServer ← TopicSourceInterface             │
│               ← SubscriptionManagerInterface     │
│               ← MiddlewareInterface              │
│  + MessageBuffer, SessionManager, Serializer     │
└────────────────────┬─────────────────────────────┘
         ┌───────────┼───────────────┐
    ┌────┴────┐ ┌────┴─────┐  ┌─────┴──────┐
    │  ros2/  │ │   rti/   │  │  fastdds/  │
    │ Ros2    │ │ Rti      │  │ FastDds    │
    │ Topic   │ │ Topic    │  │ Topic      │
    │ Source  │ │ Source   │  │ Source     │
    │ (rclcpp)│ │(Connext) │  │(Fast DDS) │
    └─────────┘ └──────────┘  └────────────┘
```

## Communication Pattern

Single WebSocket port (default 8080), two frame types:

- **Text frames**: JSON API requests and responses (`get_topics`, `subscribe`, `unsubscribe`, `heartbeat`, `pause`, `resume`)
- **Binary frames**: ZSTD-compressed aggregated message stream, sent per-client based on their subscriptions

## Abstract Interfaces

Three interfaces decouple the core from any specific middleware:

### TopicSourceInterface

Discovers available topics and retrieves their schemas. Implementations:
- `Ros2TopicSource`: wraps `TopicDiscovery` (rclcpp enumeration) + `SchemaExtractor` (.msg file parsing). Schema encoding: `"ros2msg"`.
- `RtiTopicSource`: wraps `DdsTopicDiscovery` (RTI participant discovery). Schema encoding: `"omgidl"`.
- `FastDdsTopicSource`: directly implements the interface (flattened design). Discovers topics via `on_data_writer_discovery()`, resolves `DynamicType` from TypeObject registry, generates IDL via `idl_serialize()`. Schema encoding: `"omgidl"`.

### SubscriptionManagerInterface

Manages ref-counted middleware subscriptions. A single global `MessageCallback` delivers all incoming messages as `shared_ptr<vector<byte>>` to the `MessageBuffer`. Implementations:
- `Ros2SubscriptionManager`: wraps `GenericSubscriptionManager`. Converts `rclcpp::SerializedMessage` to `shared_ptr<vector<byte>>` via memcpy. Optionally strips large message fields (Image, PointCloud2) via `MessageStripper`.
- `RtiSubscriptionManager`: wraps `DdsSubscriptionManager`. DDS natively produces `shared_ptr<vector<byte>>`, no conversion needed.
- `FastDdsSubscriptionManager`: directly implements the interface (flattened design). Creates `DataReader`s with `DynamicPubSubType`, deserializes into `DynamicData` and re-serializes to extract CDR bytes.

### MiddlewareInterface

Abstract transport layer for client connections. Separates text (request/reply) and binary (per-client push) channels. Single implementation: `WebSocketMiddleware` (IXWebSocket).

## Core Components

### BridgeServer

Main orchestrator. Takes all three interfaces via constructor injection (dependency injection). Does **not** own timers — the entry point drives the event loop by calling three public methods:
- `process_requests()` — polls and handles one JSON API request
- `publish_aggregated_messages()` — drains the message buffer, serializes per subscription group, compresses with ZSTD, sends per-client binary frames
- `check_session_timeouts()` — evicts sessions with expired heartbeats

Key behaviors:
- **Schema-before-subscribe**: `get_schema()` is called before `subscribe()` to avoid corrupting ref counts if schema extraction fails
- **Additive subscriptions**: `handle_subscribe()` only adds topics, never removes; rate limits can be updated for already-subscribed topics
- **Ref-counted pause/resume**: `handle_pause()` decrements subscription ref counts; `handle_resume()` increments them; `cleanup_session()` skips unsubscribe for paused clients

### MessageBuffer

Thread-safe per-topic message buffer. Messages are stored as `BufferedMessage` containing:
- `timestamp_ns` — source-clock timestamp from the middleware
- `received_at_ns` — wall-clock time at insertion (used for TTL cleanup)
- `data` — `shared_ptr<vector<byte>>`, CDR-encoded payload

Stale messages (older than 1 second by `received_at_ns`) are removed on every `add_message()` call. `move_messages()` atomically drains the buffer into the caller's map.

### SessionManager

Tracks client sessions by WebSocket connection identity:
- Heartbeat monitoring (default 10-second timeout)
- Per-client subscription tracking with per-topic rate limits
- Pause/resume state
- Thread-safe for concurrent access

### AggregatedMessageSerializer

Streaming binary serializer, no middleware dependencies. Per-message wire format:
- Topic name length (uint16_t LE) + topic name (UTF-8)
- Timestamp (uint64_t ns, LE)
- Message data length (uint32_t LE) + message data (CDR)

`finalize()` produces a 16-byte header (`PJRB` magic, message count, uncompressed size, flags) followed by a ZSTD-compressed payload. See [API docs](API.md#binary-message-format) for full format.

### WebSocketMiddleware

IXWebSocket-based implementation of `MiddlewareInterface`:
- Connection-oriented client identity via `connectionState->getId()`
- Incoming text requests queued and dequeued via `receive_request()`
- Per-client binary send for aggregated message frames
- Connect/disconnect callbacks for automatic session lifecycle

## Event Loop

BridgeServer exposes methods; the entry point drives timing:

**ROS2** (`ros2/src/main.cpp`): Three `rclcpp` wall timers:
- 10 ms → `process_requests()`
- 1/publish_rate → `publish_aggregated_messages()`
- 1 s → `check_session_timeouts()`

Spun via `SingleThreadedExecutor::spin_some(100ms)`.

**RTI** (`rti/src/main.cpp`): `std::chrono` loop with 1 ms sleep:
- Every iteration → `process_requests()`
- At publish_rate → `publish_aggregated_messages()`
- Every 1 s → `check_session_timeouts()`
- Every 5 s (optional) → stats snapshot

**FastDDS** (`fastdds/src/main.cpp`): Same `std::chrono` loop pattern as RTI.

## ROS2-Specific Components

- **TopicDiscovery**: Discovers topics via `rclcpp::Node::get_topic_names_and_types()`, filtering system topics
- **SchemaExtractor**: Reads `.msg` files from ROS2 package share directories via `ament_index_cpp`, recursively expanding nested types (matches rosbag2 MCAP format)
- **GenericSubscriptionManager**: Ref-counted `rclcpp::GenericSubscription` per topic
- **MessageStripper**: Strips data fields from large message types (Image, PointCloud2, etc.)

## RTI-Specific Components

- **DdsTopicDiscovery**: Discovers topics via DDS participant discovery across configured domain IDs
- **DdsSubscriptionManager**: Manages DDS DataReaders, natively produces `shared_ptr<vector<byte>>`

## FastDDS-Specific Components

Unlike the RTI backend's 4-class two-level design (discovery + subscription manager + adapters), the FastDDS backend uses a flattened 2-class design that directly implements the abstract interfaces:

- **FastDdsTopicSource**: Manages `DomainParticipant`s, discovers topics via `DomainParticipantListener::on_data_writer_discovery()`, resolves `DynamicType` from `TypeObjectRegistry`, generates IDL schema via `idl_serialize()`. Also provides `get_dynamic_type()` / `get_participant()` / `get_domain_id()` for use by the subscription manager.
- **FastDdsSubscriptionManager**: Creates `DataReader`s with `DynamicPubSubType`, ref-counted subscriptions. Extracts CDR bytes by deserializing into `DynamicData` and re-serializing via `DynamicPubSubType::serialize()`.

FastDDS dependencies are managed via Conan (`fast-dds/3.4.0`). The backend is built standalone (not through colcon/ament).

## Design Decisions

### Backend-Agnostic Core via Interfaces
`TopicSourceInterface` and `SubscriptionManagerInterface` allow the same `BridgeServer` to work with ROS2 or RTI DDS without compile-time dependencies on either middleware.

### Message Data as `shared_ptr<vector<byte>>`
Backend-agnostic type eliminates `rclcpp::SerializedMessage` from the core. ROS2 adapter converts via memcpy; RTI adapter already produces this type natively.

### Externalized Event Loop
BridgeServer has no internal timers. This avoids coupling to any specific executor model (rclcpp timers vs. `std::chrono` loop) and keeps the core library free of middleware dependencies.

### Shared Subscriptions with Reference Counting
One underlying middleware subscription per topic, shared across all clients. Ref count incremented on subscribe/resume, decremented on unsubscribe/pause/disconnect.

### TTL Cleanup by Wall-Clock Time
MessageBuffer cleanup uses `received_at_ns` (wall clock), not `timestamp_ns` (source clock). This prevents sim-time offsets from causing premature eviction or unbounded growth.

### Per-Client Message Filtering
Each client receives only messages for its subscribed topics, with per-topic rate limiting. The server serializes, compresses, and sends individually per client.

## Thread Safety

### Lock Ordering (to prevent deadlock)

```
cleanup_mutex_ > last_sent_mutex_ > stats_mutex_
```

`SessionManager::mutex_` may be acquired while holding any of these. Never acquire a higher-order lock while holding a lower-order one.

### Per-Component Guarantees

- **MessageBuffer**: Internal mutex protects all buffer operations
- **SessionManager**: Internal mutex protects session map
- **GenericSubscriptionManager**: Internal mutex protects subscription map
- **BridgeServer**: `cleanup_mutex_` prevents concurrent cleanup; `last_sent_mutex_` protects rate-limiting state; `stats_mutex_` protects counters. Frames are built under `last_sent_mutex_`, sent outside it (minimizes lock contention)
- **WebSocketMiddleware**: Separate mutexes for state, client map, and message queue

## Shutdown Sequence

Both entry points follow the same ordered shutdown to avoid use-after-free:

1. Cancel timers / exit event loop
2. Clear the subscription manager's message callback (`set_message_callback(nullptr)`)
3. Unsubscribe all middleware subscriptions
4. Shutdown WebSocket server (BridgeServer destructor handles this if not done explicitly)

## Performance Characteristics

- **Compression**: ZSTD level 1 (typically 50-70% size reduction)
- **Memory**: Automatic 1-second TTL prevents unbounded growth
- **Concurrent Clients**: 10+ clients supported
