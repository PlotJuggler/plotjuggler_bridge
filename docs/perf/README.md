# Hot-path micro-benchmarks

Standalone programs backing every **[measured]** number in
[../CPU_OPTIMIZATION.md](../CPU_OPTIMIZATION.md).

They are **not** part of the build and are not run by `colcon test`. They
deliberately *replicate* the code shapes found in `app/`, `ros2/` and
`fastdds/` rather than linking the real targets, so they compile in seconds
with nothing but a C++20 compiler (plus `libzstd-dev` for two of them) and no
ROS2 / DDS installation. The trade-off is that they drift if the real code
changes — check them against the cited `file:line` before trusting a number.

## Build and run

```bash
cd docs/perf
g++ -O2 -std=c++20 -o bench_hotpath     bench_hotpath.cpp     && ./bench_hotpath
g++ -O2 -std=c++20 -o bench_loop        bench_loop.cpp -pthread && ./bench_loop
g++ -O2 -std=c++20 -o bench_serializer  bench_serializer.cpp  -lzstd && ./bench_serializer
g++ -O2 -std=c++20 -o bench_zstd_levels bench_zstd_levels.cpp -lzstd && ./bench_zstd_levels
```

## What each one measures

| File | Replicates | Answers |
|---|---|---|
| `bench_hotpath.cpp` | `bridge_server.cpp:1027` grouping, `message_buffer.cpp:37` cleanup scan, `ros2_subscription_manager.cpp:57` ingest copy, `bridge_server.cpp:114` callback+stats | Cost of per-cycle subscription grouping vs. topic/client count; per-message TTL scan; zero-init vs. `reserve`+`insert`; `std::function` copy + global stats mutex |
| `bench_loop.cpp` | `standalone_event_loop.cpp:113` idle loop, `websocket_middleware.cpp:348` send copy | Idle CPU floor of the 1 ms sleep loop vs. deadline-driven; per-client `std::string` frame copy |
| `bench_serializer.cpp` | `message_serializer.cpp:69` `finalize()` | Cost of constructing a serializer (and thus a `ZSTD_CCtx`) per frame vs. pooling one |
| `bench_zstd_levels.cpp` | the zstd call inside `finalize()` | Whether negative zstd levels are worth it (**they are not** — see CPU_OPTIMIZATION.md §1.4) |

## Reference results

Recorded so a future run can be compared rather than interpreted from scratch.
Absolute values are hardware-specific; the **ratios** are the durable part.

Environment: 4-core Intel Xeon @ 2.10 GHz, g++ 13.3.0 `-O2`, libzstd 1.5.5,
container with no ROS2 installed.

```
=== bench_hotpath ===
  grouping   100 topics x 1 clients :   0.015 ms/cycle  ->   0.1% of one core @50Hz
  grouping   500 topics x 1 clients :   0.144 ms/cycle  ->   0.7% of one core @50Hz
  grouping   500 topics x 4 clients :   0.553 ms/cycle  ->   2.8% of one core @50Hz
  grouping  2000 topics x 2 clients :   1.375 ms/cycle  ->   6.9% of one core @50Hz

  cleanup scan    50 topics :   0.0001 ms/message -> at 5k msg/s =   0.0% of one core
  cleanup scan   200 topics :   0.0003 ms/message -> at 5k msg/s =   0.2% of one core
  cleanup scan  1000 topics :   0.0071 ms/message -> at 5k msg/s =   3.5% of one core

  ingest copy     1024 B : zero+memcpy  0.0000 ms  reserve+insert  0.0000 ms  (1.14x)
  ingest copy    65536 B : zero+memcpy  0.0035 ms  reserve+insert  0.0019 ms  (1.79x)
  ingest copy  2097152 B : zero+memcpy  0.2193 ms  reserve+insert  0.1495 ms  (1.47x)

  per-msg cb copy + stats mutex :   0.0613 us/msg  vs direct   0.0018 us/msg  (33.6x)

=== bench_loop ===
  today  (1 ms sleep) :  1.84% of one core idle  (911 wakeups/s)
  deadline-driven     :  0.20% of one core idle  ( 50 wakeups/s)

  frame     8192 B x 1 clients :  0.0001 ms/frame ->  0.0% of a core @50Hz
  frame   262144 B x 4 clients :  0.0271 ms/frame ->  0.1% of a core @50Hz
  frame  2097152 B x 4 clients :  0.5921 ms/frame ->  3.0% of a core @50Hz

=== bench_serializer ===
     frame    today(ms)   pooled(ms)   memcpy(ms)    speedup
     4096B       0.0191       0.0039       0.0000      4.94x
    65536B       0.0620       0.0480       0.0018      1.29x
   262144B       0.1652       0.1368       0.0062      1.21x
  1048576B       0.5765       0.5788       0.0355      1.00x
  4194304B       2.0607       1.9939       0.3071      1.03x

=== bench_zstd_levels ===
payload     level       ms/MiB        ratio       MB/s
float-CDR       1       2.7676         3.98        379
float-CDR      -1       2.5910         3.98        405
float-CDR      -3       2.6584         2.65        394
float-CDR      -5       2.8154         2.26        372
image-like      1       0.1700         1.00       6168
image-like     -1       0.1629         1.00       6437
image-like     -3       0.1472         1.00       7125
image-like     -5       0.1500         1.00       6991
```

Environment 2: Intel Core i7-13700H (governor powersave, pinned with
`taskset -c 2`), g++ 15.2.0 `-O2`, libzstd 1.5.7.

```
=== bench_hotpath ===
  grouping   100 topics x 1 clients :   0.040 ms/cycle  ->   0.2% of one core @50Hz
  grouping   500 topics x 1 clients :   0.158 ms/cycle  ->   0.8% of one core @50Hz
  grouping   500 topics x 4 clients :   0.528 ms/cycle  ->   2.6% of one core @50Hz
  grouping  2000 topics x 2 clients :   1.112 ms/cycle  ->   5.6% of one core @50Hz

  cleanup scan    50 topics :   0.0000 ms/message -> at 5k msg/s =   0.0% of one core
  cleanup scan   200 topics :   0.0003 ms/message -> at 5k msg/s =   0.1% of one core
  cleanup scan  1000 topics :   0.0052 ms/message -> at 5k msg/s =   2.6% of one core

  ingest copy     1024 B : zero+memcpy  0.0000 ms  reserve+insert  0.0000 ms  (1.14x)
  ingest copy    65536 B : zero+memcpy  0.0024 ms  reserve+insert  0.0013 ms  (1.83x)
  ingest copy  2097152 B : zero+memcpy  0.1555 ms  reserve+insert  0.1006 ms  (1.55x)

  per-msg cb copy + stats mutex :   0.0438 us/msg  vs direct   0.0016 us/msg  (27.7x)

=== bench_loop ===
  today  (1 ms sleep) :  0.96% of one core idle  (905 wakeups/s)
  deadline-driven     :  0.18% of one core idle  ( 50 wakeups/s)

  frame     8192 B x 1 clients :  0.0004 ms/frame ->  0.0% of a core @50Hz
  frame   262144 B x 4 clients :  0.0196 ms/frame ->  0.1% of a core @50Hz
  frame  2097152 B x 4 clients :  0.3590 ms/frame ->  1.8% of a core @50Hz

=== bench_serializer ===
     4096B       0.0125       0.0032       0.0001      3.92x
    65536B       0.0387       0.0372       0.0014      1.04x
   262144B       0.1244       0.1146       0.0054      1.09x
  1048576B       0.4678       0.4611       0.0413      1.01x
  4194304B       1.6706       1.6560       0.1744      1.01x

=== bench_zstd_levels ===
float-CDR       1       2.2053         3.98        475
float-CDR      -1       2.1174         3.98        495
float-CDR      -3       2.0758         2.65        505
float-CDR      -5       2.2985         2.26        456
image-like      1       0.1395         1.00       7516
image-like     -1       0.1257         1.00       8342
image-like     -3       0.1172         1.00       8943
image-like     -5       0.1083         1.00       9682
```

Ratios reproduce; the one exception is the pooled-serializer gain at
64–256 KiB, which shrinks from 1.2–1.3x to 1.04–1.09x (the win is confined to
small frames).

## What these do NOT cover

Anything requiring a live middleware — which is most of the ROS2 analysis. In
particular, none of these measure the rclcpp executor, rmw discovery, DDS
history behavior, or the FastDDS `DynamicData` round-trip. Use
[../CPU_OPTIMIZATION.md §2.8](../CPU_OPTIMIZATION.md#28-how-to-confirm-this-ranking)
for those.
