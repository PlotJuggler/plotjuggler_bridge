# CPU Optimization — Findings and Next Steps

Status: **measured, first fix pass done.** The ROS2 profiling pass this
document called for was done on 2026-09-20 (results in §2.0) and the fixes it
justified are on branch `perf/cpu-fixes` (outcome in §2.9). §1's micro-benchmarks were also re-run on a second machine (see
[docs/perf/README.md](./perf/README.md)) and the ratios hold.

Baseline commit: `f344cca` (0.9.0).

## How to read this document

Every claim is tagged with how it was established:

- **[measured]** — benchmarked on this tree's exact code shapes. The
  benchmark sources are in [`docs/perf/`](./perf) so the numbers can be
  reproduced and extended.
- **[derived]** — arithmetic on top of a measured number (e.g. scaling a
  per-frame cost to a frame rate).
- **[inferred]** — reasoned from the code plus known rclcpp/rmw behavior, and
  **not verified**. §2.0 now holds ROS2-specific measurements (`perf`/
  `pidstat` on a real bag); §2.5–2.7 remain in this category. Treat these as
  hypotheses to confirm with `perf`, not as conclusions.

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

On a second machine (i7-13700H, see `docs/perf/README.md`): 3.9x at 4 KiB,
~1.0–1.1x at ≥64 KiB — so the "~5×" figure applies to small frames only.

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
  ratio on float-heavy CDR from 3.98 to 2.65. **Level 1 remains right for
  light (small-message) frames — keep it there.**
- **Skipping compression for already-compressed payloads.** zstd level 1 bails
  early on incompressible data (**[measured]** ~6 GB/s, 0.17 ms/MiB), so
  compressing a JPEG is nearly free. That benchmark used
  `bench_zstd_levels.cpp`'s synthetic "image-like" payload, which is
  incompressible — **not representative of real sensor data, and the
  rejection is REVERSED for heavy frames by §2.0's real-payload sweep**: real
  point clouds/images are only partially compressible, so zstd does full work
  at ~350–420 MB/s and *is* the heavy-frame bottleneck, not the copies in
  §1.2 as originally claimed here. A negative level on heavy size-class
  frames (e.g. `-5`: 1.9–3× less compression CPU for a 7–20% larger wire
  size) is therefore a legitimate CPU/bandwidth knob for heavy frames. It is
  wire-compatible: any zstd decoder accepts a negative-level frame, and the
  `flags` header field stays 0 either way (see `protocol_constants.hpp:24`).
- **The `compressBound` output-buffer zeroing** in `finalize()`: suspected, but
  harmless at large sizes because glibc serves those allocations from fresh
  zero pages. Only the cctx churn matters, and only for small frames.

---

## 2. ROS2 backend — deep dive

### 2.0 Measured results (2026-09-20)

**[measured]** Setup: i7-13700H, RoboStack Humble (pixi), rmw_fastrtps (Fast
DDS 2.6.10), Release build of `f344cca`. Load: `ros2 bag play --loop` of a
5-minute slice of a real warehouse-robot bag — 83 playable topics, ~1000
msg/s, almost all small messages (Float32/Bool/PoseStamped/JointState at
10–100 Hz, a few PointCloud2/OccupancyGrid at 1–5 Hz). Clients: a minimal
Python `websockets` client subscribing to all 83 topics, heartbeating at
1 Hz. CPU via `pidstat` (20 s averages), percent of one core.

| Scenario | total %CPU | main (executor) thread | all `Srv:ws:*` threads |
|---|---|---|---|
| bag 1×, 0 clients (no subscriptions) | 0.85 | 0.8 | – |
| bag 1×, 1 client | 19.2 | ~19.9 | 0.0 |
| bag 1×, 2 clients | 28.1 | ~32 | 0.0 |
| bag 1×, 4 clients | 31.5 | ~25 | 0.0 |
| bag 4×, 1 client | 45.8 | ~41.8 | 0.0 |
| bag 4×, 4 clients | 52.9 | ~47.8 | 0.0 |
| bag 10×, 1 client | 53.3 | ~44.7 | 0.0 |
| no bag, no clients (idle floor) | 1.85 | 2.0 | – |

(Per-thread and total columns are separate sampling windows over a bursty
source, so they don't add exactly.) Client-side frame rate stayed at exactly
50/s per client in every scenario; no drop/backlog warnings were logged. Wire
throughput was only 0.07 MB/s per client at 1× — the cost is per-message/
per-cycle overhead, not bytes.

`perf record -F 999 --call-graph dwarf`, 15 s, bag 1×, 1 client — inclusive
share of process samples:

- `Executor::wait_for_work` 56–58%, of which `rcl_wait`/`__rmw_wait` 45–48%.
  Top self-time symbols: `pthread_mutex_lock` 12.5% + `pthread_mutex_unlock`
  8.2% (almost entirely under `__rmw_wait`), `WaitSetImpl::attach_condition`
  9.3%, `WaitSetImpl::detach_condition` 4.7%, `CallbackGroup::collect_all_ptrs`
  2.9%, `remove_null_handles` 2.1%. I.e. rmw_fastrtps attaches and detaches
  every subscription's condition to the Fast DDS WaitSet, each under a mutex,
  on every wait cycle.
- `publish_aggregated_messages` 15–17%, of which
  **`ix::WebSocketPerMessageDeflateCompressor::compress` (zlib) 7–11%** vs
  `ZSTD_compressCCtx` 0.8–2%.
- `execute_subscription` (the actual ingest callbacks: copy +
  `MessageBuffer::add_message`) 2.6–3.8%.
- By DSO: libfastrtps 28%, libc 27% (mostly mutex), librclcpp 12%, libz 11%,
  rmw_fastrtps 5%, librcl 3%, **pj_bridge_ros2 itself 1.5%**, libzstd 0.9%.

Conclusions:

1. §2.1 confirmed [measured]: one thread does everything; WebSocket threads
   ~0%.
2. §2.2's wait-set theory confirmed [measured] and is THE dominant cost in the
   many-small-messages regime: over half of all CPU is wait-set rebuild, not
   memcpy/zstd. The bridge's own code is ~1.5%. Consequently most of §1.2's
   per-message items (stats mutex, cleanup scan, zero-init) are noise for
   ROS2 at this topic count; §2.3–2.5 are where the CPU is.
3. §2.4 confirmed [measured]: idle floor with zero subscriptions is
   0.85–1.85% of a core, purely timers + wait-set.
4. NEW finding, not in the original analysis: IXWebSocket negotiates
   permessage-deflate when the client offers it, so every already-zstd-
   compressed binary frame is deflated again with zlib on the executor
   thread, costing ~5× more CPU than the zstd pass itself. Re-running the
   1-client scenario with the client's deflate offer disabled dropped total
   CPU from 19.2% to 12.9% [measured]. Caveat: this only triggers for clients
   that offer the extension (Python `websockets` does by default); whether
   the PlotJuggler plugin's WebSocket client offers it has NOT been checked —
   verify before prioritizing. Direction: disable per-message deflate
   server-side (`ix::WebSocketPerMessageDeflateOptions(false)` on the server)
   since payloads are already zstd; text frames are tiny.
5. Not measured here: DDS-level sample loss (the ROS2 entry point has no
   periodic stats log — that block only exists in
   `standalone_event_loop.cpp`; see §2.8 step 2), the §2.6 mmap question, and
   everything FastDDS-backend-specific. The large-message regime *is* now
   measured — see the subsection below.

#### Large-message regime

**[measured]** Same machine/setup as above. Client had WebSocket
permessage-deflate disabled to isolate the bridge's own path (see conclusion
4 above — deflate would otherwise dominate these numbers too).

| Workload | in | clients | bridge %CPU (one core) | wire out per client |
|---|---|---|---|---|
| 4× PointCloud2 @10 Hz (~1.3 MB each) + Imu 400 Hz + tf 480 Hz + odom 71 Hz | ~52 MiB/s | 0 | 0.9 | – |
| same | same | 1 | 33.9 | 31.0 MB/s |
| same | same | 4 (one subscription group) | 34.0 | 31.0 MB/s each (124 MB/s total) |
| 2× raw mono Image @20 Hz (~360 KB each) + Imu 200 Hz | ~14 MiB/s | 1 | 11.7 | 12.4 MB/s |

`perf record -F 999 --call-graph dwarf`, point-cloud workload, 1 client —
inclusive share: `publish_aggregated_messages` 67–77%, of which
`AggregatedMessageSerializer::finalize` → `ZSTD_compressCCtx` 60–65%;
`execute_subscription` (ingest incl. rmw take) 6.3%; `wait_for_work` 7.5%;
`WebSocketMiddleware::send_binary` 2.8%. Self time: `ZSTD_compressBlock_fast`
43%, `__memmove_avx_unaligned_erms` 9.5%, `ZSTD_encodeSequences` 5.4%,
`__memset_avx2_unaligned_erms` 3.5%. By DSO: libzstd 60%, libc 19%,
libfastrtps 6.6%, librclcpp 2.6%. Raw-image workload: libzstd 48%, memmove 9%,
memset 3%.

Conclusions:

- zstd is THE cost on heavy frames (60–65%), copies are ~13% (memmove +
  memset). This contradicts §1.4's "compression is not the heavy-frame
  bottleneck — the copies are": that claim came from `bench_zstd_levels.cpp`'s
  synthetic "image-like" payload, which is incompressible so zstd bails out at
  ~6–9 GB/s. Real sensor data is partially compressible (point clouds 1.9×,
  raw mono images 1.13× at level 1) so zstd does full work at ~350–420 MB/s.
- Fan-out is nearly free: 1 → 4 clients in the same subscription group added
  ~0% CPU (compression is done once per group; the per-client `std::string`
  copy + socket write of 31 MB/s each is lost in the noise, %system rose
  2.3 → 4.9). So §1.2's per-client frame copy is real but low priority.
- All of that zstd time runs on the single executor thread (§2.1), i.e. it is
  time during which no subscription callback runs.

Real-payload zstd level sweep (8 concatenated real messages, single thread,
i7-13700H) **[measured]**:

| zstd level | point cloud (rslidar, 11.0 MB) | raw mono image (2.9 MB) |
|---|---|---|
| 3 | 2.28× @ 186 MB/s | 1.20× @ 144 MB/s |
| 1 | 1.89× @ 350 MB/s | 1.13× @ 422 MB/s |
| -1 | 1.79× @ 411 MB/s | 1.07× @ 673 MB/s |
| -5 | 1.51× @ 652 MB/s | 1.05× @ 1274 MB/s |
| -20 | 1.27× @ 1417 MB/s | 1.02× @ 2490 MB/s |
| -100 | 1.15× @ 2997 MB/s | 1.01× @ 3633 MB/s |
| -1000 | 1.05× @ 5975 MB/s | 1.00× @ 8540 MB/s |

> Originally everything in this section was **[inferred]**: the analysis
> environment had no ROS2 installation. §2.0 above now confirms §2.1, §2.2
> and §2.4 by measurement. §2.5–2.7 remain **[inferred]**.

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

**Correction (§2.0, [measured]):** wrong on both counts. Real point-cloud
traffic (~52 MiB/s) cost 34% of a core, dominated by zstd (60–65%), not
copies (~13%) — because real data compresses and the estimate above assumed
incompressible payload.

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

§2.0 measured the wait-set cost at 45–58% of process CPU with 83
subscriptions on rmw_fastrtps; trying rmw_cyclonedds is a cheap experiment
worth doing before an executor swap.

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
# Compare against the bridge's own received count. Note: the ROS2 entry point
# has no periodic stats log (that 5s log block only exists in
# standalone_event_loop.cpp, i.e. the RTI/FastDDS backends) — for ROS2, add a
# temporary counter or read topic_receive_counts_ via a debug hook instead.

# 3. Idle floor, with no clients connected at all:
pidstat -p $(pgrep -f pj_bridge_ros2) 1 10
# Anything meaningfully above zero is timer / wait-set overhead, not real work.

# 4. Allocation behavior on image topics (tests the 2.6 mmap question):
ltrace -c -e 'mmap+munmap' -p $(pgrep -f pj_bridge_ros2)   # or a malloc-count tool
```

Run each at both ends of the §2.2 regime — for example 300 topics at 100 Hz of
small messages, and 2 cameras at 30 Hz.

---

## 2.9 Outcome of the first fix pass (branch `perf/cpu-fixes`)

**Measurement rig note (important).** The §2.0 numbers were taken unpinned
under the `powersave` governor on a hybrid P/E-core CPU and proved noisy (same
config varied 30–48%). All before/after numbers below are **[measured]** with
governor `performance`, bridge pinned to P-cores (`taskset -c 4-7`), bag
player and client on other cores, CPU from `/proc/<pid>/stat` over 20 s,
client decoding frames and counting messages per topic to prove losslessness.
Absolute values are therefore ~half of §2.0's — compare only within this
table.

| Workload (1 client) | main (`f344cca`) | `perf/cpu-fixes` | notes |
|---|---|---|---|
| 83 topics, ~700 msg/s small messages | 7.2–7.6% | 3.4% | identical message totals (20.5k / 30 s) |
| 4 lidars 52 MiB/s + 400 Hz imu + 480 Hz tf | 17.9–18.6% | 18.8–19.1% (level 1) / 15.0% (level -5) | 31 vs 39 MB/s on the wire |
| bag playing, no clients | 0.45% | 0.45% | |

Plus the deflate fix from §2.0 (19.2% → 12.9%, unpinned rig) for clients that
offer permessage-deflate.

**What was done (3 commits):**

1. Server declines permessage-deflate (`server_->disablePerMessageDeflate()`),
   with a unit test on the handshake.
2. ROS2 ingest: subscription callbacks drain their reader via
   `take_serialized()`; ingest executor polls every `ingest_poll_interval_ms`
   (default 5; 0 = blocking spin; blocks instead of polling while there are no
   subscriptions so idle is unchanged); `min_qos_depth` default 1 → 10; timers
   (publish/zstd, requests, timeouts, topic poll) moved to their own callback
   group + executor thread. Poll-interval sweep on the small-message workload
   **[measured]**: blocking 7.5%, 2 ms 4.5%, 5 ms 3.3%, 10 ms 2.6%.
3. `heavy_frame_zstd_level` knob (default 1, unchanged behaviour). End-to-end
   sweep on the lidar workload **[measured]**: level 1 = 19.0% / 31.0 MB/s; -5
   = 14.8% / 39.0; -20 = 14.4% / 45.6; -100 = 10.3% / 50.6. The end-to-end gain
   is smaller than the isolated zstd sweep in §2.0 predicts because the larger
   frames cost more in socket writes. Non-zeroing copies in ingest +
   serializer.

**Lessons worth recording:**

- Naive batching (spin_some + sleep WITHOUT draining) silently loses data: the
  executor takes one message per subscription per wait cycle, so a 5 ms poll
  capped a 481 Hz `/tf` topic at 154 Hz. Draining in the callback is what
  makes polling lossless. Draining alone (with blocking spin) gives no CPU
  gain (7.3% vs 7.5%).
- A variant that blocked in `spin_once` before every poll was worse than main
  under load (6.2% small-message, 24% lidar) — rejected.
- ThreadSanitizer live run (lidar load, clients joining/leaving for 60 s): no
  races in pj_bridge code between the ingest and timer threads; only the known
  IXWebSocket teardown warning and rclcpp's signal-handler errno warning.

**Deliberately NOT done, with the measurement that justifies skipping:**
serializer/cctx pooling (3 µs vs 12 µs per small frame ⇒ ~0.05% of a core at
50 Hz); `IXWebSocketSendData` zero-copy send and shared_ptr frame fan-out
(1 → 4 clients added ~0% CPU at 31 MB/s each); cached subscription-group
signature and `cleanup_old_messages` amortization (bridge's own code was 1.5%
of the small-message profile). Still open: compression off the publish thread
/ worker pool for heavy frames (zstd remains ~60% of heavy-regime CPU),
rmw_cyclonedds comparison, FastDDS-backend items (unmeasured), whether the
PlotJuggler plugin offers permessage-deflate.

---

## 3. Suggested order

Effort estimates are rough. Payoff is for the configurations noted in §1–2.

| # | Change | Section | Effort | Payoff | Backend | Status |
|---|---|---|---|---|---|---|
| 1 | Disable WebSocket permessage-deflate server-side | 2.0 | ~10 min | ~6% of a core per deflate-offering client (measured) | all | done (§2.9) |
| 2 | Lower zstd level for heavy size-class frames (configurable) | 1.4 / 2.0 | ~1 h | ~2–3× less CPU on heavy frames (the dominant cost there) | all | done (§2.9) |
| 3 | `IXWebSocketSendData` at the 3 send sites | 1.2 | ~10 min | up to 3% of a core — §2.0 measured all copies (this + reserve/insert) together at ~13% of heavy-regime CPU | all | skipped — measured negligible (§2.9) |
| 4 | `reserve`+`insert` for ingest buffers | 1.2 | ~15 min | 1.5–1.8× on the copy — §2.0 measured all copies together at ~13% of heavy-regime CPU | all | done (§2.9) |
| 5 | Deadline-driven standalone loop | 1.1 | ~1 h | 9× lower idle floor | RTI, FastDDS | open |
| 6 | Reuse one serializer per publish cycle | 1.2 | ~1 h | ~4–5× on small frames only | all | skipped — measured negligible (§2.9) |
| 7 | Publish work off the executor thread | 2.3 | ~half day | removes ingest stalls | **ROS2** | done (§2.9) |
| 8 | Cached subscription signature for grouping | 1.1 | ~3 h | 0.7–7% of a core | all | skipped — measured negligible (§2.9) |
| 9 | Amortize `cleanup_old_messages()` | 1.2 | ~1 h | up to 3.5% of a core | all | skipped — measured negligible (§2.9) |
| 10 | Graph event instead of 1 Hz topic poll | 1.3 | ~3 h | removes steady-state polling | ROS2 | open |
| 11 | FastDDS raw-payload reader type | 1.1 | ~1 day | largest single win | FastDDS | open |

Items 3, 4 and 6 are near-free and independent — reasonable to land together.
Item 7 (publish work off the executor thread) is confirmed valuable by §2.0 —
the executor thread is the whole bottleneck. But note that moving publish
off-thread removes only the ~15–17% publish share measured in §2.0; the
>50% wait-set share needs §2.4/§2.5 (fewer wakeups, events executor) or a
different rmw, so raise §2.5's priority accordingly.
Item 11 needs a real Fast DDS environment to verify.

## 4. Related

- [MESSAGE_TRANSFORMS.md](./MESSAGE_TRANSFORMS.md) — proposed opt-in message
  transform architecture (image/pointcloud compression). Its worker-pool design
  is a direct consequence of §2.1 and §2.3.
- [`docs/perf/`](./perf) — benchmark sources for every **[measured]** number
  above.
