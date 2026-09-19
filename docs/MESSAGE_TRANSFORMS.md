# Message Transforms — Proposed Architecture

Status: **design exploration, nothing implemented.** Opt-in, configurable
server-side transformation of message payloads (e.g. image / pointcloud
compression) behind a `MessageTransform` abstraction.

Companion to [CPU_OPTIMIZATION.md](./CPU_OPTIMIZATION.md) — several decisions
here exist specifically to avoid the CPU problems documented there.

## 1. The constraint that determines everything

The per-message wire format ([API.md](./API.md#payload-zstd-compressed)) is
`topic | timestamp | length | bytes`. **There is no per-message encoding
field**, and the `flags` header field is unusable — existing PlotJuggler
plugins reject any frame with `flags != 0` (`app/include/pj_bridge/protocol_constants.hpp:24`).

The only place a client learns how to decode a topic is the subscribe
response's `schemas[topic] = {encoding, definition}`. Therefore:

> A transform is a property of a **subscription**, chosen and advertised
> **once at subscribe time**, and stable for that subscription's lifetime. It
> can never be decided per message.

Everything below follows from that.

## 2. Where the stage goes

Today `MessageStripper` runs inside the ROS2 reader callback
(`ros2/src/ros2_subscription_manager.cpp:47`). That position is wrong for real
codecs on three counts:

1. It runs on the DDS / executor thread, so a multi-millisecond encode blocks
   the reader and causes **middleware-level** drops
   (see [CPU_OPTIMIZATION.md §2.1](./CPU_OPTIMIZATION.md#21-the-headline-the-whole-bridge-runs-on-one-thread)).
2. It runs on every message, including those the rate gate will discard —
   compressing 30 images/s to send 2 at `max_rate_hz: 2`.
3. It is ROS2-only, though the abstraction is backend-agnostic.

Proposal — an **asynchronous stage between the subscription manager and
`MessageBuffer`**:

```
reader → SubscriptionManager → [TransformPipeline] → MessageBuffer
                                 worker pool,          (holds transformed bytes)
                                 1-deep per variant
                                 ↓
         unchanged: rate gate → heavy split → serialize → zstd → send
```

The payoff of this exact placement is that **everything downstream is
untouched**: `MessageBuffer`, the rate gate, `route_message`, the serializer
and the backpressure policy never learn what a transform is. Two things then
fall out for free:

- The heavy-frame threshold applies to the **post-transform** size, which is
  what you want: a 2 MiB `Image` that becomes a 180 KiB JPEG drops below the
  256 KiB threshold and rejoins normal aggregation instead of being shed as
  heavy.
- Latched retention naturally stores the transformed sample, so replay needs
  no special case.

### Alternatives considered

- **Inside `publish_aggregated_messages()`** — simpler and correctly
  rate-aware, but a multi-millisecond codec stalls the publish cadence and
  request handling for every unrelated topic.
- **On the reader thread (today's position)** — causes middleware drops, per
  above.

## 3. The interface

```cpp
// app/include/pj_bridge/transform/message_transform.hpp
struct TopicDescriptor {
  std::string name;             // "/camera/image_raw"
  std::string type;             // "sensor_msgs/msg/Image"
  std::string schema_encoding;  // "ros2msg" | "omgidl"
  std::string schema;           // the definition text
};

struct TransformOutput {
  std::shared_ptr<std::vector<std::byte>> data;  // slots straight into BufferedMessage
  bool emit = true;                              // false = drop this sample, not an error
};

/// One instance per worker thread; implementations are single-threaded and
/// SHOULD hold reusable scratch buffers / codec contexts as members.
class MessageTransform {
 public:
  virtual ~MessageTransform() = default;
  virtual tl::expected<TransformOutput, std::string> apply(
      const std::vector<std::byte>& in, uint64_t timestamp_ns) = 0;
};

/// Registered by name; the factory answers negotiation questions without
/// instantiating a codec.
class MessageTransformFactory {
 public:
  virtual ~MessageTransformFactory() = default;
  virtual std::string name() const = 0;                     // "image_jpeg"
  virtual bool accepts(const TopicDescriptor&) const = 0;
  /// What the client is told it will receive. Computed ONCE per variant at
  /// subscribe time and cached — may be expensive.
  virtual SchemaOverride output_schema(const TopicDescriptor&) const = 0;
  virtual std::unique_ptr<MessageTransform> create(
      const TopicDescriptor&, const nlohmann::json& params) const = 0;
};
```

Three contract details that matter more than the shape:

### Per-worker instances, not a shared stateless function

Codecs want a reusable encoder context and output buffer. This is exactly the
`ZSTD_CCtx` lesson from
[CPU_OPTIMIZATION.md §1.2](./CPU_OPTIMIZATION.md#a-fresh-serializer-per-frame-throws-away-its-own-cctx-pooling):
`AggregatedMessageSerializer` holds a persistent cctx and the publish path then
throws the benefit away by constructing one per frame. Per-worker, mutable
instances mean the abstraction cannot repeat that mistake.

### `apply()` failure must drop, never fall back

`MessageStripper` currently forwards the original on failure
(`ros2/src/ros2_subscription_manager.cpp:51`). That is safe **only** because
stripping preserves the schema. A general transform cannot do it: the client
was told at subscribe time that this topic is JPEG, so forwarding raw CDR feeds
it undecodable bytes.

Failure policy must be: **drop the sample, bump a counter, throttled warning.**
Worth stating in the header, because "forward the original" is the intuitive
thing to write and it is a correctness bug here.

### One message in, zero or one out

`emit = false` covers legitimate drops (keyframe-only policies, "nothing
changed"). Fan-out to N messages is a deliberate non-goal — it would break the
1:1 topic/stream assumption the whole pipeline rests on.

## 4. Configuration and negotiation — three layers

### Layer 1: server policy decides what is permitted

Ordered rules, first match wins. Empty (the default) means the feature is
completely inert and behavior is byte-identical to today.

```yaml
transforms:
  threads: 2              # 0 = disabled
  rules:
    - match_type: "sensor_msgs/msg/Image"
      transform: image_jpeg
      mode: offer          # offer | force
      params: {quality: 80}
    - match_topic: "/lidar/.*"
      transform: pointcloud_decimate
      mode: offer
      params: {keep_ratio: 0.25}
```

`mode: offer` (the default) means a client must ask for it. `mode: force`
applies it to every subscriber — safe only because the subscribe response
always carries the authoritative `encoding`/`definition`, but it will break
clients that only speak `ros2msg`, so it must not be the default.

### Layer 2: server advertises

A new `message_transforms` capability in `kServerCapabilities`
(`app/include/pj_bridge/protocol_constants.hpp:60`), plus an optional per-topic
field in the `get_topics` entry so a client can choose before subscribing:

```json
{"name": "/camera/image_raw", "type": "sensor_msgs/msg/Image",
 "transforms": ["image_jpeg"]}
```

### Layer 3: client opts in per subscription

Alongside the existing `max_rate_hz`, in the same mixed string/object array, so
older clients are unaffected:

```json
{"command": "subscribe", "topics": [
  "/odom",
  {"name": "/camera/image_raw", "max_rate_hz": 5.0,
   "transform": "image_jpeg", "transform_params": {"quality": 60}}
]}
```

The response's `schemas["/camera/image_raw"]` then carries the **transformed**
encoding and definition, with an echo block mirroring the existing
`rate_limits` shape:

```json
{"schemas": {"/camera/image_raw": {"encoding": "ros2msg",
                                   "definition": "<CompressedImage definition>"}},
 "transforms": {"/camera/image_raw": {"name": "image_jpeg", "params": {"quality": 60}}}}
```

A requested transform that the policy does not permit, or that does not
`accept()` the topic, produces an ordinary entry in the existing `failures`
array — no new error shape.

## 5. Concurrency and cost control

The transform is the one genuinely expensive thing in the process, so the
pipeline should bound it structurally rather than by hoping:

- **Bounded worker pool** (`transforms.threads`, default 0 = disabled). Never
  scales with topic count. Must not be the ROS2 executor thread — see
  [CPU_OPTIMIZATION.md §2.3](./CPU_OPTIMIZATION.md#23-get-client-side-work-off-the-executor-thread).
- **1-deep input slot per variant, latest-wins.** Never a queue. A codec that
  cannot keep up degrades frame rate and holds memory flat; it never grows
  latency or RAM. Same policy the existing slow-client `BoundedFrameQueue`
  already uses, so it is idiomatic here.
- **Rate-hint feedback.** `BridgeServer` already knows the maximum
  `max_rate_hz` across subscribers of each variant; pushing that to the
  pipeline on subscribe/unsubscribe means you never transform a message the
  rate gate would have discarded. **This is the single biggest CPU saver in the
  feature** and it needs a deliberate seam, because the pipeline sits *upstream*
  of the rate gate and cannot discover the rate on its own.
- **No subscribers → no transform.** Variants are refcounted like subscriptions
  already are.
- **Per-transform counters** (in/out bytes, drops, µs per sample) folded into
  the existing 5 s stats line, so compression ratio and CPU cost are visible
  rather than assumed.

One thing already checked: **do not add a "skip zstd for already-compressed
payloads" path.** zstd level 1 was measured at ~6 GB/s (0.17 ms/MiB) on
incompressible data — it detects and bails. Compressing a JPEG is nearly free,
and the `flags` field could not signal it anyway. See
[CPU_OPTIMIZATION.md §1.4](./CPU_OPTIMIZATION.md#14-checked-and-deliberately-rejected).

## 6. The genuinely hard cases

### Two clients, same topic, different transforms

Subscriptions are refcounted and `MessageBuffer` holds one stream per topic, so
this is the real structural question.

Cleanest answer: key the buffer and the pipeline by **variant** =
`(topic, transform_id)` rather than by topic. One raw input fans out to the
active variants. On the wire each client still sees the plain topic name, and
because the publish path already groups clients by subscription config, folding
`transform_id` into the group key makes the two variants serialize into
separate frames automatically — each client group only ever receives its own.

This also composes with the cached 64-bit subscription signature proposed in
[CPU_OPTIMIZATION.md §1.1](./CPU_OPTIMIZATION.md#subscription-grouping-rebuilds-a-stdmap-per-client-per-cycle):
the transform id simply becomes part of the hashed tuple.

### Latched replay

The retained sample is transformed, which is correct — but only for variants
that existed when it arrived. A client subscribing to a *new* variant of a
latched topic has no retained sample to replay. Acceptable (it gets the next
live sample), but it should be documented rather than a surprise.

### Schema authority

`output_schema()` becomes the authority for that variant everywhere:
`handle_subscribe`, `include_schemas` in `get_topics`, and the
`topics_changed` notification's schema-augmented variant. All three currently
route through `extract_schema` / `attach_schema_fields`
(`app/src/bridge_server.cpp:736`, `:767`), so they should funnel through one
variant-aware resolver rather than each learning about transforms separately.

## 7. File layout and migration

```
app/include/pj_bridge/transform/
  message_transform.hpp      # interface + factory + descriptors (no ROS2/DDS deps)
  transform_registry.hpp     # name → factory, populated by main.cpp
app/src/transform/
  transform_pipeline.cpp     # worker pool, variant routing, latest-wins slots
  transform_policy.cpp       # config rules → per-topic decisions
ros2/src/transforms/         # concrete codecs that must parse ros2msg CDR
```

The core stays backend-agnostic: `app/` holds the abstraction and the
scheduling, concrete codecs that understand `sensor_msgs/msg/Image` live in
`ros2/` and self-register, and `main.cpp` wires the registry. That keeps `app/`
free of `sensor_msgs` exactly as it is free of `rclcpp` today.

**`MessageStripper` becomes the first `MessageTransform`.** It is the ideal
proving case: it already has the `should_strip` → `accepts` and `strip` →
`apply` shape. `strip_large_messages: true` becomes sugar for a built-in
`mode: force` policy rule, so the existing flag keeps working unchanged — and
it moves off the reader thread, which independently fixes the O(image)
deserialize/re-serialize documented in
[CPU_OPTIMIZATION.md §2.7](./CPU_OPTIMIZATION.md#27-two-ros2-specific-configuration-issues).

## 8. Non-goals for v1

- Changing a message's topic name
- 1→N fan-out
- Per-message transform switching (the protocol forbids it — §1)
- Client-side negotiation of codec *parameters* beyond an opaque `params` blob
- Transform chaining (composable in principle, but it multiplies the variant
  space — add only if a real case appears)

## 9. Open question to resolve first

**Does PlotJuggler's client plugin honor `schemas[topic].encoding` /
`definition` from the subscribe response, or does it resolve the type name
locally?**

If it resolves locally, `mode: offer` still works but the client needs a
matching change before any transform is usable. Worth confirming on the PJ4
side before building anything here.
