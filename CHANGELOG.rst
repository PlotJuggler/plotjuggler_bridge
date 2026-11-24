^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package pj_ros_bridge
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Forthcoming
-----------
* The WebSocket server is now Boost.Beast (header-only, Boost >= 1.74), with
  OpenSSL for ``wss://``. One io thread serves every connection with
  asynchronous reads and writes, so a slow or stalled client costs nothing to
  the others; IXWebSocket needed two threads per client and has a data race
  in its own close path. New dependencies: Boost headers and OpenSSL
  (declared for rosdep, pixi, conda and Conan). The previous IXWebSocket
  transport can still be built with ``-DPJ_BRIDGE_WS_IX=ON`` for this release;
  it will be removed. IXWebSocket remains a test-only dependency.
* Large messages are sent in 64 KiB WebSocket fragments, so the server answers
  a client's Ping (and Close) between fragments instead of after a whole
  point cloud has crossed a slow link.
* A request larger than 1 MiB closes the connection (close code 1009);
  requests are small JSON commands.
* At most 128 connections are served at once, counting those still in the
  handshake (as IXWebSocket did); a further one is closed at once.
* TLS 1.0 and 1.1 are refused; TLS and WebSocket handshakes time out after 30 s.
* FastDDS/RTI: ``--client-backlog-size`` now defaults to 1000, as documented
  (it was still 100).
* Pixi: ROS 2 Lyrical environment, CI, AppImage and conda package.
* Slow clients no longer stall the bridge. IXWebSocket's server-side send
  blocks until the kernel takes every byte, and it ran on the thread that also
  handles requests, publishing and session timeouts: one client on a slow link
  delayed every other client and got healthy clients' sessions timed out.
  Each client now has its own outbound queue, drained without blocking.
* Bounded latency on slow links (`#19`): client sockets get
  ``TCP_NOTSENT_LOWAT`` (128 KiB) so the backlog stays in the bridge's queue
  instead of the kernel's send buffer, and a queued heavy frame is replaced by
  the newer one of the same topic. Before, a slow client received every large
  message in order and fell further behind without limit.
* A topic's large messages of one publish cycle now share one heavy frame
  (before: one frame per message), so a newer frame replaces only a whole
  older cycle: a client that keeps up gets every message even when
  ``publish_rate`` is below the topic's rate.
* Latched replays (``/tf_static``, ``/map``) are never dropped from a slow
  client's backlog; they are queued like replies.
* Requests a client sent just before disconnecting are discarded instead of
  re-creating its session once per request.
* A client is not timed out while its data keeps reaching the socket (its
  heartbeats can wait unread behind a long send on a slow link).
* ``pause`` discards the frames already queued for the client, so none arrives
  after the pause response.
* A queued heavy frame is sent after at most ``client_backlog_size`` normal
  frames, so busy small topics cannot starve it (never ahead of an earlier
  response); a replaced heavy frame keeps its place in line.
* ``client_backlog_size`` default raised from 100 to 1000 frames, and the
  small-frame queue is also capped at 32 MiB. 100 frames held only ~2 s at
  50 Hz, so on a slow link most small-topic data (e.g. ``/odom`` next to
  ``/tf``) was dropped while a point cloud was in flight, although it needed
  only a few KiB/s.
* Shutdown waits for connect/disconnect callbacks already running, so none
  outlives the bridge; a second concurrent ``shutdown()`` waits for the first.
* A client that sends requests without reading the replies is disconnected once
  64 MiB of replies and latched replays are queued for it (a single larger
  reply on an idle queue still goes out).

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
