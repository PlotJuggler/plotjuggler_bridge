^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package pj_ros_bridge
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.1.0 (2026-02-11)
------------------
* Initial release of pj_ros_bridge
* WebSocket bridge server for ROS2 topics
* Features:
  - Generic subscription to any ROS2 topic
  - Message schema extraction from .msg files
  - 50 Hz aggregated message publishing over WebSocket
  - Binary ZSTD-compressed serialization format
  - Session management with heartbeat timeout
  - Message stripping for large arrays (Image, PointCloud2, etc.)
* Supports ROS2 Humble, Jazzy, Rolling, and Kilted
* Contributors:
  - davide
