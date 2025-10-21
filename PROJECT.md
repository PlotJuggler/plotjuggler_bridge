# General description

This is a C++ ROS2 package.

It's purpose is to implement a ROS2 bridge to forward the content of ROS2 Topics over the network, without DDS.
We will refer to this application either as the "bridge" or the "server".

The usual behavior is:

1. A client connect to the server and request the number of available topics. The server will answer with a JSON message that contains the name of the topic and its message type, for instance:

```json
{
    "topics": [
        {"name": "/points", "type": "sensor_msgs/msg/PointCloud2"},
        {"name": "/imu_data", "type": "sensor_msgs/msg/Imu.msg"}
    ]
}
```

2. The client will then request to subscribe to one or more of these topics.
The server will reply with a confirmation and with the schema of each of these topics, serialized using JSON.

3. At this point, the server will subscribe to the ROS2 topics using the generic subscription.
(See: https://api.nav2.org/rolling/html/generic__subscription_8hpp_source.html)

4. The row content of these messages will be stored in a rolling buffer. At a rate of 50 Hz,
these messages will be aggregated in a single large message and sent to the client.

5. The client can change the number of topics it subscribes to at any moment, sending the same message as
described in point 2.

6. The client should also send a heartbeat message once a second. If the server doesn't receive the heartbeat for
10 seconds, it will unsubscribe from the topics that the client requested.

We may have more than one client connecting, therefore we need to consider different "sessions".
If two clients request to subscribe to the same ROS2 topic, we should not subscribe twice, nor create a duplicated rolling buffer.


## How to serialize the messages

We are aggregating multiple DDS messages into a single one.
This aggregated message contains:

- Then number of messages
- For each of them:
     - The name of the topic (string is serialized as 2 bytes per length N, followed by the N characters)
     - The time when it was published (uint64 time since epoch in nanoseconds)
     - The time when it was received (uint64 time since epoch in nanoseconds)
     - The serialized message buffer, as 4 bytes length, followed by the buffer itself

We will serialize this ourself, no need to use any external library.

**Compression**: The aggregated message should be compressed using ZSTD before being published to the client via the PUB-SUB socket. This reduces network bandwidth and improves performance when dealing with large messages.

## Middleware

We will use ZeroMQ, but we will include an **abstract class** to allow us to change this to a different middleware in the future, if we need to.

We will use a REQ-REP pattenr for the API between trhe client and the server, and a PUB-SUB for the aggregated messages at 50 Hz published by the server.

## How to obtain the schemas

The schemas are obtained by reading .msg files directly from ROS2 package share directories.

We use `ament_index_cpp::get_package_share_directory()` to locate the package, then read the .msg file from `<package_share>/msg/<MessageType>.msg`. Nested message types are recursively expanded using depth-first traversal.

This approach is similar to rosbag2 MCAP storage implementation:
https://github.com/ros2/rosbag2/blob/rolling/rosbag2_storage_mcap/src/mcap_storage.cpp

**Key dependencies for this approach:**
- `ament_index_cpp` - to locate package share directories
- Access to installed ROS2 packages containing .msg files


## How to test it

During the development, we want to test periodically some feature.

We will play a rosbag to start publishing topics that the server can subscribe to. We will use a dumy client written in Python, for simplicity.
The name of the topics to subscribe to will be passed to the client as command line arguments.
