# plotjuggler_ros_bridge

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
- **Large Message Stripping**: Automatic stripping of large array fields (Image, PointCloud2, LaserScan, OccupancyGrid) to reduce bandwidth while preserving metadata
- **Type-Safe Error Handling**: Comprehensive error reporting using `tl::expected`

## Architecture

For detailed architecture documentation, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Dependencies

#### ROS2 Packages (Runtime)
- `rclcpp` - ROS2 C++ client library
- `ament_index_cpp` - Locate ROS2 package share directories

#### System Libraries
- **ZSTD** (libzstd): `sudo apt install libzstd-dev`

#### Vendored Packages
- **IXWebSocket** (`v11.4.6`): WebSocket server/client
- **nlohmann/json** - JSON library
- **tl/expected** - Type-safe error handling

## Installation

### Installation with Pixi


#### Install Pixi (if you don't have it)
```bash
curl -fsSL https://pixi.sh/install.sh | bash
```

Restart your terminal afterwards.

```bash
# 1. Clone the repository
git clone <repository_url>
cd plotjuggler_ros_bridge
```

```bash
# 2. Build the package
pixi run install
```

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

### Running Tests

```bash
# Run all unit tests (150 tests)
colcon test --packages-select pj_ros_bridge

# View test results
colcon test-result --verbose
```

## Usage

### Starting the Server

#### Basic Usage (Default Configuration)

```bash
source /opt/ros/humble/setup.bash
source ~/ws_plotjuggler/install/setup.bash
ros2 run pj_ros_bridge pj_ros_bridge_node
```

or

```bash
pixi run ros2 run pj_ros_bridge pj_ros_bridge_node
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
| `strip_large_messages` | bool | true | Strip large arrays from Image, PointCloud2, LaserScan, OccupancyGrid messages |

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

## License

**pj_ros_bridge** is licensed under the **GNU Affero General Public License v3.0 (AGPL-3.0)**.

Copyright (C) 2026 Davide Faconti

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

See the [LICENSE](LICENSE) file for the full license text.

### License FAQ

#### Can I use this software commercially?

**Yes, absolutely.** The AGPL does not restrict commercial use. You can:
- Use pj_ros_bridge in commercial products and services
- Charge customers for services that use this software
- Deploy it in production environments for profit

The AGPL only requires that if you **distribute modified versions** (or provide them as a network service), you must share those modifications under the same license.

#### Does using pj_ros_bridge affect my proprietary software?

**No, it does not.** Because pj_ros_bridge is a **standalone application** that communicates via inter-process communication (WebSocket), it does not impose license restrictions on:
- Your ROS2 nodes and packages
- Client applications connecting to the bridge
- Other software running on the same system
- Proprietary code that publishes to or subscribes from ROS2 topics

The AGPL "copyleft" provisions only apply to pj_ros_bridge itself and any modifications you make to it.

#### When do I need to share my code?

You must share modifications to pj_ros_bridge only if you:

1. **Distribute** modified versions to others (e.g., shipping a modified binary), OR
2. **Provide the modified software as a network service** to external users

You do **NOT** need to share code if you:
- Use pj_ros_bridge unmodified (even commercially)
- Modify it for internal use only within your organization
- Connect proprietary clients or ROS2 nodes to the bridge

#### What if I modify pj_ros_bridge for internal use?

**No obligations.** Internal modifications that are not distributed or provided as a service to external parties do not trigger any AGPL requirements. You can keep your internal improvements private.

#### What about the AGPL "network" clause?

The AGPL's network provision states that users who interact with the software over a network should have access to the source code. However, this only applies if you:

1. **Modify** the software, AND
2. **Provide it as a service** to external users

If you're using the **unmodified** version, there are no source code disclosure obligations, even if accessed over a network.

#### Why AGPL instead of a more permissive license?

The AGPL ensures that improvements to pj_ros_bridge benefit the entire community. If someone builds upon this work and shares it with others, those improvements remain open source. This creates a sustainable ecosystem where everyone contributes back.

For users who simply want to use the bridge (even commercially), the AGPL functions like any permissive license.

#### I'm still concerned about licensing. What should I do?

If you're using pj_ros_bridge **unmodified**, you have nothing to worry about - there are zero licensing obligations.

If you're planning to **modify and redistribute** it, the AGPL simply requires you to share those modifications. This is a reasonable trade-off that helps the open source community grow.

If still concerned, contact me for alternative licensing options.

## References

- [ROS2 Generic Subscription](https://api.nav2.org/rolling/html/generic__subscription_8hpp_source.html)
- [rosbag2 MCAP Storage](https://github.com/ros2/rosbag2/blob/rolling/rosbag2_storage_mcap/src/mcap_storage.cpp)
- [IXWebSocket](https://github.com/machinezone/IXWebSocket)
- [tl::expected](https://github.com/TartanLlama/expected)
