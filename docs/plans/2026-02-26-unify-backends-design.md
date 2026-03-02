# Unify pj_ros_bridge + dds_websocket_bridge into pj_bridge

**Date**: 2026-02-26
**Status**: Proposed

## Context

Two projects implement identical WebSocket bridge functionality for different backends:
- **pj_ros_bridge** (`/home/davide/ws_plotjuggler/src/pj_ros_bridge/`) — ROS2 Humble
- **dds_websocket_bridge** (`~/Asensus/ros_starman_ws/dds_websocket_bridge/`) — RTI Connext DDS

They share identical: API protocol, binary wire format (PJRB + ZSTD), session management, message buffering, serialization, and WebSocket middleware. The only differences are topic discovery, schema extraction, subscription management, logging, and build system.

**Goal**: Unify into a single project `pj_bridge` with a backend-agnostic `app/` core library and two optional plugins (`ros2/`, `rti/`). No client-side changes needed.

## Design Decisions

- **Project name**: `pj_bridge` (rename from `pj_ros_bridge`)
- **Namespace**: `pj_bridge` (rename from `pj_ros_bridge`)
- **Event loop**: App-driven (no timers in BridgeServer; main.cpp calls methods)
- **Executables**: `pj_bridge_ros2` + `pj_bridge_rti` (two separate binaries)
- **Logging**: spdlog in app layer (FetchContent, like IXWebSocket)
- **Message stripping**: ROS2-plugin only
- **RTI detection**: `find_package(RTIConnextDDS QUIET)`
- **ROS2 detection**: `find_package(ament_cmake QUIET)` (auto-detected by colcon)

## Abstract Interfaces

Two new interfaces in `app/include/pj_bridge/`:

### TopicSourceInterface

```cpp
struct TopicInfo { std::string name; std::string type; };

class TopicSourceInterface {
public:
  virtual ~TopicSourceInterface() = default;
  virtual std::vector<TopicInfo> get_topics() = 0;
  virtual std::string get_schema(const std::string& topic_name) = 0;
  virtual std::string schema_encoding() const = 0;  // "ros2msg" or "omgidl"
};
```

### SubscriptionManagerInterface

```cpp
using MessageCallback = std::function<void(
    const std::string& topic_name,
    std::shared_ptr<std::vector<std::byte>> cdr_data,
    uint64_t timestamp_ns)>;

class SubscriptionManagerInterface {
public:
  virtual ~SubscriptionManagerInterface() = default;
  virtual void set_message_callback(MessageCallback callback) = 0;
  virtual bool subscribe(const std::string& topic_name, const std::string& topic_type) = 0;
  virtual bool unsubscribe(const std::string& topic_name) = 0;
  virtual void unsubscribe_all() = 0;
};
```

**Key design rationale:**

- `subscribe()` takes both `topic_name` and `topic_type` — ROS2 needs the type for `GenericSubscription`; DDS ignores it.
- `set_message_callback()` — single global callback (DDS pattern). ROS2 adapter internally routes per-topic callbacks to this.
- Message type is `shared_ptr<vector<byte>>` — eliminates rclcpp dependency from app layer. ROS2 adapter converts `SerializedMessage` to `vector<byte>` (one memcpy, negligible vs ZSTD compression).

## Target Directory Structure

```
pj_bridge/
├── app/
│   ├── include/pj_bridge/
│   │   ├── topic_source_interface.hpp       [NEW]
│   │   ├── subscription_manager_interface.hpp [NEW]
│   │   ├── middleware/
│   │   │   ├── middleware_interface.hpp      [from include/pj_ros_bridge/middleware/]
│   │   │   └── websocket_middleware.hpp      [from include/pj_ros_bridge/middleware/]
│   │   ├── bridge_server.hpp                [refactored - no ROS2 deps]
│   │   ├── session_manager.hpp              [from include/pj_ros_bridge/]
│   │   ├── message_buffer.hpp               [unified - vector<byte>]
│   │   ├── message_serializer.hpp           [unified - byte*, size_t]
│   │   ├── protocol_constants.hpp           [merged both encodings]
│   │   └── time_utils.hpp                   [from include/pj_ros_bridge/]
│   └── src/
│       ├── middleware/websocket_middleware.cpp
│       ├── bridge_server.cpp                [refactored - spdlog, interfaces]
│       ├── session_manager.cpp
│       ├── message_buffer.cpp
│       └── message_serializer.cpp
├── ros2/
│   ├── include/pj_bridge_ros2/
│   │   ├── ros2_topic_source.hpp            [wraps TopicDiscovery + SchemaExtractor]
│   │   ├── ros2_subscription_manager.hpp    [wraps GenericSubscriptionManager]
│   │   ├── topic_discovery.hpp              [from include/pj_ros_bridge/]
│   │   ├── schema_extractor.hpp             [from include/pj_ros_bridge/]
│   │   ├── generic_subscription_manager.hpp [from include/pj_ros_bridge/]
│   │   └── message_stripper.hpp             [from include/pj_ros_bridge/]
│   └── src/
│       ├── ros2_topic_source.cpp            [NEW adapter]
│       ├── ros2_subscription_manager.cpp    [NEW adapter]
│       ├── topic_discovery.cpp              [from src/]
│       ├── schema_extractor.cpp             [from src/]
│       ├── generic_subscription_manager.cpp [from src/]
│       ├── message_stripper.cpp             [from src/]
│       └── main.cpp                         [ROS2 entry point]
├── rti/
│   ├── include/pj_bridge_rti/
│   │   ├── rti_topic_source.hpp             [wraps DdsTopicDiscovery]
│   │   ├── rti_subscription_manager.hpp     [wraps DdsSubscriptionManager]
│   │   ├── dds_topic_discovery.hpp          [from dds_websocket_bridge]
│   │   └── dds_subscription_manager.hpp     [from dds_websocket_bridge]
│   └── src/
│       ├── rti_topic_source.cpp             [NEW adapter]
│       ├── rti_subscription_manager.cpp     [NEW adapter]
│       ├── dds_topic_discovery.cpp          [from dds_websocket_bridge]
│       ├── dds_subscription_manager.cpp     [from dds_websocket_bridge]
│       └── main.cpp                         [RTI entry point]
├── 3rdparty/                                [existing: nlohmann, tl, ixwebsocket]
├── tests/
│   └── unit/
│       ├── test_bridge_server.cpp           [rewritten with mocks]
│       ├── test_session_manager.cpp         [namespace change]
│       ├── test_message_buffer.cpp          [data type change]
│       ├── test_message_serializer.cpp      [signature change]
│       ├── test_websocket_middleware.cpp     [namespace change]
│       ├── test_protocol_constants.cpp      [namespace change]
│       ├── test_schema_extractor.cpp        [ROS2-only]
│       ├── test_topic_discovery.cpp         [ROS2-only]
│       ├── test_generic_subscription_manager.cpp [ROS2-only]
│       └── test_message_stripper.cpp        [ROS2-only]
├── DATA/                                    [unchanged]
├── cmake/FindZSTD.cmake                     [unchanged]
├── data_path.hpp.in                         [unchanged]
├── CMakeLists.txt                           [rewritten]
├── package.xml                              [renamed to pj_bridge]
├── .clang-tidy                              [unchanged]
├── .pre-commit-config.yaml                  [unchanged]
└── CLAUDE.md                                [updated]
```

## How BridgeServer Changes

| Method | Current (ROS2) | Unified |
|--------|---------------|---------|
| Constructor | `(Node, Middleware, port, timeout, rate, strip)` | `(TopicSource, SubManager, Middleware, port, timeout, rate)` |
| `initialize()` | Creates TopicDiscovery, SchemaExtractor, GenericSubManager, timers | Creates MessageBuffer, SessionManager. Sets message callback. No timers |
| `handle_get_topics()` | `topic_discovery_->discover_topics()` | `topic_source_->get_topics()` |
| `handle_subscribe()` | `schema_extractor_->get_message_definition(type)` + `subscribe(name, type, callback)` | `topic_source_->get_schema(name)` + `subscription_manager_->subscribe(name, type)` |
| `handle_resume()` | `subscribe(topic, type, make_buffer_callback(type))` | `subscription_manager_->subscribe(topic, type)` |
| `cleanup_session()` | `subscription_manager_->unsubscribe(topic)` | Same (interface method) |
| `publish_aggregated_messages()` | ROS2 timer callback, holds lock across send | Public method called by main.cpp. DDS lock ordering (build frames under lock, send outside) |
| `check_session_timeouts()` | ROS2 timer callback | Public method called by main.cpp |
| Logging | `RCLCPP_INFO(node_->get_logger(), "%s", arg)` | `spdlog::info("{}", arg)` |

## Implementation Steps

### Step 1: Create branch and directory structure

Create `unify-backends` branch. Create `app/`, `ros2/`, `rti/` directory trees.

### Step 2: Create abstract interface headers

- `app/include/pj_bridge/topic_source_interface.hpp`
- `app/include/pj_bridge/subscription_manager_interface.hpp`

### Step 3: Move and adapt common components to `app/`

| File | Key Changes |
|------|-------------|
| `middleware_interface.hpp` | Namespace `pj_ros_bridge` → `pj_bridge` |
| `websocket_middleware.hpp/.cpp` | Namespace. Adopt DDS version's improved shutdown (shared_ptr server, timeout+detach thread) |
| `session_manager.hpp/.cpp` | Namespace only |
| `time_utils.hpp` | Namespace only |
| `protocol_constants.hpp` | Namespace. Add `kSchemaEncodingOmgIdl = "omgidl"` |
| `message_buffer.hpp/.cpp` | Namespace. `BufferedMessage` → `shared_ptr<vector<byte>>` + add `received_at_ns` field. Remove rclcpp include |
| `message_serializer.hpp/.cpp` | Namespace. `serialize_message()` → `(const std::byte* data, size_t data_size)`. Remove rclcpp include |

### Step 4: Refactor BridgeServer (the core change)

**Header** (`app/include/pj_bridge/bridge_server.hpp`):
- Remove all ROS2 includes, add `#include <spdlog/spdlog.h>`
- New constructor: `BridgeServer(shared_ptr<TopicSourceInterface>, shared_ptr<SubscriptionManagerInterface>, shared_ptr<MiddlewareInterface>, int port, double session_timeout, double publish_rate)`
- Remove: `node_`, `topic_discovery_`, `schema_extractor_`, old `subscription_manager_` (concrete), timers, `strip_large_messages_`, `make_buffer_callback()`
- Add: `topic_source_`, `subscription_manager_` (via interfaces)
- Make `publish_aggregated_messages()` and `check_session_timeouts()` public
- Add `StatsSnapshot` struct and `snapshot_and_reset_stats()` from DDS version

**Source** (`app/src/bridge_server.cpp`):
- Replace all `RCLCPP_*` with `spdlog::*` (printf → fmt syntax)
- `initialize()`: No timers. Set message callback: `subscription_manager_->set_message_callback([this](topic, data, ts) { message_buffer_->add_message(topic, ts, move(data)); })`
- `handle_get_topics()`: `topic_source_->get_topics()`
- `handle_subscribe()`: `topic_source_->get_schema(topic_name)` + `topic_source_->schema_encoding()` + `subscription_manager_->subscribe(name, type)`
- `publish_aggregated_messages()`: Adopt DDS lock ordering (build frames under lock, send outside). `msg.data->data(), msg.data->size()` for serializer

### Step 5: Move ROS2-specific code to `ros2/`

Move `topic_discovery`, `schema_extractor`, `generic_subscription_manager`, `message_stripper` headers and sources to `ros2/`.

### Step 6: Create ROS2 adapters

**`Ros2TopicSource`**: Implements `TopicSourceInterface`. Constructor takes `rclcpp::Node::SharedPtr`. Internally creates `TopicDiscovery` + `SchemaExtractor`. `get_schema(topic_name)` looks up type from cached topics, calls `SchemaExtractor::get_message_definition(type)`.

**`Ros2SubscriptionManager`**: Implements `SubscriptionManagerInterface`. Constructor takes `rclcpp::Node::SharedPtr`, `bool strip_large_messages`. Internally creates `GenericSubscriptionManager`. `subscribe(name, type)` creates ROS2 callback that: receives `SerializedMessage` → optionally strips → converts to `shared_ptr<vector<byte>>` → calls stored callback.

**`ros2/src/main.cpp`**: ROS2 entry point. Creates node, declares parameters, creates adapters and BridgeServer. Uses ROS2 wall timers to drive `process_requests()`, `publish_aggregated_messages()`, `check_session_timeouts()`. Runs `executor.spin_some()` loop.

### Step 7: Port RTI DDS backend

Copy `DdsTopicDiscovery`, `DdsSubscriptionManager` from dds_websocket_bridge. Create thin `RtiTopicSource` and `RtiSubscriptionManager` adapters. Port `main.cpp` with CLI11 parsing and signal-handler event loop.

### Step 8: Rewrite CMakeLists.txt

Three build targets:
- `pj_bridge_app` (STATIC library, always built) — links ZSTD, IXWebSocket, spdlog
- `pj_bridge_ros2_lib` + `pj_bridge_ros2` executable — conditional on `ament_cmake`
- `pj_bridge_rti_lib` + `pj_bridge_rti` executable — conditional on `RTIConnextDDS`

spdlog added via FetchContent (like IXWebSocket).

### Step 9: Update tests

**Core tests** (link `pj_bridge_app` + gtest, NO rclcpp):
- `test_bridge_server.cpp`: Biggest change. New mocks: `MockTopicSource`, `MockSubscriptionManager`. Remove `rclcpp::init/shutdown`. Fixture: `BridgeServer(mock_topic_source, mock_sub_manager, mock_middleware, ...)`. JSON protocol assertions stay identical.
- `test_message_buffer.cpp`: `rclcpp::SerializedMessage` → `vector<byte>`
- `test_message_serializer.cpp`: `serialize_message` → `(topic, ts, data, size)`
- Others: namespace change only

**ROS2-specific tests** (link `pj_bridge_ros2_lib`, require ament):
- `test_schema_extractor.cpp`, `test_topic_discovery.cpp`, `test_generic_subscription_manager.cpp`, `test_message_stripper.cpp` — namespace change, still require rclcpp

### Step 10: Update package.xml, conanfile.py, CLAUDE.md

## Execution Order (minimizing risk)

| # | Step | Risk | Test Impact |
|---|------|------|-------------|
| 1 | Create branch + dirs + interface headers | None | 0 |
| 2 | Move + adapt `protocol_constants`, `time_utils`, `session_manager` | Low | Namespace fix |
| 3 | Unify `MessageBuffer` (vector\<byte\>) | Medium | ~11 tests |
| 4 | Unify `AggregatedMessageSerializer` (byte*, size) | Medium | ~22 tests |
| 5 | Unify `MiddlewareInterface` + `WebSocketMiddleware` | Low | ~16 tests |
| 6 | Refactor `BridgeServer` + rewrite `test_bridge_server.cpp` | **High** | ~44 tests |
| 7 | Create `Ros2TopicSource` + `Ros2SubscriptionManager` | Medium | 0 |
| 8 | Move ROS2-specific code + tests to `ros2/` | Low | Path changes |
| 9 | Update `ros2/src/main.cpp` | Low | Integration |
| 10 | Rewrite `CMakeLists.txt` | Medium | All (paths) |
| 11 | Port RTI backend from dds_websocket_bridge | Low | New tests |
| 12 | Update `package.xml`, `CLAUDE.md` | None | 0 |

Steps 6 is the highest risk — BridgeServer and its tests must change in lockstep.

## Potential Pitfalls

1. **SerializedMessage → vector\<byte\> memcpy**: One copy per ROS2 message. Negligible vs ZSTD.
2. **Schema lookup by topic_name**: `Ros2TopicSource::get_schema()` must maintain name→type mapping internally.
3. **Thread safety of set_message_callback()**: ROS2 adapter must protect the callback pointer.
4. **spdlog dependency**: Add via FetchContent for ROS2 builds.
5. **Core tests must NOT depend on rclcpp**: Use plain gtest, not `ament_cmake_gtest`, for core tests.

## Verification

```bash
# Build with colcon (ROS2 backend)
cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash
colcon build --packages-select pj_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release

# Run all tests
colcon test --packages-select pj_bridge && colcon test-result --verbose

# TSAN + ASAN (existing workflow via run_and_test.sh)

# Integration test with rosbag
ros2 bag play DATA/sample.mcap --loop &
ros2 run pj_bridge pj_bridge_ros2
python3 tests/integration/test_client.py --subscribe /topic

# RTI build (on machine with Conan + RTI)
conan install . && cmake ... && make
```
