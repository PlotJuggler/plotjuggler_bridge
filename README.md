# pj_ros_bridge

A high-performance ROS2 bridge server that forwards ROS2 topic content over WebSocket, without requiring DDS on the client side.

## Overview

`pj_ros_bridge` enables clients to subscribe to ROS2 topics and receive aggregated messages at 50 Hz without needing a full ROS2/DDS installation. This is particularly useful for visualization tools like PlotJuggler, remote monitoring applications, and lightweight clients that need access to ROS2 data.

### Key Features

- **No DDS Required**: Clients connect via WebSocket (single port) without needing ROS2/DDS installed
- **High Performance**: 50 Hz message aggregation with ZSTD compression
- **Multi-Client Support**: Multiple clients can connect simultaneously with shared subscriptions
- **Session Management**: Automatic cleanup of disconnected clients via heartbeat monitoring and WebSocket close events
- **Runtime Schema Discovery**: Automatic extraction of message schemas from installed ROS2 packages
- **Zero-Copy Design**: Efficient message handling using shared pointers and move semantics
- **Type-Safe Error Handling**: Comprehensive error reporting using `tl::expected`

## Architecture

### Communication Pattern

The server uses a single WebSocket port (default 8080):

- **Text frames**: JSON API requests and responses (get_topics, subscribe, heartbeat)
- **Binary frames**: ZSTD-compressed aggregated message stream at 50 Hz

### Core Components

- **MiddlewareInterface / WebSocketMiddleware**: Abstraction layer over IXWebSocket for future middleware replacement
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
- **Conan**: Package manager for IXWebSocket dependency

### Dependencies

#### ROS2 Packages (Runtime)
- `rclcpp` - ROS2 C++ client library
- `ament_index_cpp` - Locate ROS2 package share directories

#### System Libraries
- **ZSTD** (libzstd): `sudo apt install libzstd-dev`

#### Conan-Managed Libraries
- **IXWebSocket** (`ixwebsocket/11.4.6`): WebSocket server/client

#### Header-Only Libraries (Included in 3rdparty/)
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
sudo apt install libzstd-dev

# 4. Install Conan dependencies
cd ~/ws_plotjuggler/src/pj_ros_bridge
conan install . --output-folder=conan_output --build=missing

# 5. Source ROS2
source /opt/ros/humble/setup.bash

# 6. Build the package
cd ~/ws_plotjuggler
colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release

# 7. Source the workspace
source install/setup.bash
```

### Running Tests

```bash
# Run all unit tests (64 tests)
colcon test --packages-select pj_ros_bridge

# View test results
colcon test-result --verbose
```

### Code Formatting

The project uses pre-commit hooks for code formatting:

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
- WebSocket port: 8080
- Publish rate: 50 Hz
- Session timeout: 10 seconds

#### Custom Configuration

```bash
ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args \
  -p port:=9090 \
  -p publish_rate:=30.0 \
  -p session_timeout:=15.0
```

### Configuration Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `port` | int | 8080 | WebSocket server port |
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

### Python Test Client

The package includes a Python test client for testing and demonstration.

**Install dependencies:**

```bash
pip install websocket-client zstandard
```

**Usage:**

```bash
# Discover available topics
python3 tests/integration/test_client.py --command get_topics

# Subscribe to specific topics
python3 tests/integration/test_client.py --subscribe /imu /points

# Subscribe with custom server address
python3 tests/integration/test_client.py \
  --server 192.168.1.100 \
  --port 9090 \
  --subscribe /topic1

# Run for specific duration with verbose output
python3 tests/integration/test_client.py --subscribe /topic1 --duration 60 --verbose
```

## API Protocol

### Get Topics

**Request:**
```json
{"command": "get_topics"}
```

**Response:**
```json
{
  "status": "success",
  "topics": [
    {"name": "/topic_name", "type": "package_name/msg/MessageType"}
  ]
}
```

### Subscribe

**Request:**
```json
{
  "command": "subscribe",
  "topics": ["/topic1", "/topic2"]
}
```

**Response:**
```json
{
  "status": "success",
  "schemas": {
    "/topic1": "message definition text",
    "/topic2": "message definition text"
  }
}
```

### Heartbeat

**Request:**
```json
{"command": "heartbeat"}
```

**Response:**
```json
{"status": "ok"}
```

### Binary Message Format

Aggregated messages are sent as ZSTD-compressed binary WebSocket frames at 50 Hz. The decompressed format is a sequence of messages:

```
For each message:
  - Topic name length (uint16_t little-endian)
  - Topic name (N bytes UTF-8)
  - Timestamp (uint64_t nanoseconds since epoch, little-endian)
  - Message data length (uint32_t little-endian)
  - Message data (N bytes - CDR serialized from ROS2)
```

## Performance

- **Throughput**: >1000 messages/second
- **Latency**: <100ms (publish time to receive time)
- **Concurrent Clients**: 10+ clients supported
- **Compression**: ZSTD level 1 (typically 50-70% size reduction)
- **Memory**: Automatic cleanup prevents unbounded growth (1 second message retention)

## Troubleshooting

### Server fails to start with "Failed to listen on port"

Another process is using the port. Either kill the conflicting process or use a custom port:

```bash
ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args -p port:=9090
```

### Client receives no data

1. Verify server is running: `ps aux | grep pj_ros_bridge`
2. Check topics are being published: `ros2 topic list`
3. Verify heartbeat is being sent (required every 1 second)
4. Check server logs: `ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args --log-level debug`

### "Failed to get schema for topic" error

The message type's .msg file was not found. Ensure the ROS2 package containing the message type is installed and sourced:

```bash
ros2 interface show <package_name>/msg/<MessageType>
```

### Session timeout / Automatic unsubscription

The client stopped sending heartbeats. Ensure the client sends a heartbeat every 1 second. The default timeout is 10 seconds. Increase if needed:

```bash
ros2 run pj_ros_bridge pj_ros_bridge_node --ros-args -p session_timeout:=20.0
```

## Project Structure

```
pj_ros_bridge/
├── include/pj_ros_bridge/
│   ├── middleware/
│   │   ├── middleware_interface.hpp
│   │   └── websocket_middleware.hpp
│   ├── topic_discovery.hpp
│   ├── schema_extractor.hpp
│   ├── message_buffer.hpp
│   ├── message_serializer.hpp
│   ├── session_manager.hpp
│   ├── generic_subscription_manager.hpp
│   ├── time_utils.hpp
│   └── bridge_server.hpp
├── src/
│   ├── middleware/
│   │   └── websocket_middleware.cpp
│   ├── topic_discovery.cpp
│   ├── schema_extractor.cpp
│   ├── message_buffer.cpp
│   ├── message_serializer.cpp
│   ├── session_manager.cpp
│   ├── generic_subscription_manager.cpp
│   ├── bridge_server.cpp
│   └── main.cpp
├── tests/
│   ├── unit/                        # 64 unit tests (gtest)
│   └── integration/
│       └── test_client.py
├── 3rdparty/                        # Header-only dependencies
├── DATA/                            # Test data (rosbag, reference schemas)
├── cmake/                           # CMake modules
├── conanfile.txt
├── CMakeLists.txt
├── package.xml
├── .clang-tidy
└── .pre-commit-config.yaml
```

## Coding Standards

The project follows strict coding standards enforced by `.clang-tidy`:

- Classes/Types: `CamelCase` (e.g., `BridgeServer`, `SessionManager`)
- Functions/Methods: `lower_case` (e.g., `get_topics()`, `update_heartbeat()`)
- Variables: `lower_case`
- Private members: suffix with `_` (e.g., `sessions_`, `mutex_`)
- Constants: `CamelCase` with `k` prefix (e.g., `kDefaultTimeout`)

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

## References

- [ROS2 Generic Subscription](https://api.nav2.org/rolling/html/generic__subscription_8hpp_source.html)
- [rosbag2 MCAP Storage](https://github.com/ros2/rosbag2/blob/rolling/rosbag2_storage_mcap/src/mcap_storage.cpp)
- [IXWebSocket](https://github.com/machinezone/IXWebSocket)
- [tl::expected](https://github.com/TartanLlama/expected)
