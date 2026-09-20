# Message Transforms (v1: Cloudini) — Design

Date: 2026-09-20. Status: implemented (ROS2 backend); see "Measured results".
Supersedes the exploratory `docs/MESSAGE_TRANSFORMS.md` (PR #14).

## Goal

Let the server operator replace the payload of selected topics with a
transformed one before it is buffered and sent. First transform:
[Cloudini](https://github.com/facontidavide/cloudini) point cloud compression
(`sensor_msgs/msg/PointCloud2` → `point_cloud_interfaces/msg/CompressedPointCloud2`).
The existing `MessageStripper` becomes the second implementation.

## Decisions

| Question | Decision |
|---|---|
| Scope | Minimal seam + Cloudini. No per-client variants, no offer/force negotiation, no worker pool. |
| Who enables it | Server operator, off by default. A topic is transformed for all subscribers or none. |
| Configuration | One parameter, `transform_profile`, pointing to a JSON file. |
| Wire protocol | Unchanged. Transformed topics are simply advertised with their output type and schema. |
| Cloudini dependency | `find_package(cloudini_lib)`, FetchContent fallback (pinned tag + SHA256, static). Feature compiled out if neither works. |
| Double compression | Cloudini second stage is `NONE`; the frame-level zstd is the general-purpose stage. |
| `strip_large_messages` | `strip` becomes a transform name; the boolean stays as sugar. |
| Threading | Synchronous in the ingest callback. Measure before adding a worker. |

Client requirement: a PlotJuggler whose `parser_ros` decodes
`CompressedPointCloud2` (`pj-official-plugins`, branch
`feat/compressed-pointcloud`). Older clients see a topic they cannot parse;
that is the operator's responsibility when enabling the profile. The plugin
already binds its parser from the type in `get_topics` and the
`encoding`/`definition` in the subscribe response, so it needs no protocol
change.

## Architecture

`BridgeServer` learns types and schemas only through `TopicSourceInterface`
and message bytes only through `SubscriptionManagerInterface`. It is not
modified (except for the informational fields below).

```
Ros2TopicSource ──► TransformingTopicSource ──► BridgeServer
                          │ records topic → (source_type, rule)
                          ▼
                     TransformSet  ◄── profile JSON + name→factory map
                          ▲ resolve(topic) → MessageTransform*
Ros2SubscriptionManager ──┘  (calls apply() on the rcl buffer, before any copy)
```

- **`TransformSet`** (`app/`): owns the ordered rules, the name→factory map,
  and the per-topic transform instances. `resolve(topic, source_type)` returns
  the matching rule (first match wins) or nothing. Thread-safe: written from
  the request thread (`get_topics`/`subscribe`), read from the ingest thread.
- **`TransformingTopicSource`** (`app/`, decorator): `get_topics()` rewrites
  `type` for matched topics and records the source type in the `TransformSet`;
  `get_schema()` returns the transform's output schema; everything else passes
  through.
- **Backend hook**: `Ros2SubscriptionManager::subscribe(name, advertised_type)`
  asks the `TransformSet` for the topic's real source type and subscribes with
  it. In the message callback, if a transform is resolved for the topic, call
  `apply()` on the view over the rcl buffer and forward `out`; otherwise copy
  as today. FastDDS can adopt the same three lines later; v1 wires ROS2 only.
- With no profile and `strip_large_messages: false`, no `TransformSet` is
  created and behavior is byte-identical to today.

Everything downstream (latched retention, heavy-frame threshold, rate gate,
serializer, backpressure) sees post-transform bytes with no special cases. A
2 MiB cloud that compresses below `heavy_frame_threshold_bytes` rejoins normal
aggregation.

## Interface

`app/include/pj_bridge/message_transform.hpp`, no ROS/DDS dependencies:

```cpp
/// One instance per topic. Called from a single thread (the ingest executor).
/// Implementations SHOULD keep codec contexts and scratch buffers as members.
class MessageTransform {
 public:
  virtual ~MessageTransform() = default;
  /// `in` is a view over the middleware's buffer, valid only during the call.
  /// `out` is the final storage (already inside the shared_ptr handed to
  /// MessageBuffer): cleared and filled in place, never moved or copied.
  /// On error the sample is DROPPED by the caller — never forwarded raw, since
  /// the client was told the output type at subscribe time.
  virtual tl::expected<void, std::string> apply(
      std::span<const std::byte> in, std::vector<std::byte>& out) = 0;
  /// What clients are told they receive. Defaults: unchanged (as for `strip`).
  virtual std::string output_type(const std::string& source_type) const;
  virtual std::string output_schema(const std::string& source_schema) const;
};

struct TransformFactory {
  std::function<bool(const std::string& source_type)> accepts;
  std::function<tl::expected<void, std::string>(const nlohmann::json& params)> check_params;
  std::function<std::unique_ptr<MessageTransform>(const std::string& source_type,
                                                  const nlohmann::json& params)> create;
};
```

Allocation rationale: the input is never copied for a transformed topic (the
raw cloud is read in place from the rcl buffer); the only per-message
allocation is the output vector at compressed size, which is unavoidable
because `MessageBuffer` retains it. The input cannot use move semantics: the
rcl buffer is owned by rclcpp and a `std::vector` cannot adopt it, so a
read-only span is the only zero-copy option. The hook does
`auto out = std::make_shared<std::vector<std::byte>>(); apply(in, *out);` —
the transform writes straight into the final storage. In v1 every message gets
a fresh vector; because `out` is caller-provided, the hook can later hand out
recycled vectors (`shared_ptr` with a recycling deleter —
no downstream change) if measurement justifies it. Not built in v1.

## Transforms

### `cloudini` (`app/`, compiled only when Cloudini is available)

`getDeserializedPointCloudMessage` → `applyResolutionProfile` → optional
`applyVizLossyPreprocessing` → `toEncodingInfo` →
`convertPointCloud2ToCompressedCloud`. Cloudini's CDR writer (`nanocdr`) only
targets `std::vector<uint8_t>`, so the transform encodes into a member scratch
vector (capacity kept across calls) and copies the result — at compressed size
— into `out`. v1 accepts that copy and the per-call `PointcloudEncoder`
construction inside the convenience function; removing both needs a small
Cloudini API addition (byte-generic output, reusable encoder), tracked
separately. Second stage `NONE`.
Output schema comes from Cloudini's embedded `ros_message_definitions.hpp`, so
`point_cloud_interfaces` need not be installed.

Params:

| Key | Default | Meaning |
|---|---|---|
| `resolution` | `0.001` | Resolution for FLOAT32 fields not listed in `fields` (metres for xyz) |
| `fields` | `{}` | Per-field resolution; `0` removes the field |
| `viz_preprocessing` | `false` | NaN drop + voxel dedup + 1 µs FLOAT64 quantization |

### `strip` (`ros2/`, registered by `ros2/main.cpp`)

Thin adapter over the existing `MessageStripper`; output type = input type,
schema unchanged. It keeps its current cost (full typed deserialize /
re-serialize, plus one copy into a `SerializedMessage`). A CDR-level stripper
that avoids this is a separate follow-up.

## Configuration

ROS2 parameter `transform_profile` (string, default empty = disabled).

```json
{
  "transforms": [
    {
      "match_type": "sensor_msgs/msg/PointCloud2",
      "match_topic": "/lidar/.*",
      "transform": "cloudini",
      "params": {"resolution": 0.001, "fields": {"intensity": 0.01}, "viz_preprocessing": true}
    }
  ]
}
```

- `match_type` (exact) is required; `match_topic` (full-match regex, same
  semantics as `topic_whitelist`) optionally narrows it. Requiring the type
  makes every transform/type mismatch a startup error.
- Ordered, first match wins.
- `strip_large_messages: true` appends one `strip` rule per strippable type
  **after** the profile's rules, so an explicit profile rule wins.

**Fail at startup** (clear error, non-zero exit): unreadable or malformed
file, unknown top-level/rule/param key, unknown transform name, `cloudini`
named in a build without Cloudini, invalid regex, params rejected by the
transform's `check_params`, a rule without `match_type`, a `match_type` the named
transform does not `accept()`. A
transform that cannot be instantiated for a topic (factory throws or returns
nothing) is logged as an error and the topic is left untransformed.

**At runtime**, `apply()` failure → drop the sample, bump a counter, throttled
warning. This also applies to `strip` (today it forwards the original; changed
for uniformity).

## Informational protocol additions (ignored by old clients)

- `message_transforms` in the server capabilities list.
- Optional `"source_type"` on `get_topics` entries of transformed topics
  (not on `topics_changed` entries in v1).
- Per-topic transform counters (samples, drops, compression ratio, mean µs per
  sample) logged with the final statistics at shutdown; the ROS2 entry point
  has no periodic stats line today.

## Build

The project moves from C++17 to **C++20** unconditionally: `std::span` needs
it, and `cloudini_lib` exports `cxx_std_20` as a PUBLIC requirement, so the
standard must not depend on whether the optional dependency was found. The
oldest targeted compiler is GCC 11 (Humble / Jammy), which supports it.

```cmake
find_package(cloudini_lib QUIET)
if(NOT cloudini_lib_FOUND)  # FetchContent fallback, pinned tag + URL_HASH
  ...
endif()
# PJ_BRIDGE_HAS_CLOUDINI defined on pj_bridge_app when available
```

Buildfarm debs cannot fetch at build time, so they get the feature once
`cloudini_lib` is a resolvable rosdep key for the distro; GitHub-built debs,
conda packages and AppImages get it through the fallback.

## Testing

- Profile parsing: valid file; each startup failure mode.
- Rule matching and precedence, including the `strip_large_messages` sugar.
- `TransformingTopicSource` with the existing mock topic source: type and
  schema rewrite, untouched topics pass through.
- Backend hook with a fake transform: source-type lookup on subscribe, output
  forwarded, failure drops the sample and counts it.
- `cloudini` smoke test (only when compiled in): a synthetic PointCloud2 CDR
  buffer transforms without error into a non-empty, smaller output. No
  decode/round-trip test — decoding correctness belongs to Cloudini's own
  suite.
- End-to-end, manual: PlotJuggler with the compressed-pointcloud parser against
  the 4-lidar bag.

## Acceptance measurement

Pinned rig from `docs/perf/README.md`, 4-lidar bag (~52 MiB/s), 1 client,
Cloudini off vs on: bridge CPU (% of a core), bytes on the wire, and
client-side per-topic message counts (must be lossless). If encode time
starves ingest, the designed-but-unbuilt fallback is a per-topic worker with a
1-deep latest-wins input slot.

## Measured results (2026-09-20)

Pinned rig (bridge on cores 4-7, player 12-19, client 8-11, governor
`performance`), 90 s cut of the 4-lidar bag (4 x PointCloud2 at 10 Hz, ~52 MiB/s
raw, plus /tf, /imu, /odom), one client subscribed to everything, CPU from
`/proc/<pid>/stat` over 20 s, three runs each.

| Configuration | Bridge CPU (% of a core) | Wire | Encode | Pre-zstd ratio |
|---|---|---|---|---|
| no profile | 16.7 / 18.9 / 19.3 | 31.0 MB/s | — | — |
| `cloudini`, 1 mm | 16.7 / 14.0 / 16.8 | 14.6 MB/s | 1.4 ms/cloud | 2.7–2.9 |
| `cloudini`, 1 mm, `viz_preprocessing` | 18.3 / 17.9 / 18.3 | 13.8 MB/s | 2.9 ms/cloud | 3.1–3.5 |

Lossless in every run: 300/300 clouds per lidar in 30 s, zero transform drops,
other topics at their nominal rates. Bandwidth halves at equal or slightly
lower CPU: the ~6% of a core spent encoding on the ingest thread is paid back
by zstd having a third of the bytes to compress. Run-to-run CPU noise is about
±2 points, so "CPU-neutral" is the defensible claim. `viz_preprocessing` doubles
the encode time for ~5% fewer bytes on this data, which is why it defaults to
off. Synchronous encoding in the ingest callback is sufficient; the per-topic
worker fallback was not needed.

Cloudini-side check: reusing the `PointcloudEncoder` across messages instead of
the one-shot `convertPointCloud2ToCompressedCloud` measured 0% faster with
second stage `NONE` and ~5% with `ZSTD`, so the scratch-and-copy approach above
costs nothing measurable.

## Non-goals (v1)

Per-client transform choice or parameters; client negotiation; transform
chaining; 1→N fan-out; topic renaming; image codecs; output-buffer pooling;
FastDDS/RTI wiring; CDR-level stripper.
