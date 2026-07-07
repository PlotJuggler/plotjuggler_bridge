# Foxglove-parity features for pj_bridge — Design

**Date:** 2026-07-06
**Status:** Approved
**Scope:** Six features closing the gap with foxglove_bridge on the topic-subscription
feature set, identified in a side-by-side comparison of the two bridges.

## Background

pj_bridge leads foxglove_bridge on bandwidth efficiency (ZSTD-compressed aggregated
frames, per-topic rate limiting, subscription-group fan-out) but lacks several
robustness/deployment features foxglove_bridge has. This design adds them. Where logic
is adapted from foxglove_bridge (MIT license), the affected file carries a copyright
attribution notice; MIT is compatible with this repo's AGPL-3.0.

All protocol changes are **additive**: `protocol_version` stays `1`. Old clients are
unaffected (no new unsolicited traffic unless a client opts in).

## Feature 1 — Topic whitelist regex

- New `WhitelistFilter` class in `app/` holding a list of compiled `std::regex`
  (ECMAScript grammar, `std::regex_match` = full match, mirroring foxglove's
  `isWhitelisted`).
- `BridgeServer` receives the filter via its config and applies it in two places:
  1. `get_topics` responses — non-matching topics are omitted.
  2. `subscribe` requests — non-whitelisted topics fail with reason
     `"Topic not whitelisted"` (per-topic failure, consistent with the existing
     partial_success model).
- Config: ROS2 string-array param `topic_whitelist` (default `[".*"]`);
  FastDDS/RTI CLI flag `--topic-whitelist` (repeatable, default `.*`).
- Invalid regex at startup = fatal error with a clear message.

## Feature 2 — QoS depth heuristics (ROS2 only)

- Extend `adapt_qos()` in `GenericSubscriptionManager`:
  `depth = clamp(sum(publisher depths), min_qos_depth, max_qos_depth)`.
  Publishers reporting depth 0 (unknown/KEEP_ALL) contribute 0, as in foxglove.
- Defaults: `min_qos_depth = 1`, `max_qos_depth = 100` (preserves the current
  effective ceiling of `rclcpp::QoS(100)`).
- Reliability and durability heuristics unchanged.
- Logic adapted from foxglove_bridge `determineQoS()` — attribution comment in file.
- Config: ROS2 params `min_qos_depth`, `max_qos_depth`.

## Feature 3 — Pushed topic advertisement (opt-in per client)

- New commands:
  - `{"command": "subscribe_topic_updates"}` → `{"status": "ok", ...}`; sets a
    per-session flag.
  - `{"command": "unsubscribe_topic_updates"}` → clears it. Both idempotent.
- New server-initiated notification, sent **only** to opted-in clients whenever the
  topic set changes:

  ```json
  {"notification": "topics_changed",
   "added": [{"name": "/t", "type": "pkg/msg/T"}],
   "removed": ["/gone"],
   "protocol_version": 1}
  ```

  `added`/`removed` may be empty arrays; a notification is sent only when at least
  one of them is non-empty. The whitelist (Feature 1) is applied before diffing.
- Detection is backend-agnostic: `BridgeServer::check_topic_changes()` diffs
  `TopicSourceInterface::get_topics()` against the previous snapshot. Entry points
  drive it on a timer: new param/flag `topic_poll_interval` (seconds, default `1.0`;
  `0` disables polling and the feature).

## Feature 4 — Slow-client backpressure (foxglove-style)

Policy per user decision: bounded per-client queue, **drop-oldest** on overflow,
throttled warnings, never disconnect for data-plane pressure.

- Implemented entirely in `WebSocketMiddleware`; interfaces unchanged.
- Per-client bounded `std::deque` of pending **binary** frames in front of the socket:
  - `send_binary()`: if the client's `ix::WebSocket::bufferedAmount()` is below a
    1 MiB high-watermark (`kSocketBufferHighWatermark`), flush the pending queue in
    order, then send the new frame; otherwise enqueue the frame.
  - On enqueue past `client_backlog_size` (default 100 frames ≈ 2 s at 50 Hz), drop
    the **oldest** queued frame and increment a per-client dropped-frame counter.
  - Warning log throttled to one per 30 s per client (foxglove's cadence).
- JSON replies (`send_reply`) are control-plane: direct send, never queued or dropped.
- Config: ROS2 param / CLI flag `client_backlog_size`.
- Dropped-frame totals exposed via the existing stats path.

## Feature 5 — transient_local replay (ROS2, latest message)

- `adapt_qos()` already detects the all-publishers-transient_local case; the
  subscription manager records it and exposes
  `bool is_transient_local(const std::string& topic) const` on
  `SubscriptionManagerInterface`. FastDDS/RTI implementations return `false` (defer).
- `MessageBuffer` gains a per-topic *latched* mode (`set_latched(topic, bool)`):
  the most recent message for a latched topic is exempt from TTL eviction — a
  one-slot durable cache. It is cleared when the topic's subscription is destroyed.
- On a client `subscribe` to a latched topic, the retained message is force-included
  in that client's next binary frame, bypassing the per-topic rate-limit timestamp
  check exactly once (so `/tf_static`-style topics appear immediately even though
  their message timestamp is old).

## Feature 6 — TLS (OpenSSL, server cert only)

- CMake: new option `PJ_BRIDGE_TLS` (default `ON`). The IXWebSocket FetchContent path
  sets `USE_TLS=ON` and `USE_OPEN_SSL=ON`. If a system IXWebSocket without TLS is
  found and `tls` is requested at runtime, startup fails with a clear error.
- `WebSocketMiddleware` constructor takes `std::optional<TlsConfig>{certfile, keyfile}`
  → mapped to `ix::SocketTLSOptions`. `MiddlewareInterface` unchanged.
- Config: ROS2 params `tls` (bool, default false), `certfile`, `keyfile`;
  FastDDS/RTI CLI `--certfile`/`--keyfile` (presence of both implies TLS).
- Missing or unreadable cert/key files fail startup with a clear error.
- Tests generate a self-signed cert via the `openssl` CLI in test setup.

## Cross-cutting

- `docs/API.md`: document the new commands, notification, and params.
- README/CLAUDE.md configuration tables updated.
- Every feature lands with unit tests in the existing gtest suites' style; the full
  suite must stay TSAN/ASAN clean.
- One commit per feature on branch `feature/foxglove-parity`; single PR.

## Non-goals

- FastDDS/RTI transient_local replay (interface stub only).
- mTLS / client certificates.
- Per-publisher history-depth replay caches (latest-message only).
- Any change to the binary frame format or protocol version.
