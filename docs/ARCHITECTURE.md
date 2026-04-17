# Architecture

## Module Dependency Graph

```
vtx_common          Core types, compression, schema, serialization adapters
   |
   +--- vtx_writer  Writes .vtx replay files (frame recording, chunking, flushing)
   |
   +--- vtx_reader  Reads .vtx replay files (chunk-based caching, random access)
   |
   +--- vtx_differ  Structural diff between frames (binary tree-diff engine)
```

All four modules are built as static libraries. Each can be toggled independently via CMake options (`VTX_BUILD_WRITER`, `VTX_BUILD_READER`, `VTX_BUILD_DIFFER`). `vtx_common` is always built as the shared foundation.

## Module Overview

### vtx_common

Provides the shared type system, serialization infrastructure, and utilities used by all other modules.

| Area | Key files | Purpose |
|---|---|---|
| Types | `vtx_types.h` | `Vector`, `Quat`, `Transform`, `FloatRange`, `FlatArray<T>`, `PropertyContainer`, `Bucket`, `Frame`, `FileHeader`, `FileFooter`, `VtxFormat` |
| SoA containers | `vtx_types.h` | `FlatArray<T>` — contiguous data + offset arrays for Structure of Arrays layout |
| Property cache | `vtx_property_cache.h` | `PropertyAddressCache` — O(1) indexed property lookup by type and slot |
| Schema | `schema_registry.h`, `game_schema_types.h` | Schema definitions parsed from JSON; maps property names to typed indices |
| Compression | `vtx_deserializer_service.h` | zstd-based chunk compression/decompression |
| Logger | `vtx_logger.h` | Thread-safe singleton logger with `VTX_INFO`, `VTX_WARN`, `VTX_ERROR`, `VTX_DEBUG` macros using `std::format` syntax |
| Hashing | `vtx_types_helpers.h` | xxHash64 content hashing for fast entity comparison |
| Generated code | `src/generated/` | Protobuf (`.pb.h/.cc`) and FlatBuffers (`_generated.h`) schemas, auto-generated from `schemas/` |

### vtx_writer

Records live frame data into `.vtx` replay files.

- **Facade**: `IVtxWriterFacade` — `RecordFrame()`, `Flush()`, `Stop()`
- **Factory**: `CreateFlatBuffersWriterFacade()`, `CreateProtobuffWriterFacade()`
- **Config**: `WriterFacadeConfig` — output path, chunk size, compression, schema JSON path
- **Policy**: Template-parameterized writer policies select FlatBuffers or Protobuf serialization at compile time
- **Data-source interface**: `IFrameDataSource` (`Initialize()` / `GetNextFrame()` / `GetExpectedTotalFrames()`) — streaming contract for adapters that convert third-party replay formats into VTX frames. See `samples/advance_write.cpp` for JSON / Protobuf / FlatBuffers implementations, and `tools/integrations/rl/rl15/rl15_data_source.h` for a production example.

### vtx_reader

Streams and random-accesses `.vtx` replay files with a chunk-based cache.

- **Facade**: `IVtxReaderFacade` — `GetFrame()`, `GetFrameSync()`, `GetRawFrameBytes()`, `GetSeekTable()`, etc.
- **Factory**: `OpenReplayFile(filepath)` — auto-detects format from magic bytes, creates reader, wires chunk-state events
- **Chunk state**: `ReaderChunkState` / `ReaderChunkSnapshot` — thread-safe tracker for loaded/loading/evicted chunks, automatically connected by `OpenReplayFile()`
- **Context**: `ReaderContext` — bundles reader, chunk state, format, file size, and error info
- **Policy**: `ReplayReader<TPolicy>` — template over `FlatBuffersReaderPolicy` or `ProtobufReaderPolicy`
- **Caching**: Sliding window cache with configurable backward/forward chunks. Async loading via `std::async` with `std::stop_token` cancellation

### vtx_differ

Computes structural diffs between two serialized frames.

- **Facade**: `IVtxDifferFacade` — `DiffRawFrames(span_a, span_b, options)`
- **Factory**: `CreateDifferFacade(VtxFormat)` — creates the correct wire-format adapter
- **Engine**: `DefaultTreeDiff<TNodeView>` — recursive binary tree-diff constrained by `CBinaryNodeView` concept
- **Adapters**: `FlatbufferViewAdapter`, `FProtobufViewAdapter` — zero-copy binary node views into serialized buffers
- **Output**: `PatchIndex` — list of `DiffIndexOp` operations (Add, Remove, Replace, ReplaceRange) with binary paths

## Design Patterns

### Facade Pattern

Each module exposes a single abstract interface (`IVtxReaderFacade`, `IVtxWriterFacade`, `IVtxDifferFacade`) with factory functions. Consumers never see the underlying serialization-specific implementations.

### Policy-Based Design

`ReplayReader<TPolicy>` and the writer use compile-time policies that select the Protobuf or FlatBuffers serialization backend. Each policy implements header/footer parsing, chunk deserialization, and frame extraction.

### Structure of Arrays (SoA)

`FlatArray<T>` stores all elements contiguously in a single `data` vector with an `offsets` vector delimiting sub-arrays. This avoids the overhead of `vector<vector<T>>` and improves cache locality for batch processing.

### Concept Constraints (C++20)

`CBinaryNodeView` constrains the differ's template parameter to ensure adapters implement the required binary node traversal interface. `IVtxReaderPolicy` constrains reader policy types.

## Threading Model

- **Reader chunk loading**: `std::async` with `std::stop_token`. Up to 3 concurrent chunk loads. Chunks are loaded in priority order (active chunk first, then the window around it).
- **Chunk eviction**: Automatic when the sliding window moves. Eviction fires `OnChunkEvicted` on the `ReaderChunkState`.
- **Logger**: Thread-safe singleton. Sinks are called under a lock; formatting happens before the lock.
- **ReaderChunkState**: Guarded by `std::mutex`. Snapshot reads are lock-free copies.

## Memory Management

- **Chunk cache**: `std::map<int32_t, CachedChunk>` keyed by chunk index. Each cached chunk owns its decompressed blob and holds `std::span` views into it (zero-copy frame access).
- **Raw frame bytes**: `GetRawFrameBytes()` returns a span into the cached chunk's decompressed buffer. Valid only while the chunk is resident.
- **Pinned frames**: The CLI tool supports `PinFrame()` which deep-copies a frame out of the cache so it survives chunk eviction.

## Namespace Map

| Namespace | Module | Contents |
|---|---|---|
| `VTX` | vtx_common, vtx_reader, vtx_writer | Core types, reader/writer facades, format detection, integration primitives (`JsonMapping<T>`, `ProtoBinding<T>`, `FlatBufferBinding<T>`, `IFrameDataSource`) |
| `VtxDiff` | vtx_differ | Diff engine, patch types, binary view adapters |
| `VtxDiff::Flatbuffers` | vtx_differ | FlatBuffers binary view adapter |
| `VtxDiff::Protobuf` | vtx_differ | Protobuf binary view adapter |
| `VtxServices` | tools (inspector) | UI-level presentation services |
| `VtxCli` | tools (CLI) | CLI session and command infrastructure |

## Samples and Integrations

The `samples/` directory illustrates the same architectural layers used by real integrations under `tools/integrations/`. See [SAMPLES.md](SAMPLES.md) for the full walkthrough.

| Sample | Patterns demonstrated |
|---|---|
| `basic_read` / `basic_write` / `basic_diff` | Facade APIs of the three SDK modules |
| `generate_replay` | Data-source producer (synthesising raw game telemetry) |
| `advance_write` | Three `IFrameDataSource` adapters + three mapping styles (`JsonMapping<T>`, `ProtoBinding<T>`, `FlatBufferBinding<T>`) driving the writer |
