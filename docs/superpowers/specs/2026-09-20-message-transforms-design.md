# Message Transforms (v1: Cloudini) — Design

Date: 2026-09-20. Status: approved design, not implemented.
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
};

struct TransformOutputType {
  std::string type_name;
  std::string schema;
};

struct TransformFactory {
  std::function<bool(const std::string& source_type)> accepts;
  std::function<TransformOutputType(const std::string& source_type,
                                    const std::string& source_schema)> output;
  std::function<tl::expected<std::unique_ptr<MessageTransform>, std::string>(
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
`applyVizLossyPreprocessing` → encode. Encoding uses the lower-level
`PointcloudEncoder::encode(ConstBufferView, BufferView)` +
`writePointCloudHeader` so the result is written directly into the
`std::vector<std::byte>` (the convenience
`convertPointCloud2ToCompressedCloud` writes a `vector<uint8_t>`, which would
cost an extra output copy). Second stage `NONE`. The encoder and the
preprocessing buffer are members, reused while the field layout is unchanged.
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

- At least one of `match_type` (exact) / `match_topic` (full-match regex, same
  semantics as `topic_whitelist`); when both are present both must match.
- Ordered, first match wins.
- `strip_large_messages: true` appends one `strip` rule per strippable type
  **after** the profile's rules, so an explicit profile rule wins.

**Fail at startup** (clear error, non-zero exit): unreadable or malformed
file, unknown top-level/rule/param key, unknown transform name, `cloudini`
named in a build without Cloudini, invalid regex, `create()` returning an
error, a `match_type` the named transform does not `accept()`. A rule without
`match_type` whose transform does not `accept()` a topic it matched by
`match_topic` is reported once as a warning and the topic is left
untransformed (types are only known at discovery time).

**At runtime**, `apply()` failure → drop the sample, bump a counter, throttled
warning. This also applies to `strip` (today it forwards the original; changed
for uniformity).

## Informational protocol additions (ignored by old clients)

- `message_transforms` in the server capabilities list.
- Optional `"source_type"` on `get_topics` entries of transformed topics.
- Per-transform counters in the 5 s stats line: in bytes, out bytes, drops,
  mean µs per sample.

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

## Non-goals (v1)

Per-client transform choice or parameters; client negotiation; transform
chaining; 1→N fan-out; topic renaming; image codecs; output-buffer pooling;
FastDDS/RTI wiring; CDR-level stripper.
