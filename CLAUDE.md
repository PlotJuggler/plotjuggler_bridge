# CLAUDE.md - Project Context for AI Assistant

## Project Overview

**Project Name**: pj_bridge
**Type**: Multi-backend C++ bridge server (ROS2 / RTI Connext DDS / eProsima Fast DDS)
**Purpose**: Forward middleware topic content over WebSocket to PlotJuggler clients

**Main Goal**: Enable clients to subscribe to topics and receive aggregated messages at configurable rates without needing a full middleware installation. Three backends share a common core library:
- **ROS2 backend** (`pj_bridge_ros2`) — ROS2 Humble, uses `rclcpp`
- **RTI backend** (`pj_bridge_rti`) — RTI Connext DDS, uses `rti::connext` (build disabled, code preserved)
- **FastDDS backend** (`pj_bridge_fastdds`) — eProsima Fast DDS 3.4, via Conan

## Key Documentation Files

- `docs/API.md` - API protocol documentation
- `.clang-tidy` - Coding standards and style guide

## Build Instructions

### ROS2 Backend (colcon)
```bash
cd ~/ws_plotjuggler
source /opt/ros/humble/setup.bash
colcon build --packages-select pj_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release
colcon test --packages-select pj_bridge && colcon test-result --verbose
```

### RTI Backend (standalone CMake — currently disabled)
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_RTI=ON
make -j$(nproc)
```

### FastDDS Backend (Conan + standalone CMake)
```bash
cd ~/ws_plotjuggler/src/pj_ros_bridge
conan install . --output-folder=build_fastdds --build=missing -s build_type=Release
cd build_fastdds
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_FASTDDS=ON \
         -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake
make -j$(nproc)
```

### External Dependencies

- **IXWebSocket** — `find_package` first, FetchContent fallback
- **spdlog** — system package preferred (for ROS2 ABI compatibility with `librcl_logging_spdlog`); FetchContent fallback for standalone builds
- **ZSTD** — system package (`libzstd-dev`)
- **CLI11** — FetchContent (RTI backend only)

### FastDDS Backend (Conan-managed)
- **eProsima Fast DDS 3.4.0** — via Conan (`fast-dds/3.4.0`)
- **eProsima Fast CDR 2.x** — transitive dependency via Conan
- **CLI11** — FetchContent (for CLI parsing, shared with RTI)

**Important**: Do NOT use FetchContent for spdlog when building with ROS2. The system spdlog must match the version used by `librcl_logging_spdlog.so` to avoid ABI conflicts (symbol collision causes "free(): invalid pointer" crash during `rclcpp::init()`).

## Coding Standards

### Naming Conventions (from `.clang-tidy`)

- **Classes/Structs**: `CamelCase` (e.g., `BridgeServer`, `BufferedMessage`)
- **Functions/Methods**: `lower_case` (e.g., `get_topics()`, `add_message()`)
- **Local/member variables**: `lower_case`, private members suffix with `_`
- **Constants**: `k` prefix + `CamelCase` (e.g., `kDefaultMaxMessageAgeNs`)
- **Namespaces**: `pj_bridge` (core), `pj_bridge` with ROS2/RTI-specific types

### Code Quality

- **Thread Safety**: Use mutexes, document thread safety in class comments
- **Testing**: Unit tests required for all core components (gtest)
- **Code Formatting**: `pre-commit run -a` before committing (clang-format)
- **Logging**: Use `spdlog::info/warn/error/debug()` — NOT `RCLCPP_*` macros (except in ros2/ adapter code)

## Architecture Overview

### Multi-Backend Design

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

### Abstract Interfaces (in `app/include/pj_bridge/`)

**TopicSourceInterface**: `get_topics()`, `get_schema(topic_name)`, `schema_encoding()`
**SubscriptionManagerInterface**: `set_message_callback()`, `subscribe(name, type)`, `unsubscribe()`, `unsubscribe_all()`
**MiddlewareInterface**: WebSocket text/binary frame API

### Event Loop

BridgeServer does NOT own timers. The entry point (`main.cpp`) drives the event loop:
- **ROS2**: `rclcpp` wall timers call `process_requests()`, `publish_aggregated_messages()`, `check_session_timeouts()`
- **RTI**: `std::chrono` loop with `std::this_thread::sleep_for()`
- **FastDDS**: `std::chrono` loop with `std::this_thread::sleep_for()` (same pattern as RTI)

### Key Components

1. **BridgeServer** (`app/`) — Main orchestrator. Takes interfaces via constructor injection. Uses spdlog for logging. Improved lock ordering: build frames under lock, send outside lock.

2. **MessageBuffer** (`app/`) — Thread-safe per-topic buffer. Uses `shared_ptr<vector<byte>>` (not `rclcpp::SerializedMessage`). TTL cleanup based on `received_at_ns` (wall-clock time when added).

3. **AggregatedMessageSerializer** (`app/`) — Serializes `(topic, timestamp, byte*, size)`. Backend-agnostic (no rclcpp dependency).

4. **SessionManager** (`app/`) — Tracks clients, heartbeats, per-client subscriptions.

5. **WebSocketMiddleware** (`app/`) — IXWebSocket implementation. Improved shutdown: shared_ptr server, timeout thread, pre-close clients.

6. **Ros2TopicSource** (`ros2/`) — Wraps `TopicDiscovery` + `SchemaExtractor`. Schema encoding: `"ros2msg"`.

7. **Ros2SubscriptionManager** (`ros2/`) — Wraps `GenericSubscriptionManager` + optional `MessageStripper`. Converts `rclcpp::SerializedMessage` → `shared_ptr<vector<byte>>` via memcpy.

8. **RtiTopicSource** (`rti/`) — Wraps `DdsTopicDiscovery`. Schema encoding: `"omgidl"`. (Build disabled)

9. **RtiSubscriptionManager** (`rti/`) — Wraps `DdsSubscriptionManager`. DDS already produces `shared_ptr<vector<byte>>`. (Build disabled)

10. **FastDdsTopicSource** (`fastdds/`) — Directly implements `TopicSourceInterface`. Discovers topics via `on_data_writer_discovery()`, resolves `DynamicType` from `TypeObjectRegistry`, generates IDL via `idl_serialize()`. Schema encoding: `"omgidl"`.

11. **FastDdsSubscriptionManager** (`fastdds/`) — Directly implements `SubscriptionManagerInterface`. Creates `DataReader`s with `DynamicPubSubType`, deserializes into `DynamicData` and re-serializes to extract CDR bytes.

### Communication Pattern

**WebSocket** (single port, default 9090):
- **Text frames**: JSON API commands and responses (get_topics, subscribe, heartbeat, pause, resume, unsubscribe)
- **Binary frames**: ZSTD-compressed aggregated message stream

### Message Serialization Format

```
For each message (streamed, no header):
  - Topic name length (uint16_t LE)
  - Topic name (N bytes UTF-8)
  - Timestamp (uint64_t ns since epoch, LE)
  - Message data length (uint32_t LE)
  - Message data (N bytes CDR)
```

## CMake Targets

| Target | Type | Description |
|--------|------|-------------|
| `pj_bridge_app` | STATIC | Core library (no ROS2/DDS deps) |
| `pj_bridge_ros2_lib` | STATIC | ROS2 adapter library |
| `pj_bridge_ros2` | EXECUTABLE | ROS2 entry point |
| `pj_bridge_rti_lib` | STATIC | RTI adapter library (disabled) |
| `pj_bridge_rti` | EXECUTABLE | RTI entry point (disabled) |
| `pj_bridge_fastdds_lib` | STATIC | FastDDS adapter library (if `ENABLE_FASTDDS=ON`) |
| `pj_bridge_fastdds` | EXECUTABLE | FastDDS entry point |
| `pj_bridge_tests` | EXECUTABLE | All unit tests |

## Dependencies

### Core (always required)
- **ZSTD** — compression (`libzstd-dev`)
- **spdlog** — logging (system package or FetchContent)
- **IXWebSocket** — WebSocket (`find_package` first, FetchContent fallback)
- **nlohmann/json** — JSON (`find_package(REQUIRED)`)
- **tl::expected** — error handling (header-only, vendored in 3rdparty/)

### ROS2 Backend
- `rclcpp`, `ament_index_cpp`, `ament_cmake`
- `sensor_msgs`, `nav_msgs` (for message stripper)
- `ament_cmake_gtest` (test only)

### RTI Backend (disabled)
- RTI Connext DDS (`RTIConnextDDS::cpp2_api`)
- CLI11 (FetchContent, for CLI parsing)

### FastDDS Backend
- eProsima Fast DDS 3.4.0 (Conan: `fast-dds/3.4.0`)
- eProsima Fast CDR 2.x (transitive Conan dependency)
- CLI11 (FetchContent, for CLI parsing)

## Testing

### Test Count: 154 unit tests across 10 test suites

### Commands
```bash
# Regular build + test
cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash
colcon build --packages-select pj_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release
colcon test --packages-select pj_bridge && colcon test-result --verbose

# TSAN
colcon build --packages-select pj_bridge --build-base build_tsan --install-base install_tsan --cmake-args -DCMAKE_BUILD_TYPE=Release -DENABLE_TSAN=ON
source install_tsan/setup.bash
TSAN_OPTIONS="suppressions=src/pj_ros_bridge/tsan_suppressions.txt" setarch $(uname -m) -R build_tsan/pj_bridge/pj_bridge_tests

# ASAN
colcon build --packages-select pj_bridge --build-base build_asan --install-base install_asan --cmake-args -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=ON
source install_asan/setup.bash
ASAN_OPTIONS="new_delete_type_mismatch=0" LSAN_OPTIONS="suppressions=src/pj_ros_bridge/asan_suppressions.txt" build_asan/pj_bridge/pj_bridge_tests

# Format code (before committing)
pre-commit run -a
```

### Notes
- Sanitizer binaries MUST be run with ROS2 sourced (tests need `AMENT_PREFIX_PATH`)
- TSAN exits with code 66 due to pre-existing IXWebSocket warnings (suppressed) — this is normal
- Convenience script available: `./run_and_test.sh`

## Configuration

### ROS2 (via `--ros-args -p`):
```yaml
port: 9090                  # WebSocket port
publish_rate: 50.0          # Hz
session_timeout: 10.0       # seconds
strip_large_messages: true  # Strip Image/PointCloud2/etc data fields
```

### RTI (via CLI flags):
```bash
pj_bridge_rti --domains 0 1 --port 9090 --publish-rate 50 --session-timeout 10
```

### FastDDS (via CLI flags):
```bash
pj_bridge_fastdds --domains 0 1 --port 9090 --publish-rate 50 --session-timeout 10
```

## Important Design Decisions

1. **Backend-agnostic core via interfaces**: `TopicSourceInterface` and `SubscriptionManagerInterface` allow the same `BridgeServer` to work with ROS2 or RTI DDS.

2. **spdlog for logging**: Replaced `RCLCPP_*` macros in the core. System spdlog is required for ROS2 builds (ABI compatibility). FetchContent fallback for standalone builds.

3. **Event loop externalized**: `BridgeServer` exposes `process_requests()`, `publish_aggregated_messages()`, `check_session_timeouts()` as public methods. The entry point drives timing.

4. **Message data as `shared_ptr<vector<byte>>`**: Backend-agnostic type. ROS2 adapter copies from `SerializedMessage`; RTI adapter already produces this type natively.

5. **TTL cleanup by received_at_ns**: MessageBuffer cleanup uses wall-clock reception time, not the message timestamp (which could be from sim time).

6. **Improved lock ordering**: Build serialized frames under `last_sent_mutex_`, send outside lock (adopted from DDS bridge to minimize lock contention).

---

**Last Updated**: 2026-02-26
**Project Phase**: Unified multi-backend architecture
**Test Status**: 154 unit tests passing (all sanitizers clean)
**Executables**: `pj_bridge_ros2` (ROS2), `pj_bridge_rti` (RTI DDS, disabled), `pj_bridge_fastdds` (FastDDS)
