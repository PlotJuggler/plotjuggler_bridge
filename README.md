# pj_ros_bridge

A high-performance ROS2 bridge server that forwards ROS2 topic content over the network using ZeroMQ, without requiring DDS on the client side.

## Overview

`pj_ros_bridge` enables clients to subscribe to ROS2 topics and receive aggregated messages at 50 Hz without needing a full ROS2/DDS installation. This is particularly useful for visualization tools like PlotJuggler, remote monitoring applications, and lightweight clients that need access to ROS2 data.

### Key Features

- **No DDS Required**: Clients connect via ZeroMQ (TCP) without needing ROS2/DDS installed
- **High Performance**: 50 Hz message aggregation with ZSTD compression
- **Multi-Client Support**: Multiple clients can connect simultaneously with shared subscriptions
- **Session Management**: Automatic cleanup of disconnected clients via heartbeat monitoring
- **Runtime Schema Discovery**: Automatic extraction of message schemas from installed ROS2 packages
- **Zero-Copy Design**: Efficient message handling using shared pointers and move semantics
- **Type-Safe Error Handling**: Comprehensive error reporting using `tl::expected`

## Architecture

### Communication Patterns

The server uses two ZeroMQ socket patterns:

1. **REQ-REP Pattern** (port 5555): Client API requests
   - Discover available topics
   - Subscribe/unsubscribe to topics
   - Send heartbeats

2. **PUB-SUB Pattern** (port 5556): Data streaming
   - Aggregated messages published at 50 Hz
   - ZSTD compressed binary format
   - All subscribed topics in single message

### Core Components

- **MiddlewareInterface / ZmqMiddleware**: Abstraction layer over ZeroMQ for future middleware replacement
- **TopicDiscovery**: Discovers available ROS2 topics using `rclcpp::Node::get_topic_names_and_types()`
- **SchemaExtractor**: Extracts message schemas by reading .msg files from ROS2 package share directories
- **GenericSubscriptionManager**: Manages ROS2 subscriptions using `rclcpp::GenericSubscription` with reference counting
- **MessageBuffer**: Thread-safe buffer with automatic cleanup (1 second retention)
- **SessionManager**: Tracks client sessions with heartbeat monitoring (10 second timeout)
- **AggregatedMessageSerializer**: Custom binary serialization with ZSTD compression
- **BridgeServer**: Main orchestrator integrating all components

## Requirements

### System Requirements

- **OS**: Linux (tested on Ubuntu 22.04)
- **ROS2**: Humble or later
- **C++ Standard**: C++17
- **Build System**: colcon

### Dependencies

#### ROS2 Packages (Runtime)
- `rclcpp` - ROS2 C++ client library
- `ament_index_cpp` - Locate ROS2 package share directories

#### System Libraries
- **ZeroMQ** (libzmq): `sudo apt install libzmq3-dev`
- **ZSTD** (libzstd): `sudo apt install libzstd-dev`

#### Header-Only Libraries (Included in 3rdparty/)
- **cppzmq** - C++ bindings for ZeroMQ
- **nlohmann/json** - JSON library
- **tl/expected** - Type-safe error handling

#### ROS2 Packages (Build/Test Only)
- `ament_cmake` - Build system
- `ament_cmake_gtest` - Testing framework
- `sensor_msgs` - Standard sensor message types for unit tests
- `geometry_msgs` - Standard geometry message types for unit tests

## Installation

### Building from Source

```bash
# 1. Create workspace (if needed)
mkdir -p ~/ws_plotjuggler/src
cd ~/ws_plotjuggler/src

# 2. Clone the repository
git clone <repository_url> pj_ros_bridge

# 3. Install system dependencies
sudo apt install libzmq3-dev libzstd-dev

# 4. Source ROS2
source /opt/ros/humble/setup.bash

# 5. Build the package
cd ~/ws_plotjuggler
colcon build --packages-select pj_ros_bridge

# 6. Source the workspace
source install/setup.bash
```

### Running Tests

```bash
# Run all unit tests (59 tests)
colcon test --packages-select pj_ros_bridge

# View test results
colcon test-result --verbose

# Run specific test
./build/pj_ros_bridge/pj_ros_bridge_tests --gtest_filter="MiddlewareTest.*"
```

### Code Formatting

The project uses pre-commit hooks for code formatting and linting:

```bash
# Install pre-commit (if needed)
pip3 install pre-commit

# Install hooks
cd ~/ws_plotjuggler/src/pj_ros_bridge
pre-commit install

# Run manually on all files
pre-commit run -a
```

## Usage

### Starting the Server

#### Basic Usage (Default Configuration)

```bash
source /opt/ros/humble/setup.bash
source ~/ws_plotjuggler/install/setup.bash
ros2 run pj_ros_bridge pj_ros_bridge_node
```

Default configuration:
- REQ-REP port: 5555
- PUB-SUB port: 5556
- Publish rate: 50 Hz
- Session timeout: 10 seconds

#### Custom Configuration

```bash
ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args \
  -p req_port:=5557 \
  -p pub_port:=5558 \
  -p publish_rate:=30.0 \
  -p session_timeout:=15.0
```

### Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `req_port` | int | 5555 | Port for REQ-REP socket (client API requests) |
| `pub_port` | int | 5556 | Port for PUB-SUB socket (data streaming) |
| `publish_rate` | double | 50.0 | Aggregation publish rate in Hz |
| `session_timeout` | double | 10.0 | Client timeout duration in seconds |

### Testing with Sample Data

```bash
# Terminal 1: Play rosbag
source /opt/ros/humble/setup.bash
ros2 bag play /path/to/sample.mcap --loop

# Terminal 2: Run server
source /opt/ros/humble/setup.bash
source ~/ws_plotjuggler/install/setup.bash
ros2 run pj_ros_bridge pj_ros_bridge_node

# Terminal 3: Run Python test client
cd ~/ws_plotjuggler/src/pj_ros_bridge
python3 tests/integration/test_client.py --subscribe /topic1 /topic2
```

### Python Test Client Example

The package includes a comprehensive Python test client for testing and demonstration:

```bash
# Discover available topics
python3 tests/integration/test_client.py --command get_topics

# Subscribe to specific topics
python3 tests/integration/test_client.py --subscribe /imu /points

# Subscribe with custom server address
python3 tests/integration/test_client.py \
  --req-address tcp://localhost:5555 \
  --pub-address tcp://localhost:5556 \
  --subscribe /topic1

# Run for specific duration
python3 tests/integration/test_client.py --subscribe /topic1 --duration 60
```

## Performance Characteristics

### Tested Performance

- **Throughput**: >1000 messages/second
- **Latency**: <100ms (publish time to receive time)
- **Concurrent Clients**: 10+ clients supported
- **Compression Ratio**: Varies by message type (typically 50-70% reduction)
- **Memory**: Automatic cleanup prevents unbounded growth (1 second message retention)

### Design Optimizations

- **Zero-Copy Message Handling**: Uses `shared_ptr<SerializedMessage>` to avoid data copying
- **Move Semantics**: Message buffer ownership transferred via `std::swap()`
- **Streaming Serialization**: Messages serialized directly without intermediate storage
- **Reference Counting**: Shared ROS2 subscriptions across multiple clients
- **Automatic Cleanup**: Messages older than 1 second automatically deleted

## API Protocol

See [API.md](API.md) for detailed protocol specification including:
- Connection sequence
- Request/response message formats
- Binary serialization format
- Error handling
- Client implementation guide

## Troubleshooting

### Server fails to start with "Address already in use"

**Cause**: Another process is using port 5555 or 5556

**Solution**:
- Kill the conflicting process, or
- Use custom ports:
  ```bash
  ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args -p req_port:=5557 -p pub_port:=5558
  ```

### Client receives no data

**Checks**:
1. Verify server is running: `ps aux | grep pj_ros_bridge`
2. Check topics are being published: `ros2 topic list`
3. Verify heartbeat is being sent (required every 1 second)
4. Check server logs for errors: `ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args --log-level debug`

### "Failed to get schema for topic" error

**Cause**: Message type's .msg file not found in package share directory

**Solution**:
- Ensure the ROS2 package containing the message type is installed
- Verify with: `ros2 interface show <package_name>/msg/<MessageType>`
- Source the workspace containing the message package

### Session timeout / Automatic unsubscription

**Cause**: Client stopped sending heartbeats

**Solution**:
- Ensure client sends heartbeat every 1 second
- Default timeout is 10 seconds without heartbeat
- Increase timeout if needed: `-p session_timeout:=20.0`

## Development

### Project Structure

```
pj_ros_bridge/
   include/pj_ros_bridge/
      middleware/
         middleware_interface.hpp    # Abstract middleware
         zmq_middleware.hpp          # ZeroMQ implementation
      topic_discovery.hpp             # ROS2 topic discovery
      schema_extractor.hpp            # Message schema extraction
      message_buffer.hpp              # Thread-safe message buffer
      generic_subscription_manager.hpp # Subscription management
      session_manager.hpp             # Client session tracking
      message_serializer.hpp          # Binary serialization + ZSTD
      bridge_server.hpp               # Main server orchestrator
   src/
      middleware/zmq_middleware.cpp
      topic_discovery.cpp
      schema_extractor.cpp
      message_buffer.cpp
      generic_subscription_manager.cpp
      session_manager.cpp
      message_serializer.cpp
      bridge_server.cpp
      main.cpp                        # Entry point
   tests/
      unit/                           # 59 unit tests (gtest)
      integration/
          test_client.py              # Python test client
   3rdparty/                           # Header-only dependencies
   DATA/                               # Test data (rosbag, reference schemas)
   cmake/                              # CMake modules
```

### Coding Standards

The project follows strict coding standards enforced by `.clang-tidy`:

**Naming Conventions**:
- Classes/Types: `CamelCase` (e.g., `BridgeServer`, `SessionManager`)
- Functions/Methods: `lower_case` (e.g., `get_topics()`, `update_heartbeat()`)
- Variables: `lower_case`
- Private members: suffix with `_` (e.g., `sessions_`, `mutex_`)
- Constants: `CamelCase` with `k` prefix (e.g., `kDefaultTimeout`)

**Code Quality**:
- C++17 standard
- Thread-safe by design (explicit mutex usage)
- Comprehensive documentation comments
- All warnings treated as errors

### Testing

- **Unit Tests**: 59 tests covering all core components
- **Integration Tests**: Python test client for end-to-end testing
- **Linters**: cppcheck, clang-tidy, cmake-lint, xmllint
- **Code Coverage**: >80% target for core components

## License

Copyright 2025 Davide Faconti

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

## Contributing

Contributions are welcome! Please ensure:
- Code follows the project's coding standards
- All tests pass: `colcon test --packages-select pj_ros_bridge`
- Code is formatted: `pre-commit run -a`
- New features include unit tests
- Documentation is updated

## References

- [ROS2 Generic Subscription](https://api.nav2.org/rolling/html/generic__subscription_8hpp_source.html)
- [rosbag2 MCAP Storage](https://github.com/ros2/rosbag2/blob/rolling/rosbag2_storage_mcap/src/mcap_storage.cpp)
- [ZeroMQ Guide](https://zeromq.org/socket-api/)
- [tl::expected](https://github.com/TartanLlama/expected)
