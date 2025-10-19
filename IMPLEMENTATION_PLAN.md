# ROS2 Bridge Implementation Plan

## Project Overview
Implement a C++ ROS2 bridge server that forwards ROS2 topic content over the network using ZeroMQ, without DDS. The server aggregates messages from multiple topics and publishes them at 50 Hz to connected clients.

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
  - `rosbag2_cpp`
  - `rosidl_typesupport_introspection_cpp`
  - `rosidl_runtime_cpp`
  - `gtest` (for unit tests)
- Add ZeroMQ (cppzmq) from 3rdparty folder
- Configure C++17 standard
- Set up include directories for headers

#### 1.2 Create Directory Structure
```
pj_ros_bridge/
├── include/pj_ros_bridge/
│   ├── middleware/
│   │   ├── middleware_interface.hpp
│   │   └── zmq_middleware.hpp
│   ├── message_buffer.hpp
│   ├── session_manager.hpp
│   └── bridge_server.hpp
├── src/
│   ├── middleware/
│   │   └── zmq_middleware.cpp
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
│   └── (cppzmq headers)
└── DATA/
    └── sample.mcap
```

#### 1.3 Create Middleware Abstraction
- Define `MiddlewareInterface` abstract class with methods:
  - `initialize()`: Setup middleware
  - `send_reply(data)`: Send REQ-REP response
  - `publish_data(data)`: Publish aggregated messages
  - `receive_request()`: Receive client requests
  - `get_client_identity()`: Get unique client identifier
- Implement `ZmqMiddleware` class using cppzmq
  - Use REP socket for API (bind to configurable port, default: 5555)
  - Use PUB socket for data streaming (bind to configurable port, default: 5556)
  - Implement connection identity tracking using ZMQ_IDENTITY

#### 1.4 Testing
- Write unit test for middleware initialization
- Build and verify compilation succeeds
- Test that ZMQ sockets can be created and bound

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
Research and implement schema extraction using:
- `rosidl_runtime_cpp` to get type support handles
- `rosidl_typesupport_introspection_cpp` to traverse message structure
- Create `SchemaExtractor` class with method `get_schema_json(topic_type)`

Key approach:
```cpp
// Get type support using rosidl_runtime_cpp
auto type_support = get_message_type_support_handle(msg_type);

// Get introspection typesupport
auto intro_ts = get_message_typesupport_handle(
    type_support,
    rosidl_typesupport_introspection_cpp::typesupport_identifier);

// Access MessageMembers structure to build schema
auto members = static_cast<const MessageMembers*>(intro_ts->data);
```

#### 2.3 JSON Schema Serialization
- Create `SchemaSerializer` class
- Implement recursive traversal of `MessageMembers` structure
- Convert to JSON format matching client expectations
- Include field names, types, array sizes, nested message definitions
- Use a simple JSON library (nlohmann/json recommended, or hand-craft if simple enough)

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
- Send response via middleware REP socket

#### 2.5 Testing
- Unit test topic discovery with sample.mcap playing
- Unit test schema extraction for common message types:
  - `std_msgs/msg/String`
  - `sensor_msgs/msg/Imu`
  - Custom types from sample.mcap (e.g., `ims_msgs/RoboticsInputs`)
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

#### 3.3 Timestamp Extraction
- Messages should contain publish timestamp in header
- For messages without standard header, use receive time as both timestamps
- Create utility function `extract_timestamp(serialized_msg, msg_type)`:
  - Use introspection to check if message has `std_msgs/Header`
  - Extract `stamp` field if available
  - Otherwise return current time

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
    - `client_id`: Unique identifier from ZMQ identity
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
- Publish at 50 Hz via ZeroMQ PUB socket

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

#### 5.2 50 Hz Publisher Timer
- Create ROS2 timer running at 50 Hz (20ms period)
- Timer callback should:
  1. Query MessageBuffer for all new messages since last publish
  2. If no new messages, skip (don't publish empty aggregation)
  3. Build aggregated message using AggregatedMessageSerializer
  4. Publish via middleware PUB socket
  5. Track last publish timestamp

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
- Integration tests:
  - Python client subscribes to topics
  - Client receives and deserializes aggregated messages
  - Verify message count, topic names, timestamps
  - Verify message data integrity (compare with source)
  - Measure actual publish rate (should be ~50 Hz)

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
  - `req_port`: REQ-REP socket port (default: 5555)
  - `pub_port`: PUB socket port (default: 5556)
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
  3. Close middleware sockets
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
ros2 run pj_ros_bridge bridge_server --ros-args \
    -p req_port:=5557 \
    -p pub_port:=5558 \
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
- ZeroMQ REQ-REP connection for API
- ZeroMQ SUB connection for data stream
- Command-line argument parsing
- Basic operations:
  - Connect to server
  - Get topics list
  - Subscribe to topics
  - Send heartbeats
  - Receive aggregated messages

#### 7.2 Message Deserialization
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
python3 test_client.py --server localhost:5555 --subscribe /topic1
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

## Milestone 8: Unit Test Suite

### Goals
- Comprehensive unit test coverage
- Automated test execution
- Integration with colcon test

### Tasks

#### 8.1 Unit Test Files
Create tests using Google Test (gtest):

**test_message_buffer.cpp**:
- Test add/get operations
- Test thread safety (concurrent operations)
- Test buffer size limits
- Test clear operation

**test_session_manager.cpp**:
- Test session creation
- Test heartbeat updates
- Test timeout detection
- Test session cleanup

**test_serialization.cpp**:
- Test aggregated message serialization
- Test deserialization
- Test edge cases (empty messages, large messages)
- Test endianness handling

**test_subscription_manager.cpp**:
- Test subscription reference counting
- Test subscribe/unsubscribe operations
- Test shared subscriptions

**test_schema_extractor.cpp**:
- Test schema extraction for various message types
- Test JSON serialization
- Test nested message handling

#### 8.2 Test Utilities
- Create mock middleware for testing without ZeroMQ
- Create message generators for test data
- Create test fixtures for common setup

#### 8.3 CMake Integration
- Add gtest to CMakeLists.txt
- Configure test executable
- Enable colcon test execution:
```cmake
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(${PROJECT_NAME}_tests
    tests/unit/test_message_buffer.cpp
    tests/unit/test_session_manager.cpp
    tests/unit/test_serialization.cpp
    # ... other tests
  )
  target_link_libraries(${PROJECT_NAME}_tests ${PROJECT_NAME}_lib)
endif()
```

#### 8.4 Continuous Testing
- Run tests as part of build process
- Set up pre-commit hooks (optional)
- Document how to run tests

**Test Command**:
```bash
cd ~/ws_plotjuggler
source /opt/ros/humble/setup.bash
colcon build --packages-select pj_ros_bridge
colcon test --packages-select pj_ros_bridge
colcon test-result --verbose
```

**Completion Criteria**: All unit tests pass, coverage is >80% for core components

---

## Milestone 9: Error Handling & Robustness

### Goals
- Handle edge cases gracefully
- Add proper error messages
- Improve stability and reliability

### Tasks

#### 9.1 Error Scenarios to Handle
- Client sends malformed JSON requests
- Client subscribes to non-existent topic
- Topic type cannot be determined
- Schema extraction fails for custom message
- ZeroMQ socket errors (port already in use, connection lost)
- Buffer overflow conditions
- ROS2 subscription failures
- Message deserialization errors

#### 9.2 Error Response Format
Standardize error responses:
```json
{
    "status": "error",
    "error_code": "TOPIC_NOT_FOUND",
    "message": "Topic '/invalid' does not exist"
}
```

#### 9.3 Resilience Improvements
- Add retry logic for transient failures
- Implement circuit breaker for repeatedly failing operations
- Add watchdog for detecting stuck operations
- Gracefully handle partial failures (some topics work, others don't)

#### 9.4 Resource Limits
- Enforce max clients limit
- Enforce max subscriptions per client
- Enforce max total subscriptions
- Add memory usage monitoring

#### 9.5 Testing
- Unit tests for error conditions
- Fuzzing tests for serialization/deserialization
- Stress tests (many clients, many topics, high message rates)
- Long-running stability tests

**Completion Criteria**: Server handles all error conditions gracefully without crashing, provides useful error messages

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

### ROS2 Packages
- rclcpp
- rosbag2_cpp
- rosidl_typesupport_introspection_cpp
- rosidl_runtime_cpp
- ament_cmake (build)
- ament_cmake_gtest (testing)

### External Libraries
- ZeroMQ (libzmq)
- cppzmq (headers in 3rdparty/)
- nlohmann/json or simple hand-crafted JSON serializer

### Python (for test client)
- pyzmq
- argparse
- struct (built-in)
- json (built-in)

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

2. **ZeroMQ Identity Tracking**
   - *Risk*: Client identity may not be reliable for session management
   - *Mitigation*: Research ZMQ_IDENTITY usage, consider client-generated UUIDs if needed
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

3. **Network Protocol**: Should we support encryption (e.g., CurveZMQ) for sensitive data?

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
