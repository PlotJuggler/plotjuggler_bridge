"""Launch file that starts the bridge server and publishes test topics.

Publishes several geometry_msgs and std_msgs topics at various rates
so you can test the bridge without a rosbag.

Usage:
    ros2 launch pj_ros_bridge test_topics.launch.py
    ros2 launch pj_ros_bridge test_topics.launch.py port:=9090
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, ExecuteProcess
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    port_arg = DeclareLaunchArgument(
        "port", default_value="8080", description="WebSocket server port"
    )
    publish_rate_arg = DeclareLaunchArgument(
        "publish_rate",
        default_value="50.0",
        description="Aggregation publish rate (Hz)",
    )

    bridge_node = Node(
        package="pj_ros_bridge",
        executable="pj_ros_bridge_node",
        name="pj_ros_bridge",
        output="screen",
        parameters=[
            {
                "port": LaunchConfiguration("port"),
                "publish_rate": LaunchConfiguration("publish_rate"),
                "session_timeout": 10.0,
            }
        ],
    )

    # --- Test topic publishers using ros2 topic pub ---

    pose_pub = ExecuteProcess(
        cmd=[
            "ros2",
            "topic",
            "pub",
            "--rate",
            "10",
            "/test/pose",
            "geometry_msgs/msg/PoseStamped",
            "{header: {frame_id: 'map'}, pose: {position: {x: 1.0, y: 2.0, z: 3.0}, orientation: {w: 1.0}}}",
        ],
        output="screen",
    )

    twist_pub = ExecuteProcess(
        cmd=[
            "ros2",
            "topic",
            "pub",
            "--rate",
            "20",
            "/test/twist",
            "geometry_msgs/msg/TwistStamped",
            "{header: {frame_id: 'base_link'}, twist: {linear: {x: 0.5, y: 0.0, z: 0.0}, angular: {z: 0.1}}}",
        ],
        output="screen",
    )

    accel_pub = ExecuteProcess(
        cmd=[
            "ros2",
            "topic",
            "pub",
            "--rate",
            "50",
            "/test/accel",
            "geometry_msgs/msg/AccelStamped",
            "{header: {frame_id: 'imu_link'}, accel: {linear: {x: 0.0, y: 0.0, z: 9.81}, angular: {z: 0.01}}}",
        ],
        output="screen",
    )

    wrench_pub = ExecuteProcess(
        cmd=[
            "ros2",
            "topic",
            "pub",
            "--rate",
            "10",
            "/test/wrench",
            "geometry_msgs/msg/WrenchStamped",
            "{header: {frame_id: 'tool0'}, wrench: {force: {x: 1.0, y: 2.0, z: 5.0}, torque: {z: 0.5}}}",
        ],
        output="screen",
    )

    transform_pub = ExecuteProcess(
        cmd=[
            "ros2",
            "topic",
            "pub",
            "--rate",
            "5",
            "/test/transform",
            "geometry_msgs/msg/TransformStamped",
            "{header: {frame_id: 'world'}, child_frame_id: 'robot', transform: {translation: {x: 1.0, y: 2.0, z: 0.0}, rotation: {w: 1.0}}}",
        ],
        output="screen",
    )

    point_pub = ExecuteProcess(
        cmd=[
            "ros2",
            "topic",
            "pub",
            "--rate",
            "30",
            "/test/point",
            "geometry_msgs/msg/PointStamped",
            "{header: {frame_id: 'map'}, point: {x: 5.0, y: 3.0, z: 1.0}}",
        ],
        output="screen",
    )

    return LaunchDescription(
        [
            port_arg,
            publish_rate_arg,
            bridge_node,
            pose_pub,
            twist_pub,
            accel_pub,
            wrench_pub,
            transform_pub,
            point_pub,
        ]
    )
