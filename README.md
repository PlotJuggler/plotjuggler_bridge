# pj_ros_bridge

[![ROS2 Humble](https://github.com/PlotJuggler/plotjuggler_ros_bridge/actions/workflows/ros-humble.yaml/badge.svg?branch=main)](https://github.com/PlotJuggler/plotjuggler_ros_bridge/actions/workflows/ros-humble.yaml)
[![ROS2 Jazzy](https://github.com/PlotJuggler/plotjuggler_ros_bridge/actions/workflows/ros-jazzy.yaml/badge.svg?branch=main)](https://github.com/PlotJuggler/plotjuggler_ros_bridge/actions/workflows/ros-jazzy.yaml)
[![ROS2 Rolling](https://github.com/PlotJuggler/plotjuggler_ros_bridge/actions/workflows/ros-rolling.yaml/badge.svg?branch=main)](https://github.com/PlotJuggler/plotjuggler_ros_bridge/actions/workflows/ros-rolling.yaml)

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

For detailed architecture documentation, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Dependencies

#### ROS2 Packages (Runtime)
- `rclcpp` - ROS2 C++ client library
- `ament_index_cpp` - Locate ROS2 package share directories

#### System Libraries
- **ZSTD** (libzstd): `sudo apt install libzstd-dev`

#### Automatically Fetched Libraries (via CMake FetchContent)
- **IXWebSocket** (`v11.4.6`): WebSocket server/client

#### Header-Only Libraries (Included in 3rdparty/)
- **nlohmann/json** - JSON library
- **tl/expected** - Type-safe error handling

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

# 4. Source ROS2
source /opt/ros/humble/setup.bash

# 5. Build the package
cd ~/ws_plotjuggler
colcon build --packages-select pj_ros_bridge --cmake-args -DCMAKE_BUILD_TYPE=Release

# 6. Source the workspace
source install/setup.bash
```

Note: IXWebSocket is automatically downloaded and built via CMake FetchContent during the first build.

### Running Tests

```bash
# Run all unit tests (139 tests)
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

For the full API protocol documentation (commands, responses, binary wire format), see [docs/API.md](docs/API.md).

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
│   ├── unit/                        # 139 unit tests (gtest)
│   └── integration/
│       └── test_client.py
├── 3rdparty/                        # Header-only dependencies
├── DATA/                            # Test data (rosbag, reference schemas)
├── cmake/                           # CMake modules
├── CMakeLists.txt
├── package.xml
├── .clang-tidy
└── .pre-commit-config.yaml
```

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
