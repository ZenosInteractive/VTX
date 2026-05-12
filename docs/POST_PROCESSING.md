# Frame Post-Processor

A writer-side hook that runs on every `RecordFrame()` call, **after** timer validation succeeds and **before** the frame is handed to the serializer. Whatever the processor mutates is what ends up in the on-disk `.vtx` file.

This is the canonical extension point for game-specific or integration-specific transformations: clamp out-of-range values, derive consistency state (e.g. `IsAlive` from `Health`), filter or inject entities, accumulate cross-frame statistics, branch behaviour by schema version.

The SDK provides the **pipeline** (interface, chain, mutation view, lifecycle). Each integration provides the **logic** (processor classes) and, optionally, **strongly-typed accessors** via `scripts/vtx_codegen.py`.

## Concepts

```
                +-----------------------+
RecordFrame --->|  timer validation     |
                +-----------+-----------+
                            |
                            v
                +-----------------------+
                |  POST-PROCESSOR HOOK  |  <-- IFramePostProcessor::Process
                +-----------+-----------+
                            |
                            v
                +-----------------------+
                |  Serializer::FromNat  |  <-- mutations land in wire bytes
                +-----------+-----------+
                            |
                            v
                       (.vtx file)
```

| Concept | Where it lives | What it does |
|---|---|---|
| `IFramePostProcessor` | `vtx/writer/core/vtx_frame_post_processor.h` | Interface that integrators implement. Four hooks: `Init`, `Process`, `Clear`, `PrintInfo`. |
| `FramePostProcessorChain` | same | Composable container of processors. Implements `IFramePostProcessor` itself, so a chain is registered just like a single processor. |
| `FrameMutationView` | `vtx/writer/core/vtx_frame_mutation_view.h` | Write-side mirror of `EntityView`/`FrameAccessor`. Entry point the processor receives per frame. |
| `EntityMutator` / `BucketMutator` | same | Cheap non-owning wrappers. `Get<T>` (read) + `Set<T>` (write) + iteration + structural mutation. |
| `FramePostProcessorInitContext` | `vtx_frame_post_processor.h` | Passed to `Init` once per registration: schema accessor, total frames, version info. |
| `FramePostProcessContext` | same | Passed to `Process` per frame: `global_frame_index`, schema version, frame accessor. |

## Lifecycle

| Method | When the writer calls it | Override? |
|---|---|---|
| `Init(InitContext)` | Once, synchronously inside `SetPostProcessor()` -- BEFORE the new processor becomes visible to any `RecordFrame()` | Optional. Resolve every `PropertyKey<T>` upfront and load external resources here. |
| `Process(view, ctx)` | Once per `RecordFrame()`, on the writer's calling thread | **Mandatory.** The hot path. |
| `Clear()` | Writer's destructor, OR explicit `ClearPostProcessor()` | Optional. Reset cross-frame accumulators here. |
| `PrintInfo() const` | Whenever the caller invokes it | Optional. Telemetry dump. |

`Clear` is **not** called on `Stop()` -- the writer can be re-used between `Stop` and destruction (e.g. caller does `Stop`, inspects telemetry, then drops the writer). `Clear` is the destructor's job, or use `ClearPostProcessor()` for explicit teardown.

## Threading model

The writer is **single-threaded by design**: `RecordFrame()` is called sequentially from the user's capture loop. No mutex is needed on `post_processor_`. `SetPostProcessor` and `RecordFrame` are expected to be called from the same thread.

This is different from the reader-side processor model some other VTX features use (e.g. async chunk loads). The writer's simplicity is deliberate.

## API at a glance

```cpp
namespace VTX {

    struct FramePostProcessorInitContext {
        const FrameAccessor* frame_accessor = nullptr;
        int32_t  total_frames   = 0;
        uint32_t schema_version = 0;
        uint32_t format_major   = 0;
        uint32_t format_minor   = 0;
    };

    struct FramePostProcessContext {
        int32_t  global_frame_index      = 0;
        int32_t  chunk_local_frame_index = 0;
        int32_t  chunk_index             = 0;
        uint32_t schema_version          = 0;
        const FrameAccessor* frame_accessor = nullptr;
        const Frame*         previous_frame = nullptr; // null on writer-side
    };

    class IFramePostProcessor {
    public:
        virtual ~IFramePostProcessor() = default;
        virtual void Init(const FramePostProcessorInitContext&) {}
        virtual void Clear() {}
        virtual void PrintInfo() const {}
        virtual void Process(FrameMutationView& view,
                             const FramePostProcessContext& ctx) = 0;
    };

    class FramePostProcessorChain final : public IFramePostProcessor {
    public:
        void Add(std::shared_ptr<IFramePostProcessor>);
        bool Remove(const std::shared_ptr<IFramePostProcessor>&);
        size_t size() const noexcept;
        // Init / Process / PrintInfo run in registration order.
        // Clear runs in REVERSE order (destructor-like teardown).
    };

} // namespace VTX
```

Registering on a writer:

```cpp
auto writer    = VTX::CreateFlatBuffersWriterFacade(config);
auto processor = std::make_shared<MyProcessor>();
writer->SetPostProcessor(processor);   // Init() runs here

// ... RecordFrame() loop ...

writer->ClearPostProcessor();          // explicit teardown
// OR let writer destructor handle it
```

`SetPostProcessor` does **not** invoke `Clear()` on whatever was previously registered. The previous processor is simply dropped; if you want explicit teardown of an outgoing processor, keep your own `shared_ptr` and call `Clear()` yourself.

## Two ways to write a processor

### A. Generic API (no codegen)

Resolve `PropertyKey<T>` by name once in `Init`, then use them in the hot path:

```cpp
class HealthClampProcessor : public VTX::IFramePostProcessor {
public:
    void Init(const VTX::FramePostProcessorInitContext& ctx) override {
        health_key_ = ctx.frame_accessor->Get<float>("Player", "Health");
    }

    void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext&) override {
        if (!view.HasBucket("entity") || !health_key_.IsValid()) return;
        auto bucket = view.GetBucket("entity");
        for (auto entity : bucket) {
            if (entity.raw() && entity.raw()->entity_type_id != /*Player type_id*/ 0) continue;
            const float hp = entity.Get(health_key_);
            if (hp < 0) entity.Set(health_key_, 0);
        }
    }

private:
    VTX::PropertyKey<float> health_key_ {-1};
};
```

Works on any schema. Drawback: you carry strings and you have to gate on `entity_type_id` yourself.

### B. Strongly-typed via codegen (recommended)

Run `scripts/vtx_codegen.py` over your schema JSON to produce typed views, mutators, and per-struct `ForEachX` helpers. The processor stops carrying strings entirely:

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

- No `PropertyKey<T>` members.
- No `Init()` (the codegen-generated `PlayerMutator` caches keys in `static` locals on first use).
- No manual `entity_type_id` gate (`ForEachPlayer` filters by type id internally).
- No `"Player"` / `"Health"` strings.
- If the schema gains a new property, regenerate the header; if a property is removed or renamed, the code fails to compile -- regression caught early.

The codegen emits:

| Per struct | What it gives you |
|---|---|
| `EntityType::X` enum | Strongly-typed id (`Player = 0`, `Projectile = 1`, ...) |
| `X::PropertyName` `constexpr const char*` | String constants matching the schema |
| `XView` class | Read-only wrapper (`GetHealth()`, `GetPosition()`, ...) |
| `XMutator` class | Read + write wrapper (`SetHealth(value)`, `GetMutableYyy()` for nested / arrays) |
| `ForEachX(bucket, accessor, fn)` | Filters a `BucketMutator` by entity_type_id, calls `fn(XMutator&)` |
| `ForEachXView(bucket, accessor, fn)` | Read-only iteration counterpart over `const Bucket&` |

Run the codegen:

```bash
python scripts/vtx_codegen.py path/to/schema.json path/to/generated.h <Namespace>
```

See `samples/arena_generated.h` for a complete example produced from `samples/content/writer/arena/arena_schema.json`.

## Patterns

### Cross-frame state

Processors are regular C++ objects. Cross-frame state lives on member variables -- no API ceremony needed:

```cpp
class KillStreakProcessor : public VTX::IFramePostProcessor {
public:
    void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext& ctx) override {
        // ... read current frame, compare to previous_*, update ...
        ++frames_seen_;
        last_frame_index_ = ctx.global_frame_index;
    }
    void Clear() override { frames_seen_ = 0; last_frame_index_ = -1; }

private:
    int     frames_seen_ = 0;
    int32_t last_frame_index_ = -1;
    std::unordered_map<std::string, KillStreak> streaks_;
};
```

`ctx.previous_frame` is always `nullptr` on the writer side -- the writer drops the native form after serialise. Cache what you need on the instance.

### Composition: chain of processors

When you have multiple independent transformations:

```cpp
auto chain = std::make_shared<VTX::FramePostProcessorChain>();
chain->Add(std::make_shared<HealthClampProcessor>());
chain->Add(std::make_shared<DeathConsistencyProcessor>());
chain->Add(std::make_shared<KillStreakProcessor>());
writer->SetPostProcessor(chain);
```

| Phase | Order |
|---|---|
| `Init`, `Process`, `PrintInfo` | Registration order (A, B, C) |
| `Clear` | Reverse order (C, B, A) -- destructor semantics |
| Last writer wins | If A and B both `Set` the same property, B's value persists |

### Replay-level metadata (KeyFrames, stats)

If your processor accumulates artefacts that don't belong on individual frames (e.g. a `std::vector<KeyFrame>` extracted from death events), expose them via your own getters and let the caller read after the run:

```cpp
class StatsProcessor : public VTX::IFramePostProcessor {
public:
    void Process(VTX::FrameMutationView&, const VTX::FramePostProcessContext& ctx) override {
        // ... accumulate into key_frames_ / order_stats_ / chaos_stats_ ...
    }
    const std::vector<KeyFrame>& GetKeyFrames() const { return key_frames_; }
    const GlobalStats&           GetOrderStats() const { return order_stats_; }
    const GlobalStats&           GetChaosStats() const { return chaos_stats_; }
private:
    std::vector<KeyFrame> key_frames_;
    GlobalStats order_stats_, chaos_stats_;
};

auto stats = std::make_shared<StatsProcessor>();
writer->SetPostProcessor(stats);
// ... record frames ...
writer->Stop();

// Caller reads after the run.  No SDK plumbing required.
for (const auto& kf : stats->GetKeyFrames()) { ... }
```

The processor is just an object you also hold a `shared_ptr` to.

### Schema-version branching

Read `ctx.schema_version` per frame and branch:

```cpp
void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext& ctx) override {
    if (ctx.schema_version == /*v13.8*/ 138) {
        ProcessV138(view);
    } else {
        ProcessLatest(view);
    }
}
```

`ctx.schema_version` is currently always `0` on the writer side (the version lives in the sink's header). Reader-side processors -- if added in a future release -- would see the actual version. Document any branching assumption explicitly.

### Structural mutation

`BucketMutator` exposes four operations beyond per-entity reads/writes:

```cpp
EntityMutator AddEntity();          // appends a default-constructed PropertyContainer; returns mutator
void          RemoveEntity(uint32_t entity_index);
template <class P> size_t RemoveIf(P pred);  // bulk filter -- pred is callable<bool, EntityView>
void          Clear();
```

Gotchas worth knowing:

- **`AddEntity` and `entity_type_id`**: a freshly-added `PropertyContainer` has `entity_type_id = -1` by default. The FlatBuffers serializer **silently drops** entities with `entity_type_id < 0`. Always set it explicitly:

  ```cpp
  auto ghost = bucket.AddEntity();
  ghost.raw()->entity_type_id = static_cast<int32_t>(MySchema::EntityType::Player);
  ghost.raw()->int32_properties.resize(3, 0);     // size to schema's expected slot count
  ghost.raw()->float_properties.resize(2, 0.0f);
  ghost.Set(health_key_, 100.0f);
  ```

- **`RemoveIf` invalidates `type_ranges`**: the bucket's typed-range index is wiped to zero after a bulk filter. The serializer rebuilds them from `entity_type_id`, so on-disk output is correct; in-memory tooling that reads `type_ranges` after the processor runs must recompute them or rely on `entity_type_id` directly.

- **Writer bucket limits**: the FlatBuffers serializer only persists `buckets[0]` (renamed to `"data"` on disk) and `buckets[1]` (renamed to `"bone_data"`). Additional buckets created by a processor via `view.raw()->CreateBucket(...)` are **silently dropped**. The Protobuf serializer preserves all buckets but applies the type-id reordering only to bucket 0.

## Error handling

The hook in `RecordFrame()` wraps `processor->Process()` in a try/catch:

- `std::exception` and unknown exceptions are caught and swallowed (the recording pipeline keeps running).
- Whatever the processor managed to mutate before throwing **stays mutated**.
- Out-of-range or invalid keys on `EntityMutator::Set` are silent no-ops -- matching the tolerance of `EntityView::Get`.

If `Init()` throws, the exception propagates to the caller and the half-initialized processor is **not** installed.

## Performance

When **no** processor is set, the hot path cost in `RecordFrame()` is one `shared_ptr` null-check + one untaken branch. Effectively zero overhead.

When a processor is set, the cost is dominated by the processor's own logic. The mutation view layer (`EntityMutator::Get` / `Set`) compiles to the same indexed access you'd write by hand against `PropertyContainer::*_properties` -- the wrappers are header-only and inlined.

Avoid resolving `PropertyKey<T>` per `Process()` call -- the lookup hashes by name. Resolve once in `Init` (or rely on the codegen-generated `Mutator` which caches in `static` locals).

## Out of scope

- **Re-serializing mutated bytes back to disk on the reader side.** Not this feature. Reader-side mutations would diverge from on-disk wire bytes -- documented contract.
- **Mutating raw byte spans.** `GetRawFrameBytes` returns on-disk truth by design.
- **Async processors.** `Process` is synchronous on the writer's calling thread. Long-running work blocks `RecordFrame`.
- **Replay-level metadata emission API.** Processors that produce per-replay artefacts (KeyFrames, stats) expose them via their own getters -- the SDK does not add a metadata channel.

## See also

- `samples/post_process_write.cpp` -- minimal end-to-end demo: register processor, record synthetic frames, re-open the `.vtx`, verify persisted values.
- `samples/advance_write.cpp` -- the `ArenaConsistencyProcessor` runs over all three data sources (JSON / Proto / FBS), proving the post-processor is orthogonal to source format.
- `samples/arena_generated.h` -- example codegen output (`PlayerMutator`, `ForEachPlayer`, etc.).
- `scripts/vtx_codegen.py` -- the codegen itself.
- `tests/writer/test_frame_post_processor.cpp` -- coverage including value mutation, chain ordering, structural mutation, lifecycle.
