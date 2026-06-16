# SDK API Reference

> **See [SAMPLES.md](SAMPLES.md)** for runnable examples of every API surface
> documented below. In particular:
> - `basic_read.cpp` -- the reader snippets in "Reading Replays"
> - `basic_write.cpp` -- the writer snippets in "Writing Replays"
> - `basic_diff.cpp` -- a minimal hash-based delta (the `vtx_differ` facade
>   API itself is covered here in "Diffing Frames")
> - `advance_write.cpp` -- `IFrameDataSource` + integration-style mapping
>   (JSON / Protobuf / FlatBuffers) feeding the writer

## Reading Replays

### Opening a File

```cpp
#include "vtx/reader/core/vtx_reader_facade.h"

auto ctx = VTX::OpenReplayFile("replay.vtx");
if (!ctx) {
    // ctx.GetError() contains the failure reason
    return;
}

// ctx.format    — VTX::VtxFormat::FlatBuffers or VTX::VtxFormat::Protobuf
// ctx.size_in_mb — file size in megabytes
// ctx.reader    — the IVtxReaderFacade*
// ctx.chunk_state — ReaderChunkState* (events pre-wired)
```

`OpenReplayFile()` auto-detects the wire format from magic bytes (`VTXF` / `VTXP`), creates the correct reader implementation, calculates file size, and wires chunk-state events to the built-in `ReaderChunkState` tracker.

### Accessing Frames

```cpp
auto& reader = ctx.reader;

// Total frame count
int32_t total = reader->GetTotalFrames();

// Synchronous read (blocks until chunk is loaded)
const VTX::Frame* frame = reader->GetFrameSync(42);

// Copy-based read
VTX::Frame frame_copy;
bool ok = reader->GetFrame(42, frame_copy);

// Range read
std::vector<VTX::Frame> frames;
reader->GetFrameRange(0, 99, frames);  // frames 0..99

// Context window around a center frame
auto context = reader->GetFrameContext(50, 5, 5);  // frames 45..55
```

### Navigating Frame Data

```cpp
const VTX::Frame* frame = reader->GetFrameSync(0);

for (const auto& bucket : frame->GetBuckets()) {
    for (size_t i = 0; i < bucket.entities.size(); ++i) {
        const auto& entity = bucket.entities[i];
        const auto& uid = bucket.unique_ids[i];

        // Access typed properties by index (mapped via schema)
        float hp = entity.float_properties[0];
        VTX::Vector pos = entity.vector_properties[0];

        // Access arrays (SoA)
        auto inventory = entity.int32_arrays.GetSubArray(0);
    }
}
```

### Property Address Cache

For O(1) property lookup by name instead of raw index:

```cpp
auto cache = reader->GetPropertyAddressCache();
auto schema = reader->GetContextualSchema();

// Use cache to resolve "Health" -> (float_properties, index 3)
```

A `VTX::FrameAccessor` (from `reader->CreateAccessor()`) turns names into cached `PropertyKey`s, and `VTX::EntityView` reads each container kind by key:

```cpp
VTX::FrameAccessor acc = reader->CreateAccessor();
VTX::EntityView    e(bucket.entities[i]);

float        hp    = e.Get(acc.Get<float>("Player", "Health"));         // scalar
auto         inv   = e.GetArray(acc.GetArray<int32_t>("Player", "Inventory"));   // scalar array
VTX::EntityView loadout = e.GetView(acc.GetViewKey("Player", "Loadout"));        // nested struct
auto         items = e.GetViewArray(acc.GetViewArrayKey("Player", "Inventory")); // array of structs
VTX::MapView ammo  = e.GetMap(acc.GetMapKey("Player", "AmmoByWeapon"));          // map
int rifle = VTX::EntityView(ammo.At("Rifle")).Get(acc.Get<int32_t>("AmmoEntry", "Ammo"));
```

The codegen (`scripts/vtx_codegen.py`) wraps these into typed `XView`/`XMutator` accessors so call sites use `player.GetHealth()` / `player.GetAmmoByWeapon()` with no keys or schema strings.

### Chunk State Monitoring

```cpp
// Chunk events are auto-wired by OpenReplayFile()
VTX::ReaderChunkSnapshot snapshot = ctx.chunk_state->GetSnapshot();
// snapshot.loaded_chunks  — chunk indices currently in RAM
// snapshot.loading_chunks — chunk indices being loaded asynchronously
```

### Cache Window Control

```cpp
// Keep 3 chunks behind and 3 ahead of the active chunk
reader->SetCacheWindow(3, 3);
```

### Seek Table

```cpp
const auto& seek_table = reader->GetSeekTable();
for (const auto& entry : seek_table) {
    // entry.chunk_index, entry.start_frame, entry.end_frame
    // entry.file_offset, entry.chunk_size_bytes
}
```

### File Metadata

```cpp
VTX::FileHeader header = reader->GetHeader();
// header.replay_name, header.replay_uuid, header.recorded_utc_timestamp
// header.version.format_major, header.version.format_minor, header.version.schema_version
// header.custom_json_metadata

VTX::FileFooter footer = reader->GetFooter();
// footer.total_frames, footer.duration_seconds, footer.payload_checksum
// footer.chunk_index (seek table), footer.events (timeline events)
// footer.times (ReplayTimeData)
```

### Raw Frame Bytes

For advanced use (diffing, custom serialization):

```cpp
std::span<const std::byte> raw = reader->GetRawFrameBytes(42);
// Returns a view into the cached chunk's decompressed buffer.
// Valid only while the chunk is resident in cache.
```

---

## Writing Replays

### Creating a Writer

```cpp
#include "vtx/writer/core/vtx_writer_facade.h"

VTX::WriterFacadeConfig config;
config.output_filepath  = "output.vtx";
config.schema_json_path = "schema.json";
config.replay_name      = "My Replay";
config.replay_uuid      = "550e8400-e29b-41d4-a716-446655440000";
config.chunk_max_frames = 1000;
config.chunk_max_bytes  = 10 * 1024 * 1024;  // 10 MB
config.use_compression  = true;
config.default_fps      = 60.0f;

auto writer = VTX::CreateFlatBuffersWriterFacade(config);
// or: VTX::CreateProtobufWriterFacade(config);
```

The schema can be supplied three ways (precedence: **registry > content > path**):

```cpp
config.schema_json_path    = "schema.json";        // load from a file (default)
config.schema_json_content = json_string;          // OR a raw in-memory JSON string
config.schema_registry     = my_shared_registry;   // OR a pre-built std::shared_ptr<VTX::SchemaRegistry> (copied in, no re-parse)
```

Missing parent directories of `output_filepath` are created automatically (`config.create_output_dirs`, default `true`; set it `false` to require the directory to pre-exist).

### Recording Frames

```cpp
VTX::Frame frame;
auto& bucket = frame.CreateBucket("World");

// Add entities
bucket.unique_ids.push_back("player_001");
VTX::PropertyContainer entity;
entity.float_properties.push_back(100.0f);   // Health
entity.vector_properties.push_back({1.0, 2.0, 3.0});  // Position
bucket.entities.push_back(std::move(entity));

// Record with game time
VTX::GameTime::GameTimeRegister time_reg;
time_reg.game_time = 1.5f;
time_reg.utc_timestamp = /* unix timestamp */;

writer->RecordFrame(frame, time_reg);
```

### Strict recording, finalization & rejection

`RecordFrame` is fire-and-forget. For an observable outcome use `TryRecordFrame`, which returns a `RecordResult`:

```cpp
VTX::RecordResult r = writer->TryRecordFrame(frame, time_reg);
if (!r.IsWritten()) {
    // r.error is a VtxError: code (e.g. GameTimeRejected / EntityTypeUnresolved),
    // message, source_api. A bad game-time registry no longer rolls back silently.
    VTX_WARN("frame rejected: {}", r.error.message);
} else {
    // r.frame_index is the writer-assigned, monotonic index.
}
```

Before a frame enters the chunk pipeline the writer **finalizes** it: validates that every
entity type resolves to a schema struct, recomputes each entity's `content_hash` after all
schema fields and post-processor overrides, then **freezes** the frame (any mutation handle a
post-processor stashed is revoked).

Optionally retain the last finalized frame as a read-only snapshot (off by default -- it costs
a full-frame copy per frame):

```cpp
config.retain_finalized_snapshot = true;
// ...after a written TryRecordFrame:
const VTX::PropertyContainer* e = writer->FindEntity("World", "player_001");
const VTX::Frame* snap = writer->GetLastFinalizedFrame();
```

The one-call pipeline reports outcomes structurally:

```cpp
VTX::RecordPipeline pipeline(std::move(source), std::move(writer));
VTX::PipelineReport report = pipeline.Run();
// report.written / .rejected / .skipped, with rejections split into
// .validation_errors vs .timer_errors, plus report.errors (one VtxError per rejected frame).
```

### Finishing

```cpp
writer->Flush();  // Force-write any buffered chunk
writer->Stop();   // Write footer and close file
```

If the facade is destroyed without an explicit `Stop()`, its destructor finalizes the replay as a best-effort fallback, so a dropped writer still yields a readable `.vtx`.

### One call: `WriteReplay`

When you already have an `IFrameDataSource` and just want a finished `.vtx`, `WriteReplay` runs the whole pipeline in one call -- create the writer, initialize the source, drain every frame through `TryRecordFrame`, finalize, and report:

```cpp
#include "vtx/writer/core/vtx_write_replay.h"

VTX::WriterFacadeConfig config;
config.output_filepath  = "output.vtx";
config.schema_json_path = "schema.json";   // or schema_json_content / schema_registry

MyFrameDataSource source;   // implements VTX::IFrameDataSource

VTX::WriteReplayResult result =
    VTX::WriteReplay(config, source, VTX::SerializationFormat::Flatbuffers);

if (!result.ok) {
    VTX_ERROR("write failed: {}", result.error.message);   // e.g. SchemaInvalid / source-init failure
} else {
    // result.frames_written / .frames_dropped / .total_frames
    // result.output_path / .elapsed_seconds
}
// Frames rejected by finalization/timer are counted in frames_dropped and
// surfaced one-per-warning in result.warnings; the call still produces a
// valid replay of the accepted frames.
```

`SerializationFormat` picks the backend (`Flatbuffers` default, or `Protobuf`).

### Streaming sinks

Same writer API, different transport.  Instead of writing the `.vtx` byte stream to a file, push it over a TCP socket to a remote ingestion server.  Use `CreateFlatBuffersNetworkWriterFacade` / `CreateProtobufNetworkWriterFacade` and pass a `NetworkWriterFacadeConfig`:

```cpp
#include "vtx/writer/core/vtx_writer_facade.h"

VTX::NetworkWriterFacadeConfig config;
config.host             = "127.0.0.1";
config.port             = 9000;
config.schema_json_path = "schema.json";
config.replay_name      = "Live Replay";
config.chunk_max_frames = 1000;
config.use_compression  = true;

auto writer = VTX::CreateFlatBuffersNetworkWriterFacade(config);
// RecordFrame / Flush / Stop exactly as with the file sink
```

The writer behaves identically behind the `IVtxWriterFacade` abstraction -- the same wire bytes that a `.vtx` file holds are sent over the socket in order, so the receiver just appends the incoming message bytes to a file and gets a valid `.vtx` parseable by `VTX::OpenReplayFile`.

---

## Frame Post-Processor

A writer-side hook that runs on every `RecordFrame()` call, **after** timer validation and **before** the serializer touches the native `Frame`. Whatever the processor mutates lands on disk -- callers who re-read the `.vtx` see the post-processed values.

See [`POST_PROCESSING.md`](POST_PROCESSING.md) for the full reference (lifecycle, threading, structural mutation, error handling, performance, codegen integration). This section is the API cheat-sheet.

### Implementing a processor

```cpp
#include "vtx/writer/core/vtx_frame_post_processor.h"
#include "vtx/writer/core/vtx_frame_mutation_view.h"

class HealthClampProcessor : public VTX::IFramePostProcessor {
public:
    // Called once, synchronously, inside SetPostProcessor.  Resolve every
    // PropertyKey<T> here -- the schema is constant for the recording session.
    void Init(const VTX::FramePostProcessorInitContext& ctx) override {
        health_key_ = ctx.frame_accessor->Get<float>("Player", "Health");
    }

    // Called once per RecordFrame, on the writer's calling thread.
    void Process(VTX::FrameMutationView& view,
                 const VTX::FramePostProcessContext& ctx) override {
        if (!view.HasBucket("entity") || !health_key_.IsValid()) return;
        auto bucket = view.GetBucket("entity");
        for (auto entity : bucket) {
            if (entity.raw()->entity_type_id != /*Player*/ 0) continue;
            if (entity.Get(health_key_) < 0.0f) entity.Set(health_key_, 0.0f);
        }
    }

    // Optional: reset cross-frame accumulators.  Called by writer destructor
    // or explicit ClearPostProcessor().
    void Clear() override {}

    // Optional: telemetry dump.  Called when the caller invokes it.
    void PrintInfo() const override {}

private:
    VTX::PropertyKey<float> health_key_ {-1};
};
```

### Registering on the writer

```cpp
auto writer    = VTX::CreateFlatBuffersWriterFacade(config);
auto processor = std::make_shared<HealthClampProcessor>();
writer->SetPostProcessor(processor);   // Init() runs here

for (...) { writer->RecordFrame(frame, time); }   // Process() runs per frame
writer->Stop();
// Writer's destructor calls processor->Clear()
// OR call writer->ClearPostProcessor() explicitly first.
```

### Composing multiple processors

```cpp
auto chain = std::make_shared<VTX::FramePostProcessorChain>();
chain->Add(std::make_shared<HealthClampProcessor>());
chain->Add(std::make_shared<DeathConsistencyProcessor>());
chain->Add(std::make_shared<KillStreakProcessor>());
writer->SetPostProcessor(chain);
```

`Init`, `Process`, and `PrintInfo` run in registration order. `Clear` runs in reverse (destructor-like teardown). If two processors `Set` the same property, the last one wins.

### Strongly-typed accessors via codegen

`scripts/vtx_codegen.py` produces per-struct `XMutator` classes and `ForEachX` helpers for any schema. The processor becomes string-free:

```cpp
#include "arena_generated.h"   // produced by vtx_codegen.py

class HealthClampProcessor : public VTX::IFramePostProcessor {
public:
    void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext&) override {
        if (!view.HasBucket("entity")) return;
        auto bucket = view.GetBucket("entity");
        VTX::ArenaSchema::ForEachPlayer(bucket, *view.accessor(), [](auto& p) {
            if (p.GetHealth() < 0) p.SetHealth(0);
        });
    }
};
```

No `Init`, no `PropertyKey<T>` members, no `entity_type_id` gating, no schema strings. See [`POST_PROCESSING.md`](POST_PROCESSING.md#strongly-typed-via-codegen-recommended) and [`SAMPLES.md`](SAMPLES.md#6-post_process_write----post-processor-end-to-end).

### Mutation view API

```cpp
// Per-entity reads / writes (writer-side mirror of EntityView)
template <VtxScalarType T> T    Get(PropertyKey<T> key) const;
template <VtxScalarType T> void Set(PropertyKey<T> key, T value);

// Nested struct mutation (Champion.Spells[i].Cooldown style)
EntityMutator GetMutableView(PropertyKey<EntityView> key);

// Array mutation
template <VtxArrayType T> std::span<T> GetMutableArray(PropertyKey<T> key);

// Bucket-level (BucketMutator)
EntityMutator AddEntity();                          // inject synthetic entity
void          RemoveEntity(uint32_t entity_index);
template <class P> size_t RemoveIf(P predicate);    // bulk filter; P : bool(EntityView)
void          Clear();
```

Out-of-range / invalid-key writes are **silent no-ops**, matching the read-side tolerance. The serializer drops entities whose `entity_type_id < 0`, so newly-added entities must have their type id set explicitly before the chunk is flushed (see [`POST_PROCESSING.md` gotchas](POST_PROCESSING.md#structural-mutation)).

---

## Diffing Frames

### Creating a Differ

```cpp
#include "vtx/differ/core/vtx_differ_facade.h"

auto differ = VtxDiff::CreateDifferFacade(VTX::VtxFormat::FlatBuffers);
// or: VtxDiff::CreateDifferFacade(VTX::VtxFormat::Protobuf);
```

### Computing a Diff

```cpp
// Get raw bytes from reader
auto raw_a = reader->GetRawFrameBytes(frame_a);
std::vector<std::byte> bytes_a(raw_a.begin(), raw_a.end());  // copy (B may evict A's chunk)

auto raw_b = reader->GetRawFrameBytes(frame_b);

VtxDiff::DiffOptions opts;
opts.compare_floats_with_epsilon = true;
opts.float_epsilon = 1e-5f;

VtxDiff::PatchIndex patch = differ->DiffRawFrames(bytes_a, raw_b, opts);

for (const auto& op : patch.operations) {
    // op.Operation  — Add, Remove, Replace, ReplaceRange
    // op.ContainerType — which property type changed
    // op.Path       — binary index path to the change
    // op.ActorId    — entity unique ID (if applicable)
}
```

---

## Core Types

### VtxFormat

```cpp
enum class VtxFormat : uint8_t { Unknown = 0, FlatBuffers, Protobuf };
```

### PropertyContainer

The central data container. Holds typed property vectors (SoA layout):

- Scalars: `bool_properties`, `int32_properties`, `int64_properties`, `float_properties`, `double_properties`, `string_properties`
- Spatial: `transform_properties`, `vector_properties`, `quat_properties`, `range_properties`
- Arrays: `int32_arrays`, `float_arrays`, `string_arrays`, `vector_arrays`, etc. (all `FlatArray<T>`)
- Nested: `any_struct_properties`, `any_struct_arrays`, `map_properties`, `map_arrays`
- Metadata: `entity_type_id`, `content_hash`

### FlatArray\<T\>

Structure of Arrays container with sub-array support:

```cpp
FlatArray<int32_t> arr;
arr.AppendSubArray({1, 2, 3});       // Sub-array 0
arr.AppendSubArray({10, 20});         // Sub-array 1
auto span = arr.GetSubArray(0);       // {1, 2, 3}
arr.PushBack(1, 30);                  // Sub-array 1 is now {10, 20, 30}
```

### Frame

Contains one or more `Bucket` instances, each holding a list of entities:

```cpp
VTX::Frame frame;
auto& bucket = frame.CreateBucket("Players");
bucket.unique_ids.push_back("p1");
bucket.entities.push_back(VTX::PropertyContainer{});
```

---

## Error Handling

The SDK uses one structured model (`vtx/common/vtx_diagnostics.h`) so failures can be handled
programmatically instead of parsed out of logs:

- **`VtxDiagnostic`** (aliases **`VtxError`** / **`VtxWarning`**) -- `code` (`VtxErrorCode`),
  `severity` (`Severity`), `message`, plus location/context: `frame_index`, `bucket`,
  `unique_id`, `entity_type`, `field_path`, `expected_type`/`expected_container`,
  `provided_type`/`provided_container`, `source_api`. Has `ToString()` and `operator<<`.
- **`VtxResult<T>`** -- `{ ok, value, error, warnings }`; `VtxStatus` = `VtxResult<Unit>`.
  Build with `Success(...)` / `Failure(...)`.
- **`ValidationReport`** -- aggregated diagnostics (`HasErrors` / `ok` / `ErrorCount` /
  `WarningCount` / `Errors` / `Warnings` / `ToString`).

Where it surfaces:

- `OpenReplayFile()` -> `ReaderContext`; check `if (!ctx)` then `ctx.GetError()` (a `VtxError`).
- Reader ready-state: `IsReadyFailed()` + `GetReadyError()` (a `VtxError`);
  `ReplayReaderEvents::OnReadyFailed(const VtxError&)`.
- Writer: `TryRecordFrame` -> `RecordResult` (carries a `VtxError`); `RecordPipeline::Run` ->
  `PipelineReport`.
- Strict accessors `TryResolve` / `TryGet` / `TrySet` -> `VtxResult`.
- Validation passes (below) -> `ValidationReport`.

Tolerant APIs are kept where useful: reader methods still return `nullptr`/`false` on invalid
indices, factory functions return `nullptr` for unsupported formats, and `VTX_ERROR(...)`
logging is unchanged.

---

## Validation

Validation is split into composable passes (`vtx/common/vtx_validation.h`, plus
`vtx/reader/core/vtx_replay_validation.h`), each returning a `ValidationReport`:

```cpp
#include "vtx/common/vtx_validation.h"
#include "vtx/reader/core/vtx_replay_validation.h"

// Schema document (raw JSON) -- wraps the rule-based SchemaValidator.
VTX::ValidationReport r1 = VTX::ValidateSchema(json_string);

// One entity / a whole frame against a loaded schema (SchemaRegistry or PropertyAddressCache).
VTX::ValidationReport r2 = VTX::ValidateEntity(entity, registry);
VTX::ValidationReport r3 = VTX::ValidateFrame(frame, registry, /*frame_index*/ 0);

// A whole replay: reuse an already-open reader (no re-open) or open by path.
VTX::ValidationReport r4 = VTX::ValidateReplay(*ctx.reader);
VTX::ValidationReport r5 = VTX::ValidateReplayFile("replay.vtx");

if (!r5.ok()) {
    for (const auto& d : r5.Diagnostics())
        VTX_ERROR("{}", d.ToString());
}
```

`ValidateEntity` / `ValidateFrame` flag unresolved entity types (`EntityTypeUnresolved`),
per-type arrays larger than the schema declares (`FieldIndexOutOfRange`), and duplicate
`unique_id`s within a bucket (`DuplicateUniqueId`).

### Strict accessors

Type-safe reads/writes that report failures instead of returning a default or silently
no-opping (the tolerant `Get` / `Set` stay unchanged):

```cpp
// Resolve a key strictly: TypeMismatch carries expected vs provided type, NotFound otherwise.
VTX::VtxResult<VTX::PropertyKey<float>> k = accessor.TryResolve<float>("Player", "Health");

VTX::VtxResult<float> v = entityView.TryGet(k.value);    // FieldIndexOutOfRange on a short entity
VTX::VtxStatus s = entityMutator.TrySet(k.value, 50.0f); // refused if the frame is frozen
```

---

## Integration Primitives

For ingesting third-party data formats into `.vtx` replays, the SDK exposes
three template extension points plus a streaming `IFrameDataSource` interface.
Full worked examples live in `samples/advance_write.cpp` (arena) and
`tools/integrations/` (Rocket League, League of Legends, Street Fighter).

### IFrameDataSource

```cpp
#include "vtx/writer/core/vtx_data_source.h"

class MyDataSource : public VTX::IFrameDataSource {
public:
    bool   Initialize() override;                   // open/parse the source
    bool   GetNextFrame(VTX::Frame& out_frame,
                        VTX::GameTime::GameTimeRegister& out_time) override;
    size_t GetExpectedTotalFrames() const override; // 0 if unknown / streaming
};
```

Drive it manually against an `IVtxWriterFacade`:

```cpp
my_source.Initialize();
VTX::Frame frame;
VTX::GameTime::GameTimeRegister time;
while (my_source.GetNextFrame(frame, time)) {
    writer->RecordFrame(frame, time);
}
writer->Flush();
writer->Stop();
```

### JsonMapping\<T\> (compile-time JSON reflection)

```cpp
#include "vtx/common/adapters/json/json_policy.h"
#include "vtx/common/readers/frame_reader/type_traits.h"
#include "vtx/common/readers/frame_reader/universal_deserializer.h"
#include "vtx/common/adapters/json/json_adapter.h"

struct MyPlayer { std::string id; int score; };

template <> struct VTX::JsonMapping<MyPlayer> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("id",    &MyPlayer::id),
            MakeField("score", &MyPlayer::score)
        );
    }
};

auto node   = nlohmann::json::parse(raw);
auto player = VTX::UniversalDeserializer<>::Load<MyPlayer>(VTX::JsonAdapter(node));
```

`UniversalDeserializer` recurses through `std::vector<T>`, `std::map<K,V>`, and nested mapped types. The only thing you supply is the `GetFields()` tuple.

### ProtoBinding\<T\>  -- Protobuf into PropertyContainer

```cpp
#include "vtx/common/readers/frame_reader/protobuff_loader.h"

template <> struct VTX::ProtoBinding<my_pb::Player> {
    static void Transfer(const my_pb::Player& src,
                         VTX::PropertyContainer& dest,
                         VTX::GenericProtobufLoader& loader,
                         const std::string& schema_name) {
        loader.LoadField (dest, schema_name, "UniqueID", src.id());
        loader.LoadField (dest, schema_name, "Score",    src.score());
        loader.LoadStruct(dest, schema_name, "Position", src.position());   // nested msg
        loader.LoadArray (dest, schema_name, "Inventory", src.inventory()); // repeated
    }
};
template <> struct VTX::ProtoBinding<my_pb::Frame> {
    static void TransferToFrame(const my_pb::Frame& src,
                                VTX::Frame& dest,
                                VTX::GenericProtobufLoader& loader,
                                const std::string& schema_name) {
        auto& bucket = dest.GetBucket("entity");
        loader.AppendActorList(bucket, "Player", src.players(),
            [](const my_pb::Player& p) { return p.id(); });
    }
};

// At runtime:
VTX::SchemaRegistry schema;
schema.LoadFromJson("schema.json");
VTX::GenericProtobufLoader loader(schema);
loader.LoadFrame(proto_frame, vtx_frame, "MyFrameSchema");
```

Field names resolve against the VTX property schema at load time.

### FlatBufferBinding\<T\>  -- FlatBuffers into PropertyContainer

```cpp
#include "vtx/common/readers/frame_reader/flatbuffer_loader.h"

template <> struct VTX::FlatBufferBinding<my_fb::Player> {
    static void Transfer(const my_fb::Player* src,
                         VTX::PropertyContainer& dest,
                         VTX::GenericFlatBufferLoader& loader,
                         const std::string& schema_name) {
        if (src->id())   loader.LoadField(dest, schema_name, "UniqueID", src->id()->str());
        loader.LoadField(dest, schema_name, "Score", src->score());
    }
};

// At runtime:
VTX::GenericFlatBufferLoader loader(schema.GetPropertyCache());
loader.LoadFrame(fb_frame_ptr, vtx_frame, "MyFrameSchema");
```

Unlike `ProtoBinding`, FlatBuffer field addresses are resolved once per field name through the `PropertyAddressCache` and cached inside the loader -- subsequent frames hit O(1) lookups keyed by `entity_type_id`.

### Which primitive should I use?

| Source format | Use | Why |
|---|---|---|
| JSON with stable field names | `JsonMapping<T>` + `UniversalDeserializer` | Cleanest -- one tuple per type |
| Protobuf message | `ProtoBinding<T>` + `GenericProtobufLoader` | Handles `has_*`/`set_*` + repeated fields |
| FlatBuffers table | `FlatBufferBinding<T>` + `GenericFlatBufferLoader` | Zero-copy reads, cached addresses |
| Completely custom wire format | Hand-build `VTX::PropertyContainer` inside your `GetNextFrame()` | No bindings needed |

---

## Streaming sources

For ingestion from external processes that produce frames live (game injectors, capture daemons, CLI bridges), the SDK ships two ready-made `IFrameDataSource` implementations that handle the transport + per-frame framing.  Both are templates parameterised by an `Adapter` that interprets the payload bytes -- the transport stays format-agnostic.

### PipeFrameDataSource\<Adapter\>  -- OS pipes (stdin, Windows named pipes, POSIX FIFOs)

```cpp
#include "vtx/writer/sources/pipe_frame_source.h"

struct MyJsonAdapter {
    bool ParseFrame(std::span<const std::byte> payload,
                    VTX::Frame& out_frame,
                    VTX::GameTime::GameTimeRegister& out_time) const {
        /* decode payload bytes -> out_frame */
        return true;
    }
};

VTX::PipeFrameDataSource<MyJsonAdapter>::Config cfg;
cfg.pipe_path = "\\\\.\\pipe\\vtx";   // Windows named pipe;  or  "/tmp/vtx.fifo"  on POSIX
cfg.as_server = true;                  // VTX creates the pipe and waits for a producer
VTX::PipeFrameDataSource<MyJsonAdapter> source(cfg);
source.Initialize();                   // creates the pipe, blocks until a client connects
```

Wire protocol: `[uint32 LE size][payload of size bytes]` repeated, terminated by a zero-size sentinel.

Three modes via `Config`:

| `pipe_path` | `as_server` | Behaviour |
|---|---|---|
| empty | (ignored) | read from `stdin` (for shell pipelines `producer \| vtx_consumer`) |
| set | `false` | connect to a pipe / FIFO a producer already created (VTX is the client) |
| set | `true` | **create** the pipe (Windows `CreateNamedPipe`, POSIX `mkfifo`) and block until a producer connects (VTX is the server) |

Server mode is the mode for an **external, independent producer** -- e.g. a game injector that opens the named pipe as a client when its game launches.  The producer needs nothing from the VTX SDK; it just speaks the framing.  See [`SAMPLES.md`](SAMPLES.md) for the demo `.bat` scripts.

### WebSocketFrameDataSource\<Adapter\>  -- WebSocket (ws:// + wss://)

```cpp
#include "vtx/writer/sources/websocket_frame_source.h"

VTX::WebSocketFrameDataSource<MyJsonAdapter>::Config cfg;
cfg.url = "ws://127.0.0.1:8765/";     // or "wss://host/path" for TLS
VTX::WebSocketFrameDataSource<MyJsonAdapter> source(cfg);
source.Initialize();                   // TCP connect + WebSocket handshake; blocks until open
```

VTX acts as a WebSocket client.  Each incoming WebSocket message is one frame, handed to the `Adapter`.  Ping/pong, close, fragment reassembly are handled internally.  `wss://` verifies the server certificate against the OS trust store by default.  Auto-reconnect is disabled -- a dropped connection ends the stream so the writer finalises the `.vtx` instead of silently resuming mid-file.

Backed by IXWebSocket + mbedTLS, both hidden behind a PIMPL boundary -- the public header carries no transitive dependency on either.

> **Experimental / WIP -- not functional yet.** A third source,
> `SharedMemoryFrameDataSource` (zero-copy SPSC ring over a shared-memory segment,
> with a pluggable `ISharedMemoryTransport`), exists in-tree under
> `sdk/include/vtx/writer/sources/shared_memory_frame_source.h` but **is work in
> progress and not yet functional** -- it landed unintentionally and is not a
> supported input path. Treat the headers as a preview; do not depend on them.

### IFramePayloadAdapter (shared concept)

Both streaming sources constrain their `Adapter` template parameter with the same compile-time concept:

```cpp
#include "vtx/writer/sources/frame_payload_adapter.h"

template <typename A>
concept IFramePayloadAdapter =
    requires(A& a, std::span<const std::byte> payload,
             VTX::Frame& f, VTX::GameTime::GameTimeRegister& t) {
        { a.ParseFrame(payload, f, t) } -> std::convertible_to<bool>;
    };
```

The source owns the transport and the framing; the adapter is the only place that knows the wire payload format (JSON, Protobuf, custom binary).  Returning `false` ends the stream.
