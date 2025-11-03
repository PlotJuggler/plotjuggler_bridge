# CLAUDE.md - Project Context for AI Assistant

## Project Overview

**Project Name**: pj_ros_bridge
**Type**: C++ ROS2 Package (Humble)
**Purpose**: ROS2 bridge server that forwards ROS2 topic content over the network using ZeroMQ, without DDS

**Main Goal**: Enable clients to subscribe to ROS2 topics and receive aggregated messages at 50 Hz without needing a full ROS2/DDS installation.

## Key Documentation Files

- `PROJECT.md` - Original project specification and requirements
- `IMPLEMENTATION_PLAN.md` - Detailed 10-milestone implementation roadmap
- `.clang-tidy` - Coding standards and style guide
- `README.md` - User-facing documentation (to be updated)

## Build Instructions

```bash
# Navigate to workspace root
cd ~/ws_plotjuggler

# Source ROS2 Humble
source /opt/ros/humble/setup.bash

# Build the package
colcon build --packages-select pj_ros_bridge

# Run tests
colcon test --packages-select pj_ros_bridge
colcon test-result --verbose
```

## Test Data

**Location**: `DATA/` directory contains:
- `sample.mcap` - Real rosbag data for testing
- `sensor_msgs-pointcloud2.txt` - Reference schema for PointCloud2
- `sensor_msgs-imu.txt` - Reference schema for IMU
- `pose_with_covariance_stamped.txt` - Reference schema for PoseWithCovarianceStamped

**Inspect rosbag**:
```bash
source /opt/ros/humble/setup.bash
ros2 bag info DATA/sample.mcap
```

**sample.mcap Contents** (as of 2025-10-19):
- Duration: ~37 minutes (2212 seconds)
- 1,390,034 messages
- Topics include custom types:
  - `ims_msgs::FulcrumLocation`
  - `ims_msgs::ArmUIState`
  - `ims_msgs::MotorCommandCollection`
  - `ims_msgs::RoboticsInputs`
  - `AsensusMessaging::ArmState`
  - `AsensusMessaging::ArmOutput`

**Note**: Reference schema files are used for unit test validation to ensure SchemaExtractor produces correct output.

## Coding Standards

### Naming Conventions (from `.clang-tidy`)

**Classes & Types**:
- Classes: `CamelCase` (e.g., `BridgeServer`, `SessionManager`)
- Structs: `CamelCase` with `lower_case` members
- Enums: `CamelCase` with `UPPER_CASE` constants

**Functions & Methods**:
- Functions/Methods: `lower_case` (e.g., `get_topics()`, `update_heartbeat()`)

**Variables**:
- Local/member variables: `lower_case`
- Private members: suffix with `_` (e.g., `sessions_`, `mutex_`)
- Constants: `CamelCase` with `k` prefix (e.g., `kDefaultTimeout`, `kBufferSize`)

**Example**:
```cpp
class SessionManager {
public:
  void create_session(const std::string& client_id);

private:
  static constexpr int kDefaultTimeout = 10;
  std::unordered_map<std::string, Session> sessions_;
  std::mutex mutex_;
};
```

### Code Quality

- **Warnings as Errors**: Most clang-tidy checks are treated as errors
- **Thread Safety**: Explicit from the start - use mutexes and document thread safety
- **ROS2 Compatibility**:
  - Ignores macros in complexity checks (for `RCLCPP_INFO`, etc.)
  - Allows `std::shared_ptr` as value param (for ROS2 callbacks)
- **Comments**: Comprehensive documentation for classes and methods
- **Testing**: Unit tests required for all core components (gtest)
- **Code Formatting**: Use `pre-commit run -a` to format all code before committing
  - Configured in `.pre-commit-config.yaml`
  - Uses clang-format for C++ code
  - Note: uncrustify linter removed from CMakeLists.txt (was failing on 3rdparty libs)

## Architecture Overview

### Communication Pattern

**ZeroMQ Sockets**:
1. **REQ-REP Pattern**: Client API requests (get topics, subscribe, heartbeat)
   - Default port: 5555
2. **PUB-SUB Pattern**: Aggregated message stream at 50 Hz
   - Default port: 5556

### Key Components

1. **Middleware Layer** (Abstract)
   - `MiddlewareInterface` - Abstract base class
   - `ZmqMiddleware` - ZeroMQ implementation using cppzmq
   - Allows future middleware replacement

2. **Topic Discovery**
   - Uses `rclcpp::Node::get_topic_names_and_types()`
   - Filters system topics

3. **Schema Extraction**
   - Uses `ament_index_cpp` to locate .msg files in ROS2 package share directories
   - Reads .msg files directly and recursively expands nested types
   - Uses depth-first traversal to build complete message definitions
   - Reference schema files stored in DATA/ for test validation:
     - `sensor_msgs-pointcloud2.txt`
     - `sensor_msgs-imu.txt`
     - `pose_with_covariance_stamped.txt`

4. **Generic Subscription**
   - `rclcpp::GenericSubscription` for runtime topic subscription
   - Reference counting for shared subscriptions across clients

5. **Message Buffer**
   - Thread-safe buffer per topic with automatic cleanup
   - Zero-copy design using `shared_ptr<SerializedMessage>`
   - Stores: timestamp and shared pointer to serialized message data
   - Auto-deletes messages older than 1 second to prevent unbounded memory growth
   - Cleanup triggered on every message addition
   - Move semantics: `move_messages()` atomically transfers buffer ownership via `std::swap()`

6. **Session Manager**
   - Tracks client sessions using ZMQ connection identity
   - Monitors heartbeats (expected every 1 second)
   - Timeout: 10 seconds without heartbeat
   - Manages per-client subscriptions

7. **Bridge Server** (NEW - Milestone 4)
   - Main orchestrator integrating all components
   - Handles API request/response loop
   - Routes commands (get_topics, subscribe, heartbeat)
   - Manages session timeouts with 1 Hz timer
   - Creates message buffer callbacks for subscriptions

8. **Message Aggregation** (NEW - Milestone 5)
   - 50 Hz timer collects new messages from all active topics
   - Custom binary serialization format (little-endian)
   - ZSTD compression applied to serialized data
   - Published via ZMQ PUB socket
   - Statistics tracking (total messages/bytes published)

### Message Serialization Format

**Streaming Binary Format** (before compression):
- No header or message count placeholder
- Messages are serialized directly in sequence:
```
For each message:
  - Topic name length (uint16_t little-endian)
  - Topic name (N bytes UTF-8)
  - Publish timestamp (uint64_t nanoseconds since epoch, little-endian)
  - Receive timestamp (uint64_t nanoseconds since epoch, little-endian)
  - Message data length (int32_t little-endian)
  - Message data (N bytes - CDR serialized from ROS2)
```

**Design Philosophy**:
- **Zero-copy**: Messages pass via `shared_ptr<SerializedMessage>` to avoid data copying
- **Streaming**: Messages serialized immediately to output buffer (no intermediate storage)
- **Move semantics**: Buffer ownership transferred atomically via `std::swap()`
- **ZSTD Compression**: Final buffer compressed (level 1) before publishing via ZMQ PUB socket

### API Protocol

**Get Topics Request**:
```json
{
  "command": "get_topics"
}
```

**Get Topics Response**:
```json
{
  "topics": [
    {"name": "/topic_name", "type": "package_name/msg/MessageType"},
    ...
  ]
}
```

**Subscribe Request**:
```json
{
  "command": "subscribe",
  "topics": ["/topic1", "/topic2"]
}
```

**Subscribe Response**:
```json
{
  "status": "success",
  "schemas": {
    "/topic1": { /* schema JSON */ },
    "/topic2": { /* schema JSON */ }
  }
}
```

**Heartbeat Request**:
```json
{
  "command": "heartbeat"
}
```

**Heartbeat Response**:
```json
{
  "status": "ok"
}
```

**Error Response**:
```json
{
  "status": "error",
  "error_code": "ERROR_CODE",
  "message": "Human readable error message"
}
```

## Project Structure

```
pj_ros_bridge/
├── include/pj_ros_bridge/
│   ├── middleware/
│   │   ├── middleware_interface.hpp
│   │   └── zmq_middleware.hpp
│   ├── topic_discovery.hpp
│   ├── schema_extractor.hpp
│   ├── message_buffer.hpp
│   ├── session_manager.hpp          [✓ Milestone 4]
│   ├── generic_subscription_manager.hpp
│   └── bridge_server.hpp             [✓ Milestone 4]
├── src/
│   ├── middleware/
│   │   └── zmq_middleware.cpp
│   ├── topic_discovery.cpp
│   ├── schema_extractor.cpp
│   ├── message_buffer.cpp
│   ├── session_manager.cpp           [✓ Milestone 4]
│   ├── generic_subscription_manager.cpp
│   ├── bridge_server.cpp             [✓ Milestone 4]
│   └── main.cpp                      [✓ Milestone 6]
├── tests/
│   ├── unit/
│   │   ├── test_middleware.cpp
│   │   ├── test_topic_discovery.cpp
│   │   ├── test_schema_extractor.cpp
│   │   ├── test_message_buffer.cpp
│   │   ├── test_generic_subscription_manager.cpp
│   │   └── test_session_manager.cpp  [✓ Milestone 4]
│   └── integration/
│       └── test_client.py            [✓ Milestone 7]
├── 3rdparty/
│   ├── cppzmq/ (ZeroMQ C++ headers)
│   └── nlohmann/ (JSON library header)
├── DATA/
│   ├── sample.mcap
│   ├── sensor_msgs-pointcloud2.txt (reference schema)
│   ├── sensor_msgs-imu.txt (reference schema)
│   └── pose_with_covariance_stamped.txt (reference schema)
├── cmake/
│   └── FindZSTD.cmake
├── CMakeLists.txt
├── package.xml
├── .clang-tidy
├── .pre-commit-config.yaml
├── PROJECT.md
├── IMPLEMENTATION_PLAN.md
├── CLAUDE.md (this file)
└── README.md
```

## Dependencies

### ROS2 Packages (Runtime)
- `rclcpp` - ROS2 C++ client library
- `ament_index_cpp` - Locate ROS2 package share directories
- `ament_cmake` - Build system (buildtool)

### ROS2 Packages (Test Only)
- `ament_cmake_gtest` - Testing framework
- `sensor_msgs` - Standard sensor message types for unit tests
- `geometry_msgs` - Standard geometry message types for unit tests

### External Libraries
- **ZeroMQ** (libzmq) - Networking middleware (system package)
- **cppzmq** - C++ bindings for ZeroMQ (header-only, in 3rdparty/)
- **ZSTD** (libzstd) - Compression library (system package, FindZSTD.cmake in cmake/)
- **nlohmann/json** - JSON library (header-only, in 3rdparty/)

### Python (for test clients)
- `pyzmq` - ZeroMQ Python bindings
- `zstandard` - ZSTD decompression library
- `struct` - Binary serialization (built-in)
- `json` - JSON parsing (built-in)
- `argparse` - CLI parsing (built-in)

## Implementation Status

**Current Milestone**: Milestone 9 substantially completed
**Next Steps**: Milestone 10 - Documentation & Polish

### Milestone Checklist
- [x] Milestone 1: Project Setup & Infrastructure (completed 2025-10-19)
- [x] Milestone 2: Topic Discovery & Schema Extraction (completed 2025-10-19)
- [x] Milestone 3: Generic Subscription & Message Buffering (completed 2025-10-19)
- [x] Milestone 4: Client Session Management (completed 2025-10-21)
- [x] Milestone 5: Message Aggregation & Publishing (completed 2025-10-21)
- [x] Milestone 6: Main Server Integration & Configuration (completed 2025-10-21)
- [x] Milestone 7: Python Test Client Development (completed 2025-10-21)
- [x] Milestone 8: Unit Test Suite (completed 2025-11-03)
- [x] Milestone 9: Error Handling & Robustness (substantially completed 2025-11-03)
- [ ] Milestone 10: Documentation & Polish

### Completed Components

**Milestone 1** (branch: claude/milestone_1):
- MiddlewareInterface abstract class
- ZmqMiddleware implementation (REP/PUB sockets)
- CMakeLists.txt and package.xml setup
- Unit tests: 9 tests passing

**Milestone 2** (branch: claude/milestone_2):
- TopicDiscovery class (discovers topics, filters system topics)
- SchemaExtractor class (runtime introspection with dlopen)
- nlohmann/json integration
- Unit tests: 18 total tests passing

**Milestone 3** (branch: claude/milestone_3):
- MessageBuffer class with 1-second auto-cleanup
- GenericSubscriptionManager with reference counting
- Thread-safe operations
- SchemaExtractor uses depth-first traversal for nested message definitions
- Reference schema files in DATA/ for test validation
- Unit tests: 34 total tests passing (all green)

**Milestone 4** (milestone_4 branch):
- SessionManager class with client session tracking
- Session timeout monitoring (10 second default timeout)
- Heartbeat management
- Per-client subscription tracking
- BridgeServer class integrating all components
- API request handlers (get_topics, subscribe, heartbeat)
- Session cleanup on timeout
- Unit tests: 44 total tests passing (all green)

**Milestone 5** (milestone_5 branch):
- AggregatedMessageSerializer class with custom binary format
- Little-endian serialization (uint16_t, uint32_t, uint64_t)
- ZSTD compression/decompression wrappers
- 50 Hz publisher timer in BridgeServer
- Automatic message aggregation and publishing
- Publish statistics tracking (messages, bytes)
- Unit tests: 55 total tests passing (11 new serializer tests)

**Milestone 6** (milestone_6 branch):
- main.cpp entry point with full integration
- ROS2 parameter configuration (req_port, pub_port, publish_rate, session_timeout)
- Signal handlers for graceful shutdown (SIGINT, SIGTERM)
- Main processing loop (100 Hz for API requests + ROS2 spin)
- Executable creation and installation (pj_ros_bridge_node)
- Configuration logging and final statistics display
- Complete server lifecycle management

**Milestone 7** (milestone_7 branch):
- Python test client (tests/integration/test_client.py)
- ZeroMQ integration (REQ-REP + PUB-SUB sockets)
- ZSTD decompression using zstandard library
- Binary message deserialization matching C++ format
- Background heartbeat thread (1 second interval)
- Command-line interface with argparse (get_topics, subscribe commands)
- Statistics tracking (messages, bytes, rate per topic)
- Latency calculation and display
- Tested successfully with multiple topics at different rates

## Important Design Decisions

### 1. Middleware Abstraction
**Decision**: Use abstract `MiddlewareInterface` class
**Rationale**: Allow future replacement of ZeroMQ if needed
**Impact**: Slight overhead, but provides flexibility

### 2. Session Identity via ZMQ
**Decision**: Use ZeroMQ connection identity (ZMQ_IDENTITY) for session tracking
**Rationale**: Avoids requiring clients to manage UUIDs
**Fallback**: If unreliable, require client-generated UUID in connection message

### 3. Shared Subscriptions with Reference Counting
**Decision**: Single ROS2 subscription per topic, shared across clients
**Rationale**: Reduces resource usage, improves scalability
**Impact**: Requires careful reference counting and thread safety

### 4. Custom Binary Serialization
**Decision**: Hand-craft aggregated message serialization
**Rationale**: Simple format, no external dependencies, minimal overhead
**Impact**: Must handle endianness and test cross-platform compatibility

### 5. 50 Hz Publish Rate
**Decision**: Fixed timer at 50 Hz for aggregated messages
**Rationale**: Balances latency vs. network overhead
**Configuration**: Make configurable for different use cases

### 6. Schema Extraction via .msg Files
**Decision**: Read .msg files directly from ROS2 package share directories instead of runtime introspection
**Rationale**: Simpler implementation, more reliable for complex types, matches rosbag2 approach
**Implementation**: Use `ament_index_cpp` to locate packages, recursively expand nested types with depth-first traversal
**Impact**: Requires ROS2 packages to be installed; produces identical schemas to rosbag2 MCAP storage

## Key References

### ROS2 Documentation
- GenericSubscription: https://api.nav2.org/rolling/html/generic__subscription_8hpp_source.html
- Type Introspection: https://github.com/ros2/rosidl/tree/master/rosidl_typesupport_introspection_cpp

### Schema Extraction Examples
- rosbag2 MCAP storage: https://github.com/ros2/rosbag2/blob/rolling/rosbag2_storage_mcap/src/mcap_storage.cpp
- Message definition access: Use `message.__class__._full_text` in Python

### ZeroMQ
- cppzmq GitHub: https://github.com/zeromq/cppzmq
- REQ-REP pattern: https://zeromq.org/socket-api/#request-reply-pattern
- PUB-SUB pattern: https://zeromq.org/socket-api/#publish-subscribe-pattern
- Identity routing: ZMQ_IDENTITY socket option

## Testing Strategy

### Unit Tests (gtest)
- Component isolation
- Thread safety verification
- Edge case handling
- Mock middleware for testing without ZeroMQ

### Integration Tests (Python)
- Full workflow testing with real rosbag data
- Multi-client scenarios
- Session timeout verification
- Performance benchmarking

### Manual Testing Workflow
```bash
# Terminal 1: Play rosbag
source /opt/ros/humble/setup.bash
ros2 bag play DATA/sample.mcap --loop

# Terminal 2: Run server
source /opt/ros/humble/setup.bash
ros2 run pj_ros_bridge bridge_server --ros-args --log-level debug

# Terminal 3: Run Python test client
python3 tests/integration/test_client.py --subscribe <topics>
```

## Configuration Parameters

Default values (to be implemented):
```yaml
req_port: 5555              # REQ-REP API port
pub_port: 5556              # PUB data stream port
publish_rate: 50.0          # Hz - aggregation publish rate
session_timeout: 10.0       # seconds - client heartbeat timeout
message_retention: 1.0      # seconds - max age of buffered messages
topic_filter: ""            # regex - filter topics (optional)
```

## Known Challenges & Solutions

### 1. Schema Extraction for Custom Types
**Challenge**: Custom message types from sample.mcap may not expose schema correctly
**Approach**: Test incrementally, start with standard messages
**Monitor**: Test with `ims_msgs` and `AsensusMessaging` types early

### 2. Thread Safety
**Challenge**: Concurrent access to buffers and sessions
**Approach**: Use explicit mutexes from the start, write thread safety tests
**Tool**: Use thread sanitizer during testing

### 3. Performance at High Message Rates
**Challenge**: sample.mcap has 463,156 messages of some types
**Approach**: Profile early, optimize serialization and buffer operations
**Fallback**: Make publish rate configurable, implement message dropping if needed

### 4. CDR Serialization Portability
**Challenge**: Serialized messages may have endianness issues
**Approach**: Document format assumptions, test cross-platform
**Mitigation**: Add endianness indicator to message header if needed

## Success Criteria

Project complete when:
- ✅ Server successfully bridges all topics from sample.mcap
- ✅ Multiple Python clients can connect simultaneously
- ✅ Aggregated messages published consistently at 50 Hz
- ✅ Session management handles connects/disconnects/timeouts correctly
- ✅ All unit tests pass (>80% code coverage)
- ✅ Integration tests pass with real rosbag data
- ✅ No memory leaks or crashes during 1-hour stress test
- ✅ Documentation is complete and clear
- ✅ Performance requirements met:
  - ≥10 concurrent clients
  - ≥1000 messages/second throughput
  - <100ms latency (receive_time - publish_time)

## Notes for Future Sessions

### When resuming work:
1. Check current milestone progress in IMPLEMENTATION_PLAN.md
2. Review any recent code changes
3. Ensure build environment is properly sourced
4. Run existing tests to verify baseline
5. Follow coding standards in .clang-tidy

### Before committing code:
1. Run `pre-commit run -a` to format all code
2. Run unit tests: `colcon test --packages-select pj_ros_bridge`
3. Test with sample.mcap rosbag (if applicable)
4. Update documentation if needed
5. Update milestone checklist in this file

### Common commands:
```bash
# Build
cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge

# Format code (before committing)
pre-commit run -a

# Test
colcon test --packages-select pj_ros_bridge && colcon test-result --all

# Run server (default configuration)
ros2 run pj_ros_bridge pj_ros_bridge_node

# Run server with custom parameters
ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args \
  -p req_port:=5557 \
  -p pub_port:=5558 \
  -p publish_rate:=30.0 \
  -p session_timeout:=15.0

# Inspect rosbag
ros2 bag info DATA/sample.mcap

# Play rosbag
ros2 bag play DATA/sample.mcap
```

---

**Last Updated**: 2025-11-03
**Project Phase**: Active Implementation - API Refactor Complete
**Current Focus**: Zero-copy, move-based API implementation
**Test Status**: 59 unit tests passing (9 middleware, 4 discovery, 3 schema, 10 buffer, 10 subscription, 10 session, 13 serializer)
**API Changes**:
- MessageBuffer: Zero-copy using `shared_ptr<SerializedMessage>`, move semantics via `move_messages()`
- Serializer: Streaming API, no placeholder header, out-parameter compression
**Integration Test**: Python test client fully functional and tested
**Linter Status**: All linters passing (cppcheck, lint_cmake, xmllint; uncrustify removed)
**Executable**: pj_ros_bridge_node ready to run
