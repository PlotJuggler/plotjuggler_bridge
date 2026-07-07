^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package pj_ros_bridge
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.8.0 (2026-07-07)
------------------
* Foxglove-parity feature set, closing the gap with foxglove_bridge's
  topic-subscription features:
  - Topic whitelist: full-match regex filtering of visible/subscribable
    topics (``topic_whitelist`` / ``--topic-whitelist``)
  - QoS depth heuristics (ROS2): KEEP_LAST subscription depth derived from
    discovered publisher depths, clamped to ``min_qos_depth``/``max_qos_depth``
  - Opt-in pushed topic advertisement: ``subscribe_topic_updates`` /
    ``unsubscribe_topic_updates`` commands and a ``topics_changed``
    notification, polled at ``topic_poll_interval``
  - Slow-client backpressure: bounded, drop-oldest per-client send queue
    (``client_backlog_size``) instead of blocking the publish loop or
    disconnecting the client
  - Latched topic replay (ROS2): new subscribers to a ``TRANSIENT_LOCAL``
    topic (e.g. ``/tf_static``) immediately receive the retained last message
  - TLS (``wss://``): optional server-certificate TLS via OpenSSL
    (``tls``/``certfile``/``keyfile``, CMake option ``PJ_BRIDGE_TLS``)
* Demand-driven client support (PlotJuggler 4 per-topic subscriptions):
  - ``include_schemas`` opt-in on ``get_topics`` and ``subscribe_topic_updates``:
    topic entries gain ``encoding``/``definition`` so clients can classify
    topics BEFORE subscribing; per-topic schema failure keeps the topic
    listed (name+type only)
  - ``latched: true`` badge on ``get_topics`` / ``topics_changed`` entries
    when discovery knows every publisher offers ``TRANSIENT_LOCAL``
    (ROS2: live graph QoS query, no subscription needed); latched replay
    after the subscribe response is now a documented protocol guarantee
  - ``server`` object in ``get_topics`` responses: ``{name, version,
    capabilities[]}`` — clients feature-detect by capability name;
    ``protocol_version`` stays the only hard compatibility gate
* All changes are additive; ``protocol_version`` remains ``1``
* 245 unit tests passing (TSAN/ASAN clean)

0.1.0 (2026-02-11)
------------------
* **License changed to AGPL-3.0** (was Apache-2.0 in development)
  - Added comprehensive license FAQ to README
  - Clarifies commercial use, proprietary software compatibility
  - No restrictions on unmodified use
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
