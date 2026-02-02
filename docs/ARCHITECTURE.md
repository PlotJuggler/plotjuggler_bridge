# Architecture

## Communication Pattern

The server uses a single WebSocket port (default 8080):

- **Text frames**: JSON API requests and responses (get_topics, subscribe, heartbeat)
- **Binary frames**: ZSTD-compressed aggregated message stream (per-client filtered)

## Core Components

### MiddlewareInterface / WebSocketMiddleware

Abstract middleware layer over IXWebSocket. Provides:
- Connection-oriented client identity via `connectionState->getId()`
- Text frame send/receive for JSON API
- Binary frame broadcast and per-client send
- Connect/disconnect callbacks for automatic session cleanup

### TopicDiscovery

Discovers available ROS2 topics using `rclcpp::Node::get_topic_names_and_types()`. Filters system topics.

### SchemaExtractor

Extracts complete message schemas by reading `.msg` files from ROS2 package share directories via `ament_index_cpp`. Recursively expands nested types using depth-first traversal, producing schemas identical to rosbag2 MCAP storage format.

### GenericSubscriptionManager

Manages ROS2 subscriptions using `rclcpp::GenericSubscription` with reference counting. A single ROS2 subscription is shared across all clients subscribing to the same topic. Subscriptions are destroyed when the reference count reaches zero.

### MessageBuffer

Thread-safe per-topic message buffer:
- **Zero-copy**: Messages stored as `shared_ptr<SerializedMessage>`
- **Auto-cleanup**: Messages older than 1 second are removed on every insertion
- **Move semantics**: `move_messages()` atomically transfers buffer ownership

### SessionManager

Tracks client sessions using WebSocket connection identity:
- Heartbeat monitoring (expected every 1 second, default 10 second timeout)
- Per-client subscription tracking
- Automatic cleanup on WebSocket disconnect or heartbeat timeout
- Thread-safe for concurrent access

### AggregatedMessageSerializer

Custom streaming binary serializer:
- Messages serialized immediately to output buffer (no intermediate storage)
- ZSTD compression (level 1) applied to final buffer
- See [API docs](API.md#binary-message-format) for wire format

### BridgeServer

Main orchestrator integrating all components:
- Handles API request/response routing
- Per-client message filtering and delivery
- Session timeout monitoring (1 Hz timer)
- Message aggregation publishing (configurable rate, default 50 Hz)
- Thread-safe cleanup with idempotency guard

## Design Decisions

### Shared Subscriptions with Reference Counting
Single ROS2 subscription per topic shared across clients reduces resource usage. Requires careful reference counting and thread safety.

### Per-Client Message Filtering
Each client receives only messages for its subscribed topics. The server iterates active sessions, filters the message buffer, serializes, compresses, and sends individually per client.

### Custom Binary Serialization
Hand-crafted format with no external dependencies. Little-endian throughout. See [API docs](API.md#binary-message-format) for format details.

### Schema Extraction via .msg Files
Reads .msg files directly from ROS2 package share directories rather than runtime introspection. Simpler, more reliable for complex types, and matches rosbag2's approach.

## Performance Characteristics

- **Throughput**: >1000 messages/second
- **Latency**: <100ms (publish time to receive time)
- **Concurrent Clients**: 10+ clients supported
- **Compression**: ZSTD level 1 (typically 50-70% size reduction)
- **Memory**: Automatic cleanup prevents unbounded growth (1 second message retention)

## Thread Safety

- `MessageBuffer`: Internal mutex protects all buffer operations
- `SessionManager`: Internal mutex protects session map
- `GenericSubscriptionManager`: Internal mutex protects subscription map
- `BridgeServer`: `cleanup_mutex_` prevents concurrent cleanup of the same session; `stats_mutex_` protects publish statistics
- `WebSocketMiddleware`: Separate mutexes for state, client map, and message queue
