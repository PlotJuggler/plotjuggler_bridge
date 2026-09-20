^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package pj_ros_bridge
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Forthcoming
-----------
* Message transforms (ROS2 only): operator-configured, off by default, a
  topic is transformed for all subscribers or none — no per-client
  negotiation. New ``transform_profile`` parameter points to a JSON file of
  ordered per-topic rules (``match_type`` exact and/or ``match_topic``
  full-match regex, first match wins). A transformed topic is advertised
  with its transform's output type/schema, plus an optional ``source_type``
  on ``get_topics`` entries (not yet on ``topics_changed`` entries); new
  ``message_transforms`` server capability.
* New ``cloudini`` transform: ``sensor_msgs/msg/PointCloud2`` ->
  ``point_cloud_interfaces/msg/CompressedPointCloud2`` using `Cloudini
  <https://github.com/facontidavide/cloudini>`_ point cloud compression
  (lossy at a configurable resolution; its own second compression stage is
  disabled since the bridge already ZSTD-compresses every frame). Only
  built when ``cloudini_lib`` is available (``find_package``, else fetched
  at configure time unless ``-DPJ_BRIDGE_FETCH_CLOUDINI=OFF``); a profile
  naming ``cloudini`` on a build without it fails at startup.
* ``strip_large_messages`` is now sugar for a ``strip`` transform rule
  appended after the profile's own rules (an explicit profile rule for the
  same type wins).
* **Behavior change:** a transform (including ``strip``) that fails on a
  message now drops the sample instead of forwarding it — previously
  ``strip_large_messages`` forwarded the original, unstripped message on
  failure.
* Per-topic transform statistics (samples, drops, compression ratio,
  microseconds/sample) logged with the final statistics at shutdown.
* The project now requires **C++20** (``std::span``, and ``cloudini_lib``'s
  PUBLIC ``cxx_std_20`` requirement).
* 301 unit tests passing.

0.10.0 (2026-09-20)
-------------------
* WebSocket permessage-deflate declined server-side (redundant with our own
  ZSTD compression; was costing CPU for no bandwidth benefit).
* ROS2: ingest executor polled and drained (``ingest_poll_interval_ms``,
  default 5 ms) instead of blocking ``spin()``, amortizing the per-wait-cycle
  wait-set rebuild; publish/request/timeout timers moved to their own
  executor thread so a long publish cycle never delays ingest.
* ROS2: ``min_qos_depth`` default raised ``1`` -> ``10`` so the ingest poll
  interval can't overflow a shallow reader between polls.
* Dependencies: IXWebSocket 11.4.6 -> 12.0.1 (FetchContent and .deb builds),
  Fast DDS 3.4.0 -> 3.4.3, CLI11 2.6.0 -> 2.6.2. Fixed the FastDDS/RTI
  link failure when IXWebSocket is fetched with TLS.
* CI and Debian release for ROS 2 Lyrical.
* AppImage: bundle the dlopen'd RMW providers and rosidl typesupport
  libraries, which ``ldd`` never reported — 0.9.0 AppImages aborted at
  startup on machines without ROS 2 (`#10`).
* Removed zero-initializing copies in the ingest and serializer hot paths
  (``resize()`` + ``memcpy`` -> direct-construct/``insert``).

0.9.0 (2026-07-11)
------------------
* Size-class frames: isolate heavy messages (``>= heavy_frame_threshold_bytes``)
  into their own binary frames so a single large message can no longer stall
  delivery of small, high-rate topics that would otherwise share a frame.
* Backpressure shedding: heavy frames are shed before transmit when a slow
  client's bounded send queue is under pressure, preserving liveness for the
  remaining topics instead of blocking the publish loop.
* ``heavy_frame_threshold_bytes`` exposed on all three backends (ROS2 param,
  RTI/FastDDS ``--heavy-frame-threshold-bytes``); ``0`` disables the split.
* Wire compatibility preserved: heavy frames are not wire-flagged, keeping the
  existing PlotJuggler plugin (which rejects non-zero frame flags) compatible.
* New ``size_class_frames`` capability advertised in the ``get_topics``
  ``server`` object.

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
