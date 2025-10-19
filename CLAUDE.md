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

**Location**: `DATA/sample.mcap`
**Inspect rosbag**:
```bash
source /opt/ros/humble/setup.bash
ros2 bag info DATA/sample.mcap
```

**Contents** (as of 2025-10-19):
- Duration: ~37 minutes (2212 seconds)
- 1,390,034 messages
- Topics include custom types:
  - `ims_msgs::FulcrumLocation`
  - `ims_msgs::ArmUIState`
  - `ims_msgs::MotorCommandCollection`
  - `ims_msgs::RoboticsInputs`
  - `AsensusMessaging::ArmState`
  - `AsensusMessaging::ArmOutput`

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
   - Uses `rosidl_typesupport_introspection_cpp`
   - Traverses `MessageMembers` structure at runtime
   - Serializes to JSON format

4. **Generic Subscription**
   - `rclcpp::GenericSubscription` for runtime topic subscription
   - Reference counting for shared subscriptions across clients

5. **Message Buffer**
   - Thread-safe circular buffer per topic
   - Stores: topic name, publish time, receive time, serialized CDR data
   - Max size: configurable (default 1000 messages)

6. **Session Manager**
   - Tracks client sessions using ZMQ connection identity
   - Monitors heartbeats (expected every 1 second)
   - Timeout: 10 seconds without heartbeat
   - Manages per-client subscriptions

7. **Message Aggregation**
   - 50 Hz timer collects new messages from all active topics
   - Custom binary serialization format
   - ZSTD compression applied to serialized data
   - Published via ZMQ PUB socket

### Message Serialization Format

Custom binary format for aggregated messages (before compression):
```
- Number of messages (uint32_t)
- For each message:
  - Topic name length (uint16_t)
  - Topic name (N bytes UTF-8)
  - Publish timestamp (uint64_t nanoseconds since epoch)
  - Receive timestamp (uint64_t nanoseconds since epoch)
  - Message data length (uint32_t)
  - Message data (N bytes - CDR serialized)
```

**ZSTD Compression**: After serialization, the aggregated message is compressed using ZSTD before being published via the PUB-SUB socket. This reduces network bandwidth and improves performance for large messages.

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
│   ├── session_manager.hpp
│   ├── generic_subscription_manager.hpp
│   └── bridge_server.hpp
├── src/
│   ├── middleware/
│   │   └── zmq_middleware.cpp
│   ├── topic_discovery.cpp
│   ├── schema_extractor.cpp
│   ├── message_buffer.cpp
│   ├── session_manager.cpp
│   ├── generic_subscription_manager.cpp
│   ├── bridge_server.cpp
│   └── main.cpp
├── tests/
│   ├── unit/
│   │   ├── test_message_buffer.cpp
│   │   ├── test_session_manager.cpp
│   │   ├── test_serialization.cpp
│   │   ├── test_subscription_manager.cpp
│   │   └── test_schema_extractor.cpp
│   └── integration/
│       ├── test_client.py
│       └── test_full_workflow.py
├── 3rdparty/
│   └── (cppzmq headers)
├── DATA/
│   └── sample.mcap
├── CMakeLists.txt
├── package.xml
├── .clang-tidy
├── PROJECT.md
├── IMPLEMENTATION_PLAN.md
├── CLAUDE.md (this file)
└── README.md
```

## Dependencies

### ROS2 Packages
- `rclcpp` - ROS2 C++ client library
- `rosbag2_cpp` - For rosbag utilities
- `rosidl_typesupport_introspection_cpp` - Runtime type introspection
- `rosidl_runtime_cpp` - Runtime type support utilities
- `ament_cmake` - Build system
- `ament_cmake_gtest` - Testing framework

### External Libraries
- **ZeroMQ** (libzmq) - Networking middleware
- **cppzmq** - C++ bindings for ZeroMQ (headers in 3rdparty/)
- **ZSTD** (libzstd) - Compression library (FindZSTD.cmake in cmake/)
- **JSON library** - For schema serialization (nlohmann/json or hand-crafted)

### Python (for test clients)
- `pyzmq` - ZeroMQ Python bindings
- `zstandard` - ZSTD decompression library
- `struct` - Binary serialization (built-in)
- `json` - JSON parsing (built-in)
- `argparse` - CLI parsing (built-in)

## Implementation Status

**Current Milestone**: Planning complete
**Next Steps**: Begin Milestone 1 - Project Setup & Infrastructure

### Milestone Checklist
- [ ] Milestone 1: Project Setup & Infrastructure
- [ ] Milestone 2: Topic Discovery & Schema Extraction
- [ ] Milestone 3: Generic Subscription & Message Buffering
- [ ] Milestone 4: Client Session Management
- [ ] Milestone 5: Message Aggregation & Publishing
- [ ] Milestone 6: Main Server Integration & Configuration
- [ ] Milestone 7: Python Test Client Development
- [ ] Milestone 8: Unit Test Suite
- [ ] Milestone 9: Error Handling & Robustness
- [ ] Milestone 10: Documentation & Polish

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

### 6. Schema via Runtime Introspection
**Decision**: Use `rosidl_typesupport_introspection_cpp` to extract schemas
**Rationale**: No need for compile-time knowledge of message types
**Challenge**: Complex custom types may require special handling
**Fallback**: Use message definition text from type support if introspection insufficient

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
req_port: 5555          # REQ-REP API port
pub_port: 5556          # PUB data stream port
publish_rate: 50.0      # Hz - aggregation publish rate
session_timeout: 10.0   # seconds - client heartbeat timeout
buffer_size: 1000       # messages - max per topic buffer
topic_filter: ""        # regex - filter topics (optional)
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
1. Run clang-tidy checks
2. Run unit tests
3. Test with sample.mcap rosbag
4. Update documentation if needed
5. Update milestone checklist in this file

### Common commands:
```bash
# Build
cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build --packages-select pj_ros_bridge

# Test
colcon test --packages-select pj_ros_bridge && colcon test-result --verbose

# Run
ros2 run pj_ros_bridge bridge_server

# Inspect rosbag
ros2 bag info DATA/sample.mcap

# Play rosbag
ros2 bag play DATA/sample.mcap
```

---

**Last Updated**: 2025-10-19
**Project Phase**: Planning Complete, Ready for Implementation
**Current Focus**: Milestone 1 - Project Setup & Infrastructure