# CPU Optimization — Findings and Next Steps

Status: **analysis only, no code changed.** This is a working document for a
profiling pass on a machine with ROS2 installed.

Baseline commit: `f344cca` (0.9.0).

## How to read this document

Every claim is tagged with how it was established:

- **[measured]** — benchmarked on this tree's exact code shapes. The
  benchmark sources are in [`docs/perf/`](./perf) so the numbers can be
  reproduced and extended.
- **[derived]** — arithmetic on top of a measured number (e.g. scaling a
  per-frame cost to a frame rate).
- **[inferred]** — reasoned from the code plus known rclcpp/rmw behavior, and
  **not verified**. Everything ROS2-specific in §2 is in this category, because
  the analysis environment had no ROS2 installation. Treat these as hypotheses
  to confirm with `perf`, not as conclusions.

Measurements were taken on a 4-core Intel Xeon @ 2.10 GHz, g++ 13.3.0 `-O2`,
libzstd 1.5.5. Absolute values will differ on other hardware; the *ratios*
should hold.

**Percentages of "one core" assume a 50 Hz publish rate** (the default) unless
stated otherwise.

---

## 1. Backend-agnostic findings

### 1.1 Tier 1 — structural

#### FastDDS ingest does a full XTypes round-trip it never needs

`fastdds/src/fastdds_subscription_manager.cpp:73-84`

Every sample is deserialized into `DynamicData` (walking the type tree with
per-member virtual dispatch), then immediately `calculate_serialized_size()` +
`serialize()`'d **back into the CDR bytes it arrived as**, then memcpy'd into
the output buffer. The bridge never inspects message content — it forwards
opaque CDR.

**[inferred]** This is the largest avoidable cost in the FastDDS backend,
plausibly 5–20× the ingest cost of a raw byte copy. Not measured: no Fast DDS
in the analysis environment.

Direction: register a trivial `RawPayloadPubSubType` whose `deserialize()`
copies `payload.data[0..length]` straight into the output buffer — the
rosbag2 / foxglove generic-reader pattern. One copy, no type walking. The CDR
encapsulation header is preserved either way, so the wire output is unchanged.
Also prefer `take()` over `take_next_sample()` in a loop to amortize reader
overhead.

#### The standalone event loop burns ~1.8% of a core doing nothing

`app/src/standalone_event_loop.cpp:113`

`sleep_for(1ms)` produces ~900 wakeups/s to drive a 50 Hz job, each taking two
mutexes in `process_requests()` to poll an empty queue.

**[measured]** (`docs/perf/bench_loop.cpp`)

```
today  (1 ms sleep) :  1.84% of one core idle   (911 wakeups/s)
deadline-driven     :  0.20% of one core idle   ( 50 wakeups/s)
```

Direction: sleep until the earliest next deadline (publish / timeout /
topic-poll), and have `WebSocketMiddleware` signal a condition variable when a
request is enqueued so the loop wakes on demand. **9× lower idle floor** for
the RTI and FastDDS backends. Affects those two backends only — the ROS2
backend uses rclcpp timers (see §2.2 for its equivalent problem).

#### Subscription grouping rebuilds a `std::map` per client, per cycle

`app/src/bridge_server.cpp:1027-1043`

Each publish cycle deep-copies every client's subscription map
(`get_subscriptions` returns by value), copies it *again* into a
`std::map<std::string,int>` group key, then does O(log n) map-vs-map
comparisons that compare topic strings one at a time.

**[measured]** (`docs/perf/bench_hotpath.cpp`)

```
 100 topics x 1 client  : 0.015 ms/cycle ->  0.1% of a core @50Hz
 500 topics x 1 client  : 0.144 ms/cycle ->  0.7%
 500 topics x 4 clients : 0.553 ms/cycle ->  2.8%
2000 topics x 2 clients : 1.375 ms/cycle ->  6.9%
```

None of this work depends on the messages — it is pure per-cycle bookkeeping
that scales with (clients × topics).

Direction: compute a 64-bit signature over the (topic, rate_mhz) set **once,
when a subscription changes**, cache it on the `Session`, and group by
`unordered_map<uint64_t, ...>`.

Related: `bridge_server.cpp:1150` copies the whole per-topic `last_sent_times_`
map to every non-representative client in a group, every cycle. By
construction those maps are identical within a group — one shared entry per
group removes the copy entirely.

### 1.2 Tier 2 — per-message and per-frame copies

#### Every frame is copied into a `std::string` per client before sending

`app/src/middleware/websocket_middleware.cpp:348` (also `:298`, `:452`)

**[measured]** (`docs/perf/bench_loop.cpp`)

```
frame  256 KiB x 4 clients : 0.027 ms/frame -> 0.1% of a core @50Hz
frame    2 MiB x 4 clients : 0.592 ms/frame -> 3.0% of a core @50Hz
```

The pinned IXWebSocket **v11.4.6 already has the fix**. Verified in the tagged
source:

- `IXWebSocket.h:84` — `WebSocketSendInfo sendBinary(const IXWebSocketSendData& data, ...)`
- `IXWebSocketSendData.h` — has a `IXWebSocketSendData(const std::vector<uint8_t>&)`
  constructor that wraps pointer+size without copying.

So `send_binary`'s send lambda becomes
`ws->sendBinary(ix::IXWebSocketSendData(frame)).success` — a one-line change
per call site. Note the ctor's documented lifetime requirement: the wrapped
vector must outlive the call, which holds at all three sites.

Related: `BoundedFrameQueue::push` takes the frame **by value** while
`send_binary`'s `queue_pending` lambda passes a `const&`, so every queued frame
is copied per client. Carrying frames as `shared_ptr<const vector<uint8_t>>`
end-to-end would make fan-out to N clients cost N refcount bumps instead of N
full copies.

#### A fresh serializer per frame throws away its own cctx pooling

`app/src/bridge_server.cpp:1073`, `:1099`, `:923`

`AggregatedMessageSerializer` deliberately holds a persistent `ZSTD_CCtx*`
(`message_serializer.cpp:31`), but the publish path constructs one serializer
per group **and one per heavy message**, so every frame pays
`ZSTD_createCCtx` / `ZSTD_freeCCtx`.

**[measured]** (`docs/perf/bench_serializer.cpp`)

```
  frame     today(ms)   pooled(ms)   speedup
  4 KiB        0.0191       0.0039      4.94x
 64 KiB        0.0620       0.0480      1.29x
256 KiB        0.1652       0.1368      1.21x
  1 MiB        0.5765       0.5788      1.00x
```

Direction: hold one serializer (or a small pool) as a member and `clear()` it
between frames. The gain is concentrated in the many-small-frames case, which
is the common one. For the heavy path there is also a redundant memcpy of the
whole payload into `serialized_data_` before compression — a single-message
frame could compress directly from the payload.

#### Ingest zeroes every buffer before overwriting it

`ros2/src/ros2_subscription_manager.cpp:57`, `fastdds/src/fastdds_subscription_manager.cpp:83`

`make_shared<vector<byte>>(n)` value-initializes the buffer, then `memcpy`
overwrites all of it.

**[measured]** (`docs/perf/bench_hotpath.cpp`)

```
 64 KiB : 0.0035 ms  vs  0.0019 ms with reserve+insert  (1.8x)
  2 MiB : 0.2193 ms  vs  0.1495 ms                      (1.5x)
```

#### Two mutexes and a `std::function` copy per received message

`ros2/src/ros2_subscription_manager.cpp:63`, `fastdds/src/fastdds_subscription_manager.cpp:57`,
`app/src/bridge_server.cpp:114`

Each message copies the stored callback under a lock, then takes the **global**
`stats_mutex_` just to increment `topic_receive_counts_[topic]`.

**[measured]** (`docs/perf/bench_hotpath.cpp`)

```
per-msg callback copy + stats mutex : 0.0613 us/msg  vs  0.0018 us/msg direct  (33x)
```

The absolute cost is small when uncontended (**[derived]** ~0.06% of a core at
10k msg/s), so this is not urgent on ROS2 where a single executor thread
serializes everything anyway. It matters on **FastDDS**, where each reader has
its own listener thread and `stats_mutex_` — also taken by the publish cycle —
serializes all ingest.

Direction: resolve a per-topic `atomic<uint64_t>*` at subscribe time, and
publish the callback via a `shared_ptr` loaded atomically rather than copied.

#### `cleanup_old_messages()` scans every topic on every message

`app/src/message_buffer.cpp:37`

Two full passes over the topic map per `add_message`, even when nothing is
stale.

**[measured]** (`docs/perf/bench_hotpath.cpp`)

```
  200 topics : 0.2% of a core at 5k msg/s
 1000 topics : 3.5% of a core at 5k msg/s
```

Direction: TTL only needs enforcing when a topic is drained or on a timer —
amortize to once per publish cycle, or clean only the touched topic's deque.

### 1.3 Tier 3 — discovery and polling

- **`handle_get_topics` issues one RMW graph query per topic.**
  `bridge_server.cpp:282` → `attach_latched_badge` → `is_transient_local` →
  `get_publishers_info_by_topic`. With 500 topics that is 500
  discovery-database queries per `get_topics` request. Batch into the single
  `discover_topics()` pass and cache per poll.
- **`std::regex_match` per topic per poll.** `whitelist_filter.cpp:39`.
  `std::regex` costs on the order of a microsecond per match. Short-circuit the
  default `".*"` (already match-everything) and memoize per topic name.
- **`MessageStripper` fully deserializes a whole Image/PointCloud2 to discard
  `data`.** `message_stripper.cpp:40-49`. Opt-in only, but when enabled it is
  O(image) to produce an O(header) result. See §2.7.

### 1.4 Checked and deliberately rejected

Recorded so nobody re-investigates these.

- **Lower zstd compression levels.** Measured `-1`, `-3`, `-5` against level 1
  (`docs/perf/bench_zstd_levels.cpp`): at best 6% faster, and `-3` drops the
  ratio on float-heavy CDR from 3.98 to 2.65. **Level 1 is the right choice —
  keep it.**
- **Skipping compression for already-compressed payloads.** zstd level 1 bails
  early on incompressible data (**[measured]** ~6 GB/s, 0.17 ms/MiB), so
  compressing a JPEG is nearly free. It is *not* the heavy-frame bottleneck —
  the copies in §1.2 are. The `flags` header field could not signal it anyway
  (see `protocol_constants.hpp:24`).
- **The `compressBound` output-buffer zeroing** in `finalize()`: suspected, but
  harmless at large sizes because glibc serves those allocations from fresh
  zero pages. Only the cctx churn matters, and only for small frames.

---

## 2. ROS2 backend — deep dive

> **Everything in this section is [inferred].** It was not measured: the
> analysis environment had no ROS2 installation. §2.8 is the recipe to confirm
> or falsify it. The single most valuable thing to check first is whether
> `perf` actually shows executor machinery dominating, or whether it is all
> `memcpy` and zstd — that determines which half of this section matters.

### 2.1 The headline: the whole bridge runs on one thread

`ros2/src/main.cpp:162-189` creates four wall timers and a
`SingleThreadedExecutor`, with **no callback groups anywhere**. Every generic
subscription and every timer therefore lands in the node's default
mutually-exclusive callback group, so one thread serially executes:

- every subscription callback (strip → `memcpy` → `MessageBuffer::add_message`)
- `publish_aggregated_messages()` — grouping, zstd, per-client `std::string`
  copies, socket writes
- `process_requests()` at 100 Hz, `check_session_timeouts()` at 1 Hz,
  `check_topic_changes()` at 1 Hz (an RMW graph query)

The consequence: **zstd compression and WebSocket sends block DDS ingest.** A
1 MiB heavy frame costs ~0.6 ms of zstd plus ~0.15 ms per client of
`std::string` copy (both **[measured]**, §1.2). During that ~1 ms no
subscription callback runs, the rmw history fills, and on a BEST_EFFORT sensor
topic those samples are **dropped inside DDS** — invisible to the bridge's own
stats, which only count what it received.

A slow WebSocket client can therefore cause silent sensor data loss. That is a
strictly worse failure mode than the app-level frame dropping the backpressure
policy was built to provide.

### 2.2 Where the single-thread ceiling bites

Two regimes that fail for different reasons. **Test both** — a fix aimed at one
does little for the other.

**Large messages are not the problem people expect.** **[derived]** from §1.2's
measurements (~14 GB/s for a full pass; zstd-1 at ~0.17 ms/MiB on incompressible
image data): one 2 MiB image costs roughly 0.9 ms in copies + 0.34 ms in zstd.
Four 30 Hz cameras ≈ 15% of a core. Annoying, not fatal.

**Many small messages are the problem.** Per message the executor must
dispatch, rmw allocates a `SerializedMessage`, then the bridge pays copy +
`shared_ptr` alloc + `std::function` copy + two mutexes + a topic-string hash +
the `cleanup_old_messages()` scan.

In rclcpp Humble, each `wait_for_work()` cycle **clears and rebuilds the entire
wait set** — walking every callback group and every subscription, timer,
service and waitable, resizing `rcl_wait_set_t`, re-attaching conditions —
before `rcl_wait()`. That is O(entity count) *per wait cycle*, not per message.

With 300 subscribed topics a wait cycle visits 300+ entities. At a nominal
~20 ns per entity that is ~6 µs of bookkeeping per cycle. How bad it gets
depends on how many messages are ready per wait: under a heavy burst many
subscriptions are ready at once and the cost amortizes well; at moderate rates
with messages arriving spread out you approach one wait cycle per message and
~6 µs of overhead on a callback whose useful work is a 200-byte `memcpy`.

That is the regime where a few thousand msg/s across a few hundred topics can
saturate a core. Note the 100 Hz request timer forces at least 100 wait cycles
per second **even when completely idle**, each rebuilding the full wait set.

### 2.3 Get client-side work off the executor thread

The highest-value ROS2 change, and mostly plumbing. Two options:

- **Minimal:** keep `SingleThreadedExecutor` for subscriptions only, and run
  publish / requests / timeouts on their own `std::thread` — essentially
  `run_standalone_event_loop()`, which already exists and becomes
  deadline-driven with the §1.1 fix. The bridge core is already thread-safe at
  these seams (`MessageBuffer` and `SessionManager` are mutex-protected;
  `publish_aggregated_messages` is explicitly written to send outside locks),
  so this is close to a wiring change.
- **ROS2-idiomatic:** one `MutuallyExclusiveCallbackGroup` for subscriptions,
  another for the timers, and a `MultiThreadedExecutor(2)`. Less code, but it
  inherits the multi-threaded executor's own per-wait overhead, which in Humble
  is worse than the single-threaded one.

Recommendation: the first. It also provides the home for the transform workers
described in [MESSAGE_TRANSFORMS.md](./MESSAGE_TRANSFORMS.md), which must never
run on the executor thread for exactly this reason.

### 2.4 Delete the 100 Hz request timer

`ros2/src/main.cpp:162`. It exists only to poll a queue that is almost always
empty, and each poll drags a full wait-set rebuild with it.

The ROS2-native replacement is an `rclcpp::GuardCondition` wrapped in a
`Waitable` and registered via the node's waitables interface, triggered by
`WebSocketMiddleware` when it enqueues an incoming request. The executor then
wakes on an actual request instead of 100 times a second.

If §2.3's first option is taken, this becomes a condition variable instead and
the problem disappears with it.

### 2.5 Reconsider the executor

Humble's per-wait entity rebuild is the known scalability wall, and is the
reason `ExecutorEntitiesCollector` and the `EventsExecutor` were written. An
events-driven executor turns O(entities) per wait into O(ready events).

Upstream `rclcpp::experimental::executors::EventsExecutor` landed in Iron; for
Humble it is available as a separate backport package. **Verify what is
installable in the target environment before planning around it.** If §2.3 is
done first, the subscription-only executor does far less per cycle anyway and
this drops in priority.

### 2.6 The ROS2 copy chain — six passes, three removable

| # | Where | Note |
|---|---|---|
| 1 | rmw → `rclcpp::SerializedMessage` | unavoidable with generic subscriptions |
| 2 | `make_shared<vector<byte>>(n)` + `memcpy` | `ros2_subscription_manager.cpp:57` — **zero-inits first**, so ~1.5 passes |
| 3 | `serialize_message` → `serialized_data_` | `message_serializer.cpp:55` |
| 4 | zstd read + write | necessary |
| 5 | `std::string frame_data(...)` **per client** | `websocket_middleware.cpp:348` — removable, see §1.2 |
| 6 | IXWebSocket internal send buffer, per client | inside the library |

Copies 2 (the memset), 3 (for single-message heavy frames, which can compress
straight from the payload) and 5 are all removable with the §1.2 fixes.

**[derived]** For a 2 MiB image at 30 Hz with 4 clients: roughly 360 MB/s of
memory traffic today against ~180 MB/s achievable — all of it currently on the
ingest thread.

**Allocation note.** Each `SerializedMessage` is a fresh malloc sized to the
message. Above glibc's mmap threshold those become `mmap`/`munmap` with page
faults and kernel zeroing per message — **though glibc raises its threshold
dynamically** once it observes such blocks being freed, so this may
self-correct after the first few images. Worth checking with a malloc counter
rather than assuming either way. If it is happening, a recycling buffer pool
for the ingest vectors fixes it and removes the zero-init at the same time.

### 2.7 Two ROS2-specific configuration issues

**QoS depth on large topics.** `adapt_qos()`
(`ros2/src/generic_subscription_manager.cpp:27`) sums publisher depths and
clamps to `max_qos_depth` (default 100). A KEEP_LAST(100) reader on a 2 MiB
image topic means rmw may hold ~200 MiB of history for that one reader, and
deeper history means more reader-side bookkeeping per sample. The bridge drains
at `publish_rate` and discards whatever the rate gate rejects, so deep history
buys little here. Consider clamping depth by *message size* — a small depth
(5–10) above the heavy threshold — or lowering the default. Memory-dominant
rather than CPU-dominant, but the two couple through cache pressure on the
ingest thread.

**`strip_large_messages` is worse than it looks.** `strip_and_reserialize`
(`ros2/src/message_stripper.cpp:40`) runs
`rclcpp::Serialization<Image>::deserialize_message`, which allocates and copies
the entire 2 MiB `data` vector through generated code, only to overwrite it
with `{0}` and re-serialize. That is roughly three more full passes over the
image, **on the executor thread**, to produce a few hundred bytes.

This is the strongest argument for the transform pipeline in
[MESSAGE_TRANSFORMS.md](./MESSAGE_TRANSFORMS.md): moving it to a worker pool
fixes the thread-blocking, and giving it the rate hint means you stop doing it
30 times a second to send 2.

### 2.8 How to confirm this ranking

```bash
# 1. Is the executor thread saturated, and where?
top -H -p $(pgrep -f pj_bridge_ros2)          # per-thread CPU: one hot thread = confirmed
perf record -F 999 -g -p $(pgrep -f pj_bridge_ros2) -- sleep 30
perf report --sort=dso,symbol
# If the wait-set theory holds, expect rcl_wait / rmw_wait / collect_entities /
# MemoryStrategy high in the profile, ABOVE your own memcpy and ZSTD_compressCCtx.
# If instead it is all memcpy + zstd, then Sections 2.3 and 2.6 are the whole
# story and 2.4 / 2.5 are not worth the disruption.

# 2. Is DDS dropping before the bridge ever sees it?
ros2 topic hz /your/high_rate_topic      # publisher-side rate
# Compare against "DDS receive rates" in the 5s stats log. A gap is rmw-level loss.

# 3. Idle floor, with no clients connected at all:
pidstat -p $(pgrep -f pj_bridge_ros2) 1 10
# Anything meaningfully above zero is timer / wait-set overhead, not real work.

# 4. Allocation behavior on image topics (tests the 2.6 mmap question):
ltrace -c -e 'mmap+munmap' -p $(pgrep -f pj_bridge_ros2)   # or a malloc-count tool
```

Run each at both ends of the §2.2 regime — for example 300 topics at 100 Hz of
small messages, and 2 cameras at 30 Hz.

---

## 3. Suggested order

Effort estimates are rough. Payoff is for the configurations noted in §1–2.

| # | Change | Section | Effort | Payoff | Backend |
|---|---|---|---|---|---|
| 1 | `IXWebSocketSendData` at the 3 send sites | 1.2 | ~10 min | up to 3% of a core | all |
| 2 | `reserve`+`insert` for ingest buffers | 1.2 | ~15 min | 1.5–1.8× on the copy | all |
| 3 | Deadline-driven standalone loop | 1.1 | ~1 h | 9× lower idle floor | RTI, FastDDS |
| 4 | Reuse one serializer per publish cycle | 1.2 | ~1 h | ~5× on small frames | all |
| 5 | Publish work off the executor thread | 2.3 | ~half day | removes ingest stalls | **ROS2** |
| 6 | Cached subscription signature for grouping | 1.1 | ~3 h | 0.7–7% of a core | all |
| 7 | Amortize `cleanup_old_messages()` | 1.2 | ~1 h | up to 3.5% of a core | all |
| 8 | Graph event instead of 1 Hz topic poll | 1.3 | ~3 h | removes steady-state polling | ROS2 |
| 9 | FastDDS raw-payload reader type | 1.1 | ~1 day | largest single win | FastDDS |

Items 1, 2 and 4 are near-free and independent — reasonable to land together.
Item 5 is the one to do first on ROS2 **if** §2.8's profile confirms §2.1.
Item 9 needs a real Fast DDS environment to verify.

## 4. Related

- [MESSAGE_TRANSFORMS.md](./MESSAGE_TRANSFORMS.md) — proposed opt-in message
  transform architecture (image/pointcloud compression). Its worker-pool design
  is a direct consequence of §2.1 and §2.3.
- [`docs/perf/`](./perf) — benchmark sources for every **[measured]** number
  above.
