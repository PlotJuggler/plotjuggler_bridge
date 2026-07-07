# Foxglove-parity Features Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the six features in `docs/superpowers/specs/2026-07-06-foxglove-parity-design.md`: topic whitelist, QoS depth heuristics, pushed topic advertisement, slow-client backpressure, transient_local replay, TLS.

**Architecture:** Backend-agnostic changes go in `app/` (core, no ROS deps); ROS2-specific changes in `ros2/`. All protocol changes are additive (protocol_version stays 1). One commit per task.

**Tech Stack:** C++17, gtest, nlohmann/json, IXWebSocket 11.4.6, ZSTD, spdlog, rclcpp (Humble).

**Build/test (from the worktree root `.worktrees/foxglove-parity`):**
```bash
pixi run -e humble build    # colcon build (Release)
pixi run -e humble test     # colcon test + result; baseline = 177 tests green
```
Format before every commit: `pre-commit run -a` (clang-format). Follow `.clang-tidy` naming: classes `CamelCase`, methods `lower_case`, members `trailing_underscore_`, constants `kCamelCase`. Core code logs via `spdlog::*`, never `RCLCPP_*` (except `ros2/` adapter code). New files carry the AGPL header found at the top of every existing file (copy from `app/src/bridge_server.cpp:1-18`).

**Key existing files (read before coding):**
- `app/include/pj_bridge/bridge_server.hpp` + `app/src/bridge_server.cpp` — orchestrator; request dispatch at `bridge_server.cpp:182-196`; subscribe validation loop at `:299-327`; publish/fan-out loop at `:677-827`; lock ordering documented at `bridge_server.hpp:163-166` (`cleanup_mutex_ > last_sent_mutex_ > stats_mutex_`).
- `app/include/pj_bridge/session_manager.hpp` — per-session state (pause flag, ref-held topics) — mimic its patterns for new per-session flags.
- `app/include/pj_bridge/message_buffer.hpp` + `.cpp` — TTL buffer; injectable `ClockFn` for tests.
- `app/include/pj_bridge/middleware/websocket_middleware.hpp` + `app/src/middleware/websocket_middleware.cpp` — IXWebSocket server; per-client `ix::WebSocket` map `clients_`.
- `ros2/src/generic_subscription_manager.cpp:28-61` — `adapt_qos()`.
- `ros2/src/main.cpp:39-47` — ROS2 param declarations; `fastdds/src/main.cpp` — CLI11 flags; `app/src/standalone_event_loop.cpp` — FastDDS/RTI event loop.
- `tests/unit/test_bridge_server.cpp` — has mock `TopicSourceInterface` / `SubscriptionManagerInterface` / `MiddlewareInterface` implementations; extend those mocks, don't invent new ones.

---

### Task 1: Topic whitelist regex

**Files:**
- Create: `app/include/pj_bridge/whitelist_filter.hpp`, `app/src/whitelist_filter.cpp`
- Create: `tests/unit/test_whitelist_filter.cpp`
- Modify: `app/include/pj_bridge/bridge_server.hpp` (ctor param + member), `app/src/bridge_server.cpp` (`handle_get_topics`, `handle_subscribe`)
- Modify: `ros2/src/main.cpp`, `fastdds/src/main.cpp`, `rti/src/main.cpp` (params/flags)
- Modify: `CMakeLists.txt` (add new .cpp to `pj_bridge_app` and test target), `docs/API.md`
- Test: `tests/unit/test_whitelist_filter.cpp`, extend `tests/unit/test_bridge_server.cpp`

- [ ] **Step 1: Write failing tests for `WhitelistFilter`** — construction from patterns, ECMAScript `std::regex_match` (full match: pattern `/cam` must NOT match `/camera`), default-constructed filter matches everything, `create()` returns `tl::unexpected` with the offending pattern for invalid regex (e.g. `"("`).

Class contract:
```cpp
class WhitelistFilter {
 public:
  WhitelistFilter() = default;  // matches everything
  static tl::expected<WhitelistFilter, std::string> create(const std::vector<std::string>& patterns);
  bool matches(const std::string& topic_name) const;
 private:
  std::vector<std::regex> patterns_;  // empty = match all
};
```

- [ ] **Step 2: Run the new test, verify it fails to compile/link.**
- [ ] **Step 3: Implement `WhitelistFilter`** (compile with `std::regex::ECMAScript`; `matches` = any pattern full-matches; add file to `pj_bridge_app` sources in CMakeLists.txt).
- [ ] **Step 4: Run tests, verify green.**
- [ ] **Step 5: Write failing BridgeServer tests**: (a) `get_topics` response omits non-whitelisted topics; (b) `subscribe` to a non-whitelisted topic yields a per-topic failure with `"reason": "Topic not whitelisted"` (and `ALL_SUBSCRIPTIONS_FAILED` when it was the only topic); (c) whitelisted topics still subscribe fine.
- [ ] **Step 6: Integrate**: add `WhitelistFilter whitelist = {}` as the **last** BridgeServer ctor param (existing call sites unaffected). In `handle_get_topics` skip non-matching topics; in `handle_subscribe` add the whitelist check at the top of the per-topic validation loop (`bridge_server.cpp:299`), before the existence check.
- [ ] **Step 7: Run tests, verify green.**
- [ ] **Step 8: Wire config**: ROS2 string-array param `topic_whitelist` default `[".*"]` (declare next to `ros2/src/main.cpp:42`); CLI11 repeatable `--topic-whitelist` default `{".*"}` in `fastdds/src/main.cpp` and `rti/src/main.cpp`. On `WhitelistFilter::create()` error: log the message and exit 1. Document the param and the full-match semantics in `docs/API.md` (get_topics + subscribe sections).
- [ ] **Step 9: Full build + test + `pre-commit run -a`.**
- [ ] **Step 10: Commit** `feat: add topic whitelist regex filtering`.

### Task 2: QoS depth heuristics (ROS2)

**Files:**
- Modify: `ros2/include/pj_bridge_ros2/generic_subscription_manager.hpp`, `ros2/src/generic_subscription_manager.cpp` (`adapt_qos`), `ros2/include/pj_bridge_ros2/ros2_subscription_manager.hpp` + `.cpp` (pass-through), `ros2/src/main.cpp`
- Test: extend `tests/unit/test_generic_subscription_manager.cpp`

- [ ] **Step 1: Write failing tests**: publisher with depth 10 → subscription depth ≥ 10; two publishers depth 60 each → depth clamped to `max_qos_depth` (100); no publishers → depth = `max_qos_depth`? No — **no publishers → keep depth = max(min_qos_depth, 100 default path unchanged)**; precise rule below. Test via `node->create_publisher` with explicit `rclcpp::QoS(depth)` and inspecting the returned `rclcpp::QoS` from `adapt_qos` (make `adapt_qos` public or keep tests as friend — it is already `const` public in the header; check).

Depth rule (adapted from foxglove_bridge `determineQoS()` — add attribution comment in the file: *"Depth aggregation adapted from foxglove_bridge (MIT License, Copyright (c) Foxglove Technologies Inc)"*):
```
total = sum over publishers of profile.depth()   // depth 0 (unknown/KEEP_ALL) contributes 0
depth = clamp(total, min_qos_depth, max_qos_depth)
if publishers.empty(): depth = min(100, max_qos_depth)  // preserve current default behavior
```
- [ ] **Step 2: Run tests, verify failure.**
- [ ] **Step 3: Implement**: `GenericSubscriptionManager` ctor gains `size_t min_qos_depth = 1, size_t max_qos_depth = 100`; `Ros2SubscriptionManager` forwards them; reliability/durability logic at `generic_subscription_manager.cpp:39-58` unchanged.
- [ ] **Step 4: Run tests, verify green.**
- [ ] **Step 5: Wire params** `min_qos_depth` (int, 1) and `max_qos_depth` (int, 100) in `ros2/src/main.cpp`; document in `docs/API.md` config section and README parameter table.
- [ ] **Step 6: Full build + test + format. Commit** `feat: aggregate publisher QoS depths with min/max clamping`.

### Task 3: Pushed topic advertisement (opt-in)

**Files:**
- Modify: `app/include/pj_bridge/session_manager.hpp` + `app/src/session_manager.cpp` (per-session `wants_topic_updates` flag, default false, following the existing `paused` flag pattern)
- Modify: `app/include/pj_bridge/bridge_server.hpp` + `app/src/bridge_server.cpp`
- Modify: `ros2/src/main.cpp`, `app/include/pj_bridge/standalone_event_loop.hpp` + `app/src/standalone_event_loop.cpp`, `fastdds/src/main.cpp`, `rti/src/main.cpp`
- Modify: `docs/API.md`
- Test: extend `tests/unit/test_bridge_server.cpp`, `tests/unit/test_session_manager.cpp`

- [ ] **Step 1: Write failing SessionManager tests** for `set_topic_updates(client_id, bool)` / `wants_topic_updates(client_id)` (default false; false for unknown client).
- [ ] **Step 2: Implement the flag. Tests green.**
- [ ] **Step 3: Write failing BridgeServer tests**:
  - `{"command": "subscribe_topic_updates"}` → `{"status": "ok", "topic_updates": true, ...}`; `unsubscribe_topic_updates` → `topic_updates: false`. Both idempotent, both create a session if needed (copy the pattern from `handle_pause`).
  - New public method `void check_topic_changes()`: first call snapshots silently (no notification); after the mock topic source adds `/new` and drops `/old`, the **opted-in** client receives exactly one text frame `{"notification": "topics_changed", "added": [{"name": "/new", "type": "..."}], "removed": ["/old"], "protocol_version": 1}`; a non-opted-in client receives nothing; no change → no notification; non-whitelisted topics (Task 1 filter) never appear in the diff.
- [ ] **Step 4: Implement**: dispatch the two commands in `process_single_request` (`bridge_server.cpp:182-196`); `check_topic_changes()` diffs `topic_source_->get_topics()` (whitelist-filtered) against member `std::unordered_map<std::string, std::string> known_topics_` guarded by a new `topics_mutex_` (leaf lock — never held while taking any existing lock); send via `middleware_->send_reply()` to each session with the flag.
- [ ] **Step 5: Tests green.**
- [ ] **Step 6: Wire timers**: ROS2 param `topic_poll_interval` (double, default 1.0, `0` disables) + wall timer in `ros2/src/main.cpp`; add the same interval arg to `run_standalone_event_loop()` (follow how `check_session_timeouts` is scheduled there) + `--topic-poll-interval` CLI flag in `fastdds/src/main.cpp` / `rti/src/main.cpp`. Document command, notification, and param in `docs/API.md`.
- [ ] **Step 7: Full build + test + format. Commit** `feat: opt-in pushed topic advertisement (topics_changed notification)`.

### Task 4: Slow-client backpressure (drop-oldest)

**Files:**
- Create: `app/include/pj_bridge/middleware/bounded_frame_queue.hpp` (header-only helper)
- Create: `tests/unit/test_bounded_frame_queue.cpp`
- Modify: `app/include/pj_bridge/middleware/websocket_middleware.hpp` + `app/src/middleware/websocket_middleware.cpp`
- Modify: `ros2/src/main.cpp`, `fastdds/src/main.cpp`, `rti/src/main.cpp` (`client_backlog_size` param/flag)
- Test: `tests/unit/test_bounded_frame_queue.cpp`, extend `tests/unit/test_websocket_middleware.cpp`

- [ ] **Step 1: Write failing tests for `BoundedFrameQueue`** (pure, no sockets — this is what makes the policy unit-testable): `push` returns number of dropped frames (0 normally); pushing past capacity drops the **oldest**; `pop_front`/`empty`/`size`; `dropped_total()` accumulates.

```cpp
class BoundedFrameQueue {
 public:
  explicit BoundedFrameQueue(size_t max_frames);
  size_t push(std::vector<uint8_t> frame);  // returns #dropped (0 or 1)
  std::optional<std::vector<uint8_t>> pop_front();
  bool empty() const;  size_t size() const;  uint64_t dropped_total() const;
};
```
- [ ] **Step 2: Implement; tests green.**
- [ ] **Step 3: Integrate into `WebSocketMiddleware`**: ctor gains `size_t client_backlog_size = 100`; per-client `BoundedFrameQueue` map guarded by `clients_mutex_` (erased on disconnect alongside `clients_`). New `send_binary` logic, adapted from foxglove_bridge's lossy send policy (MIT attribution comment):
  1. If `ws->bufferedAmount() < kSocketBufferHighWatermark` (constant, `1u << 20` = 1 MiB): flush queued frames in order via `sendBinary`, then send the new frame.
  2. Else enqueue; on drop, warn at most once per 30 s per client (`kDropWarnIntervalSeconds = 30`), including the cumulative drop count.
  - JSON replies (`send_reply`) are untouched — direct send only.
  - Add `uint64_t dropped_frame_count() const` (sum across clients) to `WebSocketMiddleware` (NOT to `MiddlewareInterface`); print it in the `--stats` output of `fastdds/src/main.cpp`.
- [ ] **Step 4: Extend `test_websocket_middleware.cpp`**: normal send path still works end-to-end (existing loopback tests keep passing); `dropped_frame_count()` starts at 0.
- [ ] **Step 5: Full build + test + format. Also run TSAN** (new mutex interactions):
```bash
pixi run -e humble bash -c 'colcon build --packages-select pj_bridge --build-base build_tsan --install-base install_tsan --cmake-args -DCMAKE_BUILD_TYPE=Release -DENABLE_TSAN=ON && source install_tsan/setup.bash && TSAN_OPTIONS="suppressions=$(pwd)/tsan_suppressions.txt" setarch $(uname -m) -R build_tsan/pj_bridge/pj_bridge_tests'
```
(exit code 66 = pre-existing suppressed IXWebSocket warnings, acceptable; NEW data-race reports are not).
- [ ] **Step 6: Commit** `feat: bounded per-client send queue with drop-oldest backpressure`.

### Task 5: transient_local replay (ROS2, latest message)

**Files:**
- Modify: `app/include/pj_bridge/subscription_manager_interface.hpp` (new virtual with default)
- Modify: `ros2/include/pj_bridge_ros2/generic_subscription_manager.hpp` + `.cpp`, `ros2/include/pj_bridge_ros2/ros2_subscription_manager.hpp` + `.cpp`
- Modify: `app/include/pj_bridge/message_buffer.hpp` + `app/src/message_buffer.cpp`
- Modify: `app/src/bridge_server.cpp` (`initialize` callback + `handle_subscribe`)
- Modify: `docs/API.md`
- Test: extend `tests/unit/test_message_buffer.cpp`, `tests/unit/test_bridge_server.cpp`, `tests/unit/test_generic_subscription_manager.cpp`

- [ ] **Step 1: Interface**: add to `SubscriptionManagerInterface`:
```cpp
/// True when every publisher of the topic offers TRANSIENT_LOCAL durability
/// (detected at subscribe time). Backends without this info return false.
virtual bool is_transient_local(const std::string& /*topic_name*/) const { return false; }
```
Non-pure with default → FastDDS/RTI/mocks unchanged.
- [ ] **Step 2: Write failing tests**: `GenericSubscriptionManager::is_transient_local` true after subscribing to a topic whose (test) publisher is transient_local, false otherwise (store the `all_transient_local` result computed in `adapt_qos` into `SubscriptionInfo` at subscribe time — `generic_subscription_manager.cpp:80-82`).
- [ ] **Step 3: Implement; tests green.** (`Ros2SubscriptionManager` forwards to the wrapped manager.)
- [ ] **Step 4: Write failing MessageBuffer tests**: `set_latched("/t", true)` → after `add_message` + TTL expiry (use injectable `ClockFn`) + `move_messages`, `get_latched("/t")` still returns the newest message; `set_latched("/t", false)` clears it; non-latched topics return `nullopt`; `clear()` clears latched storage too.

New API on MessageBuffer:
```cpp
void set_latched(const std::string& topic_name, bool latched);
std::optional<BufferedMessage> get_latched(const std::string& topic_name) const;
```
Implementation: separate `std::unordered_map<std::string, BufferedMessage> latched_last_` + `std::unordered_set<std::string> latched_topics_`, updated inside `add_message` (normal queue behavior unchanged; `move_messages` does NOT touch the latched store). Retained entries persist until `set_latched(false)` or `clear()` — bounded at one message per latched topic.
- [ ] **Step 5: Implement; tests green.**
- [ ] **Step 6: Write failing BridgeServer test**: mock sub manager reports `is_transient_local("/latched") == true`; first client subscribes, a message arrives, TTL passes; a **second** client subscribes → that client immediately receives a single-message binary frame containing the retained message (decode it with the test helper already used by binary-frame assertions in `test_bridge_server.cpp`); the first client receives nothing extra.
- [ ] **Step 7: Implement in BridgeServer**:
  - In `handle_subscribe`, after a successful `subscription_manager_->subscribe(...)` (`bridge_server.cpp:340-348`): call `message_buffer_->set_latched(topic, subscription_manager_->is_transient_local(topic))`.
  - Still inside the success path: if latched and `get_latched(topic)` has a value, serialize that one message with `AggregatedMessageSerializer` and `middleware_->send_binary(client_id, frame)` immediately (rate-limit state untouched — replay bypasses it by construction).
  - Rationale (comment it): a brand-new first subscription receives the sample from DDS naturally; the replay path covers later clients joining an already-shared subscription whose sample aged out of the TTL buffer.
- [ ] **Step 8: Full build + test + format. Commit** `feat: replay latest transient_local message to late subscribers (ROS2)`.

### Task 6: TLS (OpenSSL, server cert only)

**Files:**
- Modify: `CMakeLists.txt` (option `PJ_BRIDGE_TLS` default ON; FetchContent branch: `set(USE_TLS ${PJ_BRIDGE_TLS} ...)` + `set(USE_OPEN_SSL ON ...)` replacing the hardcoded `USE_TLS OFF` at `CMakeLists.txt:64`)
- Modify: `app/include/pj_bridge/middleware/websocket_middleware.hpp` + `app/src/middleware/websocket_middleware.cpp`
- Modify: `ros2/src/main.cpp`, `fastdds/src/main.cpp`, `rti/src/main.cpp`, `docs/API.md`, `.github` CI if OpenSSL dev headers are missing (pixi.toml `openssl` dependency if needed)
- Test: extend `tests/unit/test_websocket_middleware.cpp`

- [ ] **Step 1: CMake**: flip the FetchContent flags; verify a clean configure+build passes locally (OpenSSL comes from the pixi env; add `openssl` to pixi.toml dependencies if configure fails to find it).
- [ ] **Step 2: Write failing tests**: `WebSocketMiddleware` constructed with `TlsConfig{"/nonexistent.pem", "/nonexistent.key"}` → `initialize()` returns an error mentioning the missing file; TLS round-trip test (guarded by `#ifdef IXWEBSOCKET_USE_TLS`): generate a self-signed cert in the test fixture via
```bash
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 1 -nodes -subj "/CN=localhost"
```
(into a temp dir; `std::system` in the fixture), start middleware with TLS, connect an `ix::WebSocket` client with `tlsOptions.caFile = "NONE"` (disables verification), assert echo works.
- [ ] **Step 3: Implement**:
```cpp
struct TlsConfig { std::string certfile; std::string keyfile; };
explicit WebSocketMiddleware(size_t client_backlog_size = 100, std::optional<TlsConfig> tls = std::nullopt);
```
In `initialize()`: if TLS requested — fail with a clear message when the files don't exist/aren't readable, or when built without TLS (`#ifndef IXWEBSOCKET_USE_TLS` → error "IXWebSocket built without TLS support"); otherwise `ix::SocketTLSOptions opts; opts.tls = true; opts.certFile = ...; opts.keyFile = ...; opts.caFile = "NONE"; server_->setTLSOptions(opts);` before `listen()`.
- [ ] **Step 4: Tests green.**
- [ ] **Step 5: Wire config**: ROS2 params `tls` (bool, false), `certfile` (""), `keyfile` (""); `tls=true` with missing params → error + exit 1. CLI: `--certfile` + `--keyfile` (both or neither; CLI11 `needs()`), presence implies TLS. Document in `docs/API.md` + README (`wss://` URL note).
- [ ] **Step 6: Full build + test + format. Commit** `feat: optional TLS (wss://) via OpenSSL server certificate`.

### Task 7: Documentation & changelog sweep

**Files:**
- Modify: `README.md` (feature list + parameter tables), `CLAUDE.md` (config section, test count), `CHANGELOG.rst`, `docs/API.md` (final consistency pass)

- [ ] **Step 1**: Update README parameter tables (all new params/flags for all backends), CLAUDE.md configuration + test-count sections, CHANGELOG.rst entry for the six features.
- [ ] **Step 2**: Re-read `docs/API.md` top-to-bottom for consistency (every new command/notification/param documented exactly once, examples valid JSON).
- [ ] **Step 3: Full build + test + TSAN + ASAN**:
```bash
pixi run -e humble bash -c 'colcon build --packages-select pj_bridge --build-base build_asan --install-base install_asan --cmake-args -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=ON && source install_asan/setup.bash && ASAN_OPTIONS="new_delete_type_mismatch=0" LSAN_OPTIONS="suppressions=$(pwd)/asan_suppressions.txt" build_asan/pj_bridge/pj_bridge_tests'
```
- [ ] **Step 4: Commit** `docs: document foxglove-parity features`.

---

## Verification (whole plan)

1. `pixi run -e humble build && pixi run -e humble test` — all tests green (baseline 177 + new).
2. TSAN and ASAN runs clean (modulo documented pre-existing suppressions).
3. `git log --oneline origin/main..HEAD` shows one commit per task + spec/plan docs.
4. `docs/API.md` documents: `topic_whitelist`, `min_qos_depth`/`max_qos_depth`, `subscribe_topic_updates`/`unsubscribe_topic_updates` + `topics_changed`, `client_backlog_size`, `topic_poll_interval`, `tls`/`certfile`/`keyfile`.
