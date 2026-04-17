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
// or: VTX::CreateProtobuffWriterFacade(config);
```

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

### Finishing

```cpp
writer->Flush();  // Force-write any buffered chunk
writer->Stop();   // Write footer and close file
```

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

- `OpenReplayFile()` returns a `ReaderContext`. Check with `if (!ctx)` and read `ctx.GetError()`.
- Reader methods return `nullptr` or `false` on invalid frame indices.
- Writer and differ factory functions return `nullptr` for unsupported formats.
- Internal errors are logged via `VTX_ERROR(...)` to the thread-safe logger.

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
