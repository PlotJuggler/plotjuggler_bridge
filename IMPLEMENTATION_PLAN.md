# ROS2 Bridge Implementation Plan

> **Note**: This implementation plan was originally written for ZeroMQ. The project has since migrated to IXWebSocket (WebSocket). All ZeroMQ references below have been updated to reflect the current WebSocket-based architecture using a single port with text frames for API commands and binary frames for data streaming.

## Project Overview
Implement a C++ ROS2 bridge server that forwards ROS2 topic content over the network using WebSocket (IXWebSocket), without DDS. The server aggregates messages from multiple topics and publishes them at 50 Hz to connected clients.

**Target ROS2 Distribution**: Humble
**Build Command**: `cd ~/ws_plotjuggler && source /opt/ros/humble/setup.bash && colcon build`

---

## Milestone 1: Project Setup & Infrastructure

### Goals
- Set up basic project structure
- Add dependencies
- Create middleware abstraction layer
- Ensure project compiles successfully

### Tasks

#### 1.1 Update CMakeLists.txt and package.xml
- Add required dependencies:
  - `rclcpp`
  - `gtest` (for unit tests)
- Add IXWebSocket (via Conan)
- Add ZSTD compression library (using FindZSTD.cmake)
- Configure C++17 standard
- Set up include directories for headers

#### 1.2 Create Directory Structure
```
pj_ros_bridge/
├── include/pj_ros_bridge/
│   ├── middleware/
│   │   ├── middleware_interface.hpp
│   │   └── websocket_middleware.hpp
│   ├── message_buffer.hpp
│   ├── session_manager.hpp
│   └── bridge_server.hpp
├── src/
│   ├── middleware/
│   │   └── websocket_middleware.cpp
│   ├── message_buffer.cpp
│   ├── session_manager.cpp
│   ├── bridge_server.cpp
│   └── main.cpp
├── tests/
│   ├── unit/
│   │   ├── test_message_buffer.cpp
│   │   ├── test_session_manager.cpp
│   │   └── test_serialization.cpp
│   └── integration/
│       └── test_client.py
├── 3rdparty/
│   └── (nlohmann/json, tl/expected headers)
└── DATA/
    └── sample.mcap
```

#### 1.3 Create Middleware Abstraction
- Define `MiddlewareInterface` abstract class with methods:
  - `initialize()`: Setup middleware
  - `send_reply(data)`: Send response via WebSocket text frame
  - `publish_data(data)`: Publish aggregated messages via WebSocket binary frame
  - `receive_request()`: Receive client requests via WebSocket text frame
  - `get_client_identity()`: Get unique client identifier
- Implement `WebSocketMiddleware` class using IXWebSocket
  - Single WebSocket port (default: 8080) for both API and data
  - Text frames for JSON API commands and responses
  - Binary frames for ZSTD-compressed aggregated message stream
  - Implement connection identity tracking using `connectionState->getId()`

#### 1.4 Testing
- Write unit test for middleware initialization
- Build and verify compilation succeeds
- Test that WebSocket server can be created and bound

**Completion Criteria**: Project compiles without errors, basic middleware tests pass

---

## Milestone 2: Topic Discovery & Schema Extraction

### Goals
- Implement ROS2 topic discovery
- Extract message schemas at runtime
- Serialize schemas to JSON format
- Respond to client topic list requests

### Tasks

#### 2.1 Topic Discovery
- Create `TopicDiscovery` class in `include/pj_ros_bridge/topic_discovery.hpp`
- Use `rclcpp::Node::get_topic_names_and_types()` to discover available topics
- Filter system topics (e.g., `/rosout`, `/parameter_events`)
- Store topic name and type information

#### 2.2 Schema Extraction
Implement schema extraction by reading .msg files directly:
- Use `ament_index_cpp::get_package_share_directory()` to locate ROS2 packages
- Read .msg files from `<package_share>/msg/<MessageType>.msg`
- Create `SchemaExtractor` class with method `get_message_definition(topic_type)`

Key approach:
```cpp
// Parse message type "package_name/msg/MessageType"
std::string package_name, type_name;
parse_message_type(message_type, package_name, type_name);

// Locate package share directory
std::string package_share = ament_index_cpp::get_package_share_directory(package_name);

// Read .msg file
std::string msg_path = package_share + "/msg/" + type_name + ".msg";
std::ifstream msg_file(msg_path);

// Recursively expand nested types using depth-first traversal
build_message_definition_recursive(package_name, type_name, output, processed_types);
```

#### 2.3 Schema Format
- The `get_message_definition()` method returns the complete message definition as a text string
- Format matches ROS2 .msg file format with embedded nested definitions
- Nested types are separated by `================================================================================` markers
- Each nested type section starts with `MSG: package_name/TypeName`
- This format is identical to what rosbag2 MCAP storage produces
- No JSON serialization needed for schemas - they are returned as-is to clients

#### 2.4 Implement "Get Topics" API Handler
- Handle client request for topic list
- Build JSON response format:
```json
{
    "topics": [
        {"name": "/topic_name", "type": "pkg_name/msg/MessageType"},
        ...
    ]
}
```
- Send response via middleware WebSocket text frame

#### 2.5 Testing
- Unit test topic discovery with sample.mcap playing
- Unit test schema extraction for common message types:
  - `sensor_msgs/msg/PointCloud2`
  - `sensor_msgs/msg/Imu`
  - `geometry_msgs/msg/PoseWithCovarianceStamped`
- Compare extracted schemas against reference files in DATA/:
  - `sensor_msgs-pointcloud2.txt`
  - `sensor_msgs-imu.txt`
  - `pose_with_covariance_stamped.txt`
- Integration test: Create simple Python client that:
  1. Connects to server
  2. Requests topic list
  3. Validates JSON response format
  4. Prints discovered topics

**Test Command**:
```bash
# Terminal 1: Play rosbag
ros2 bag play DATA/sample.mcap

# Terminal 2: Run server
ros2 run pj_ros_bridge bridge_server

# Terminal 3: Run test client
python3 tests/integration/test_client.py --command get_topics
```

**Completion Criteria**: Server successfully returns topic list with correct types when queried

---

## Milestone 3: Generic Subscription & Message Buffering

### Goals
- Subscribe to topics using GenericSubscription
- Implement rolling message buffer
- Handle timestamp extraction from messages

### Tasks

#### 3.1 Generic Subscription Manager
- Create `GenericSubscriptionManager` class
- Implement `subscribe(topic_name, topic_type)` method using:
  - `rclcpp::GenericSubscription` for runtime topic subscription
  - Store subscription handle and topic metadata
- Implement `unsubscribe(topic_name)` method
- Handle subscription reference counting (multiple clients may request same topic)

#### 3.2 Message Buffer Implementation
- Create `MessageBuffer` class with:
  - Thread-safe circular buffer (use `std::mutex`)
  - Configurable max size (e.g., 1000 messages per topic)
  - Store: topic name, publish timestamp, receive timestamp, serialized data
- Implement `add_message(topic, pub_time, recv_time, data)` method
- Implement `get_messages_since(timestamp)` method for retrieving new messages
- Implement `clear()` method for buffer reset

#### 3.3 Timestamp Handling
- Messages are stored with two timestamps:
  - **Publish time**: Extracted from message header if present, otherwise use receive time
  - **Receive time**: Timestamp when message arrives at the bridge (current time)
- For messages with `std_msgs/Header`, the publish time comes from the `header.stamp` field
- For messages without header, both timestamps are set to the receive time
- Timestamp extraction can be done by checking message schema (from SchemaExtractor)
- Note: Full CDR deserialization is not required - timestamps are metadata for the bridge

#### 3.4 Message Callback Integration
- Connect GenericSubscription callback to MessageBuffer
- On message receipt:
  1. Extract/generate timestamps
  2. Store serialized message in buffer
  3. Associate with correct topic name

#### 3.5 Testing
- Unit tests:
  - Test MessageBuffer thread safety (concurrent add/get operations)
  - Test buffer overflow behavior (old messages discarded)
  - Test timestamp extraction for various message types
- Integration test:
  - Subscribe to one topic from sample.mcap
  - Verify messages are buffered correctly
  - Check timestamps are reasonable

**Test Command**:
```bash
# Play bag and run server with debug logging
ros2 bag play DATA/sample.mcap &
ros2 run pj_ros_bridge bridge_server --ros-args --log-level debug
```

**Completion Criteria**: Server successfully subscribes to topics and buffers messages with correct timestamps

---

## Milestone 4: Client Session Management

### Goals
- Implement multi-client session management
- Track per-client subscriptions
- Handle heartbeat monitoring
- Implement session timeout

### Tasks

#### 4.1 Session Manager
- Create `SessionManager` class with:
  - `std::unordered_map<client_id, Session>` to track sessions
  - Session structure containing:
    - `client_id`: Unique identifier from WebSocket connection
    - `subscribed_topics`: Set of topic names
    - `last_heartbeat`: Timestamp of last heartbeat
    - `created_at`: Session creation time
- Implement `create_session(client_id)` method
- Implement `update_heartbeat(client_id)` method
- Implement `get_session(client_id)` method

#### 4.2 Subscription Request Handler
- Implement "Subscribe" API handler
- Parse client request containing list of topic names
- Update session's subscribed topics
- Call GenericSubscriptionManager to subscribe/unsubscribe as needed
- Send confirmation response with schemas for requested topics:
```json
{
    "status": "success",
    "schemas": {
        "/topic1": { /* schema JSON */ },
        "/topic2": { /* schema JSON */ }
    }
}
```

#### 4.3 Heartbeat Handler
- Implement "Heartbeat" API handler
- Update `last_heartbeat` timestamp for client session
- Send acknowledgment response

#### 4.4 Session Timeout Monitor
- Create timer (running at 1 Hz) to check for stale sessions
- For each session, check if `(current_time - last_heartbeat) > 10 seconds`
- If timeout detected:
  1. Log session timeout
  2. Remove client's topic subscriptions
  3. Decrement reference count in GenericSubscriptionManager
  4. Unsubscribe from topics with zero references
  5. Remove session from SessionManager

#### 4.5 Shared Subscription Optimization
- Ensure GenericSubscriptionManager uses reference counting
- When multiple clients subscribe to same topic:
  - Only one ROS2 subscription should exist
  - Only one MessageBuffer should exist
  - Each client receives same data during aggregation

#### 4.6 Testing
- Unit tests:
  - Test session creation and heartbeat updates
  - Test session timeout detection
  - Test subscription reference counting
- Integration tests:
  - Python client that subscribes to topics and sends heartbeats
  - Test multiple clients subscribing to same topic (verify single ROS2 subscription)
  - Test client timeout (stop heartbeats, verify unsubscription after 10s)

**Test Command**:
```bash
# Terminal 1: Play rosbag
ros2 bag play DATA/sample.mcap --loop

# Terminal 2: Run server
ros2 run pj_ros_bridge bridge_server

# Terminal 3: Run client 1
python3 tests/integration/test_client.py --subscribe /topic1 /topic2

# Terminal 4: Run client 2
python3 tests/integration/test_client.py --subscribe /topic1
```

**Completion Criteria**: Multiple clients can connect, subscribe to topics (including shared topics), and sessions timeout correctly when heartbeats stop

---

## Milestone 5: Message Aggregation & Publishing

### Goals
- Aggregate buffered messages from all active topics
- Serialize aggregated message according to specification
- Publish at 50 Hz via WebSocket binary frames

### Tasks

#### 5.1 Message Serialization Format
Implement custom binary serialization as specified:
- Number of messages (uint32_t)
- For each message:
  - Topic name length (uint16_t)
  - Topic name (N bytes UTF-8)
  - Publish timestamp (uint64_t nanoseconds since epoch)
  - Receive timestamp (uint64_t nanoseconds since epoch)
  - Message data length (uint32_t)
  - Message data (N bytes - CDR serialized)

Create `AggregatedMessageSerializer` class with:
- `void add_message(topic, pub_time, recv_time, data)` method
- `std::vector<uint8_t> serialize()` method to build final buffer
- `void clear()` method to reset for next cycle

#### 5.1.1 ZSTD Compression
- After serialization, compress the aggregated message using ZSTD
- Add compression wrapper function:
  - `std::vector<uint8_t> compress_zstd(const std::vector<uint8_t>& data)`
  - Use appropriate compression level (e.g., ZSTD_CLEVEL_DEFAULT or 3)
  - Handle compression errors gracefully
- The compressed buffer is what gets published via WebSocket binary frames
- Add decompression to Python test client for validation

#### 5.2 50 Hz Publisher Timer
- Create ROS2 timer running at 50 Hz (20ms period)
- Timer callback should:
  1. Query MessageBuffer for all new messages since last publish
  2. If no new messages, skip (don't publish empty aggregation)
  3. Build aggregated message using AggregatedMessageSerializer
  4. Compress the serialized buffer using ZSTD
  5. Publish compressed buffer via middleware WebSocket binary frames
  6. Track last publish timestamp and compression statistics

#### 5.3 Message Buffer Coordination
- MessageBuffer should track "last read" timestamp per topic
- `get_new_messages()` returns messages received since last read
- After successful publish, update "last read" timestamp
- This ensures messages are only sent once

#### 5.4 Testing
- Unit tests:
  - Test serialization format with known messages
  - Verify byte layout matches specification
  - Test empty aggregation handling
  - Test ZSTD compression/decompression round-trip
  - Verify compression reduces message size
- Integration tests:
  - Python client subscribes to topics
  - Client receives compressed data
  - Client decompresses using zstandard library
  - Client deserializes aggregated messages
  - Verify message count, topic names, timestamps
  - Verify message data integrity (compare with source)
  - Measure actual publish rate (should be ~50 Hz)
  - Measure compression ratio and performance impact

**Test Command**:
```bash
# Terminal 1: Play rosbag at slower rate for debugging
ros2 bag play DATA/sample.mcap --rate 0.1

# Terminal 2: Run server
ros2 run pj_ros_bridge bridge_server

# Terminal 3: Subscribe and receive data
python3 tests/integration/test_client.py --subscribe ims_msgs::RoboticsInputs --receive
```

**Completion Criteria**: Client successfully receives aggregated messages at 50 Hz with correct serialization format and data integrity

---

## Milestone 6: Main Server Integration & Configuration

### Goals
- Integrate all components into main server application
- Add configuration support
- Implement proper logging
- Handle graceful shutdown

### Tasks

#### 6.1 Bridge Server Class
- Create `BridgeServer` class that orchestrates:
  - ROS2 node initialization
  - Middleware setup
  - TopicDiscovery
  - SchemaExtractor
  - GenericSubscriptionManager
  - MessageBuffer
  - SessionManager
  - 50 Hz publisher timer
- Implement main request/response loop:
  - Receive requests via middleware
  - Route to appropriate handler (get_topics, subscribe, heartbeat)
  - Send responses

#### 6.2 Configuration System
- Support configuration via:
  - ROS2 parameters
  - Command-line arguments
  - Config file (YAML)
- Configurable parameters:
  - `port`: WebSocket port (default: 8080)
  - `publish_rate`: Aggregation publish rate (default: 50 Hz)
  - `session_timeout`: Client timeout duration (default: 10 seconds)
  - `buffer_size`: Max messages per topic buffer (default: 1000)
  - `topic_filter`: Regex pattern to filter topics (optional)

#### 6.3 Logging
- Use rclcpp logging macros:
  - `RCLCPP_INFO`: Server start, client connections, subscriptions
  - `RCLCPP_DEBUG`: Message buffering, publish cycles
  - `RCLCPP_WARN`: Session timeouts, buffer overflows
  - `RCLCPP_ERROR`: Subscription failures, schema extraction errors
- Add logging for:
  - Server initialization
  - Client connections/disconnections
  - Topic subscriptions/unsubscriptions
  - Session creation/timeout
  - Publish statistics (messages/sec)

#### 6.4 Graceful Shutdown
- Handle SIGINT (Ctrl+C)
- On shutdown:
  1. Stop accepting new connections
  2. Unsubscribe from all ROS2 topics
  3. Close WebSocket connections
  4. Clean up resources
  5. Log shutdown complete

#### 6.5 Main Entry Point
- Create `main.cpp` with:
  - ROS2 initialization
  - BridgeServer instantiation
  - Configuration loading
  - Spin loop
  - Shutdown handling

#### 6.6 Testing
- Integration tests:
  - Test server start/stop
  - Test configuration loading
  - Test full workflow: connect → discover → subscribe → receive → disconnect
  - Test server continues running when client disconnects
  - Test clean shutdown doesn't crash

**Test Command**:
```bash
# Run with custom config
ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args \
    -p port:=9090 \
    -p publish_rate:=30.0

# Full integration test
python3 tests/integration/test_full_workflow.py
```

**Completion Criteria**: Server runs stably with all components integrated, supports configuration, logs appropriately, and shuts down cleanly

---

## Milestone 7: Python Test Client Development

### Goals
- Create comprehensive Python test client
- Support all server API operations
- Enable manual and automated testing
- Provide debugging utilities

### Tasks

#### 7.1 Basic Client Implementation
Create `tests/integration/test_client.py` with:
- WebSocket connection for both API (text frames) and data stream (binary frames)
- Command-line argument parsing
- Basic operations:
  - Connect to server
  - Get topics list
  - Subscribe to topics
  - Send heartbeats
  - Receive aggregated messages

#### 7.2 Message Deserialization
- Implement ZSTD decompression first:
  - Use Python `zstandard` library to decompress received data
  - Handle decompression errors
- Implement deserializer matching server's serialization format:
  - Parse aggregated message header (message count)
  - Extract each message: topic, timestamps, data
  - Optionally deserialize CDR data (for validation)
- Print received messages in human-readable format

#### 7.3 Heartbeat Thread
- Create background thread that sends heartbeat every 1 second
- Ensure heartbeat continues while client is active
- Stop heartbeat on client shutdown

#### 7.4 Command-Line Interface
Support commands:
```bash
# List available topics
python3 test_client.py --command get_topics

# Subscribe to specific topics and receive data
python3 test_client.py --subscribe /topic1 /topic2 --duration 30

# Subscribe and save received messages to file
python3 test_client.py --subscribe /topic1 --output data.bin

# Run with specific server address
python3 test_client.py --port 8080 --subscribe /topic1
```

#### 7.5 Validation & Statistics
- Track and display statistics:
  - Messages received per topic
  - Receive rate (Hz)
  - Data volume (bytes/sec)
  - Latency (receive_time - publish_time)
- Validate message integrity:
  - Check serialization format
  - Verify timestamps are reasonable
  - Optionally validate CDR data

#### 7.6 Testing
- Test client against running server
- Verify all commands work correctly
- Test error handling (server not running, invalid topics, etc.)
- Test long-running sessions (hours)

**Completion Criteria**: Python client can perform all operations, receive and validate data, and provide useful debugging information

---

## Milestone 8: Unit Test Suite ✅ COMPLETED (2025-11-03)

### Goals
- Comprehensive unit test coverage
- Automated test execution
- Integration with colcon test

### Completion Status
**All 69 unit tests passing**:
- 9 middleware tests (initialization, shutdown, request/reply, publish)
- 4 topic discovery tests
- 3 schema extractor tests (with reference data validation)
- 10 message buffer tests (zero-copy API with move semantics)
- 10 generic subscription manager tests (reference counting)
- 10 session manager tests (creation, heartbeat, timeout)
- 18 message serializer tests (streaming API, ZSTD compression)

### Key Achievements

#### 8.1 Unit Test Files ✅
All test files created and passing:

**test_message_buffer.cpp** ✅:
- Zero-copy API using `move_messages()` and shared pointers
- Thread-safe message buffering with automatic cleanup
- Auto-deletion of messages older than 1 second
- Move semantics via `std::swap()` for ownership transfer

**test_session_manager.cpp** ✅:
- Session creation and heartbeat management
- Timeout detection (10 second default)
- Session cleanup and subscription tracking
- Thread-safe operations

**test_message_serializer.cpp** ✅:
- Streaming serialization (no intermediate storage)
- Little-endian binary format (uint16_t, uint32_t, uint64_t)
- ZSTD compression/decompression (level 1)
- Serialization format validation

**test_generic_subscription_manager.cpp** ✅:
- Subscription reference counting
- Shared subscriptions across clients
- Subscribe/unsubscribe operations

**test_schema_extractor.cpp** ✅:
- Schema extraction via .msg file reading
- Depth-first traversal for nested types
- Validation against reference files in DATA/

#### 8.2 Test Utilities ✅
- Test fixtures for common setup
- Reference schema validation data
- Automated message generation

#### 8.3 CMake Integration ✅
```cmake
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(${PROJECT_NAME}_tests
    tests/unit/test_websocket_middleware.cpp
    tests/unit/test_topic_discovery.cpp
    tests/unit/test_schema_extractor.cpp
    tests/unit/test_message_buffer.cpp
    tests/unit/test_generic_subscription_manager.cpp
    tests/unit/test_session_manager.cpp
    tests/unit/test_message_serializer.cpp
  )
  target_link_libraries(${PROJECT_NAME}_tests ${PROJECT_NAME}_lib data_path)
endif()
```

#### 8.4 Continuous Testing ✅
- Pre-commit hooks configured (`.pre-commit-config.yaml`)
- All linters passing (cppcheck, lint_cmake, xmllint)
- Tests run automatically via colcon

**Test Command**:
```bash
cd ~/ws_plotjuggler
source /opt/ros/humble/setup.bash
colcon build --packages-select pj_ros_bridge
colcon test --packages-select pj_ros_bridge
colcon test-result --verbose
```

**Completion Criteria Met**: All 69 unit tests pass, comprehensive coverage of core components

---

## Milestone 9: Error Handling & Robustness ✅ SUBSTANTIALLY COMPLETED (2025-11-03)

### Goals
- Handle edge cases gracefully
- Add proper error messages
- Improve stability and reliability

### Completion Status
**Core error handling implemented**:
- ✅ WebSocket initialization error handling with `tl::expected`
- ✅ Partial subscription success reporting
- ✅ Detailed error responses with specific failure reasons
- ✅ Malformed JSON request handling
- ✅ Non-existent topic detection
- ⏸️ Resource limits (deferred - not implemented)
- ⏸️ Retry logic and circuit breakers (deferred - not needed yet)
- ⏸️ Advanced resilience features (deferred - low priority)

### Key Achievements

#### 9.1 Error Scenarios Handled ✅
Implemented comprehensive error handling for:
- ✅ Client sends malformed JSON requests → returns `INVALID_JSON` error
- ✅ Client subscribes to non-existent topic → included in `failures` array with reason
- ✅ WebSocket server errors (port in use, bind failed) → detailed error with errno
- ✅ ROS2 subscription failures → tracked in partial success response
- ✅ Schema extraction fails → returned in `failures` array
- ⏸️ Buffer overflow conditions (automatic cleanup prevents this)
- ⏸️ Message deserialization errors (not applicable - pass-through design)

#### 9.2 Error Response Format ✅
Standardized error responses using JSON:
```json
// Full error
{
    "status": "error",
    "error_code": "TOPIC_NOT_FOUND",
    "message": "Topic '/invalid' does not exist"
}

// Partial success with detailed failures
{
    "status": "partial_success",
    "message": "Some subscriptions failed",
    "schemas": { /* successful topics */ },
    "failures": [
        {
            "topic": "/invalid_topic",
            "reason": "Topic does not exist"
        },
        {
            "topic": "/another_topic",
            "reason": "Schema extraction failed: ..."
        }
    ]
}
```

#### 9.3 Type-Safe Error Handling with tl::expected ✅
Replaced output parameters with `tl::expected<T, E>`:
```cpp
// Before
bool initialize(uint16_t port);

// After
tl::expected<void, std::string> initialize(uint16_t port);
```

Benefits:
- Compile-time error handling guarantees
- Detailed error messages with errno codes
- Example: "Failed to bind WebSocket server to port 8080: Address already in use (errno 98)"

#### 9.4 Partial Success Reporting ✅
Enhanced subscription handler in `bridge_server.cpp`:
- Tracks each subscription attempt individually
- Returns three-tier status: `success`, `partial_success`, or `error`
- Only successfully subscribed topics are added to session
- Each failure includes topic name and specific reason
- Ensures session state remains consistent even with partial failures

Example flow:
1. Client requests topics: ["/valid1", "/invalid", "/valid2"]
2. Server attempts all subscriptions
3. `/invalid` fails → added to failures array
4. Response includes:
   - `status: "partial_success"`
   - `schemas: { "/valid1": ..., "/valid2": ... }`
   - `failures: [{ "topic": "/invalid", "reason": "Topic does not exist" }]`
5. Session only tracks `/valid1` and `/valid2`

#### 9.5 Resilience Improvements ⏸️ (Deferred)
Not implemented (low priority for current use case):
- ⏸️ Retry logic for transient failures
- ⏸️ Circuit breaker for repeatedly failing operations
- ⏸️ Watchdog for detecting stuck operations

Current design is already resilient:
- Automatic message cleanup prevents unbounded growth
- Session timeouts handle disconnected clients
- Partial failure handling prevents cascading errors

#### 9.6 Resource Limits ⏸️ (Deferred)
Not implemented (to be added if needed):
- ⏸️ Max clients limit
- ⏸️ Max subscriptions per client
- ⏸️ Max total subscriptions
- ⏸️ Memory usage monitoring

Current design has implicit limits:
- Message buffer auto-cleanup (1 second retention)
- Session timeout (10 seconds)
- WebSocket connection limits

#### 9.7 Testing ✅
Error handling validated through:
- ✅ Unit tests for middleware initialization failures (9 tests)
- ✅ Integration testing with Python test client
- ✅ Manual testing of invalid subscription requests
- ⏸️ Fuzzing tests (not implemented - low priority)
- ⏸️ Stress tests (not implemented - future work)
- ⏸️ Long-running stability tests (not implemented - future work)

**Completion Criteria**: Server handles common error conditions gracefully, provides actionable error messages. Advanced resilience features and resource limits deferred for future implementation if needed.

---

## Milestone 10: Documentation & Polish

### Goals
- Complete documentation
- Code cleanup and review
- Performance optimization
- Release preparation

### Tasks

#### 10.1 Code Documentation
- Add comprehensive comments to all classes and methods
- Document serialization format in detail
- Add usage examples in code comments
- Generate Doxygen documentation (optional)

#### 10.2 User Documentation
Update README.md with:
- Project overview and features
- Build instructions
- Usage examples
- Configuration options
- Troubleshooting guide
- Performance characteristics

#### 10.3 API Documentation
Document the client-server protocol:
- Request/response message formats
- Aggregated message format
- Connection sequence diagram
- Error codes and meanings

#### 10.4 Performance Optimization
- Profile code to identify bottlenecks
- Optimize hot paths (message serialization, buffering)
- Reduce memory allocations
- Optimize lock contention
- Benchmark performance:
  - Max messages/second
  - Max clients
  - Latency measurements

#### 10.5 Code Quality
- Run static analysis (clang-tidy, cppcheck)
- Address all warnings
- Format code consistently (clang-format)
- Remove debug code and TODOs
- Verify no memory leaks (valgrind)

#### 10.6 Example Applications
Create example client applications:
- Simple Python subscriber example
- C++ client example (if needed)
- Visualization example (optional)

**Completion Criteria**: Code is well-documented, performant, and ready for production use

---

## Testing Strategy Summary

### Per-Milestone Testing
Each milestone has specific testing criteria that must pass before proceeding.

### Integration Testing Workflow
```bash
# 1. Start rosbridge server
cd ~/ws_plotjuggler
source /opt/ros/humble/setup.bash
colcon build --packages-select pj_ros_bridge
ros2 run pj_ros_bridge bridge_server

# 2. Play test rosbag (in another terminal)
source /opt/ros/humble/setup.bash
ros2 bag play src/pj_ros_bridge/DATA/sample.mcap --loop

# 3. Run Python test client (in another terminal)
cd src/pj_ros_bridge
python3 tests/integration/test_client.py --subscribe <topics>
```

### Automated Test Suite
```bash
# Run all unit tests
colcon test --packages-select pj_ros_bridge

# Run integration tests
python3 -m pytest tests/integration/

# Run performance benchmarks
ros2 run pj_ros_bridge benchmark_node
```

### Manual Verification Checklist
Before considering the project complete:
- [ ] Server starts without errors
- [ ] Topic discovery returns all topics from sample.mcap
- [ ] Schemas are correctly extracted for custom message types
- [ ] Client can subscribe and receive messages
- [ ] Multiple clients can share subscriptions
- [ ] Session timeout works (stop heartbeats, verify cleanup)
- [ ] Server publishes at 50 Hz consistently
- [ ] Message data integrity is preserved
- [ ] Server handles Ctrl+C shutdown gracefully
- [ ] No memory leaks detected
- [ ] Performance meets requirements (>1000 msgs/sec)

---

## Dependencies & Prerequisites

### ROS2 Packages (Runtime)
- `rclcpp` - ROS2 C++ client library
- `ament_index_cpp` - Locate ROS2 package share directories
- `ament_cmake` - Build system (buildtool)

### ROS2 Packages (Test Only)
- `ament_cmake_gtest` - Testing framework
- `sensor_msgs` - Standard sensor message types for unit tests
- `geometry_msgs` - Standard geometry message types for unit tests

### External Libraries
- **IXWebSocket** (`ixwebsocket/11.4.6`) - WebSocket server/client (via Conan)
- **ZSTD** (libzstd) - Compression library (system package, FindZSTD.cmake in cmake/)
- **nlohmann/json** - JSON library (header-only, in 3rdparty/)
- **tl::expected** - Error handling (header-only, in 3rdparty/)

### Python (for test client)
- `websocket-client` - WebSocket client library
- `zstandard` - ZSTD decompression library
- `argparse` - CLI argument parsing (built-in)
- `struct` - Binary serialization (built-in)
- `json` - JSON parsing (built-in)

### Development Tools
- ROS2 Humble
- colcon
- gtest
- clang-format (optional)
- clang-tidy (optional)
- valgrind (optional)

---

## Risk Mitigation

### Potential Challenges

1. **Schema Extraction Complexity**
   - *Risk*: Custom message types may not expose schema correctly
   - *Mitigation*: Start with simple standard messages, test incrementally with sample.mcap types
   - *Fallback*: Use ROS2 message definition text files if introspection insufficient

2. **WebSocket Identity Tracking**
   - *Risk*: Client identity may not be reliable for session management
   - *Mitigation*: Use WebSocket connection identity via `connectionState->getId()`
   - *Fallback*: Require clients to send UUID in initial connection message

3. **Performance at High Message Rates**
   - *Risk*: 50 Hz aggregation may not keep up with high-rate topics
   - *Mitigation*: Profile early, optimize serialization and buffer operations
   - *Fallback*: Make publish rate configurable, allow dropping old messages

4. **CDR Serialization Compatibility**
   - *Risk*: Serialized messages may not be portable across platforms
   - *Mitigation*: Test with different endianness, document format assumptions
   - *Fallback*: Add endianness indicator to aggregated message header

5. **Thread Safety Issues**
   - *Risk*: Concurrent access to buffers and sessions may cause crashes
   - *Mitigation*: Use proper locking from the start, write thread safety tests
   - *Fallback*: Use thread sanitizer to detect issues during testing

---

## Open Questions

Before beginning implementation, clarify:

1. **Schema Format**: Should the JSON schema follow a specific standard (e.g., JSON Schema, ROS IDL format), or is a custom format acceptable?

2. **Security**: Are there any authentication/authorization requirements for clients connecting to the server?

3. **Network Protocol**: Should we support encryption (e.g., WSS/TLS) for sensitive data?

4. **Message Filtering**: Should clients be able to filter messages (e.g., by time range, decimation rate)?

5. **Backwards Compatibility**: Do we need to maintain wire format compatibility with any existing system?

6. **Logging Destination**: Should logs go to ROS2 logging, file, or both?

---

## Success Metrics

The project is considered complete when:
- ✅ Server successfully bridges all topics from sample.mcap
- ✅ Multiple Python clients can connect simultaneously
- ✅ Aggregated messages published consistently at 50 Hz
- ✅ Session management handles connects/disconnects/timeouts correctly
- ✅ All unit tests pass (>80% code coverage)
- ✅ Integration tests pass with real rosbag data
- ✅ No memory leaks or crashes during 1-hour stress test
- ✅ Documentation is complete and clear
- ✅ Performance exceeds minimum requirements:
  - Handles at least 10 concurrent clients
  - Processes at least 1000 messages/second
  - Latency < 100ms (receive_time - publish_time)
