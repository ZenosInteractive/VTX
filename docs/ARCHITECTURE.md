# Architecture

## Module Dependency Graph

```
vtx_common             Core types, compression, on-disk framing, schema, serialization adapters
   |
   +--- vtx_writer     Writes .vtx replay files (frame recording, chunking, flushing)
   |
   +--- vtx_reader     Reads .vtx replay files (chunk-based caching, random access)
   |
   +--- vtx_differ     Structural diff between frames (binary tree-diff engine)
   |
   +--- vtx_transform  Modifies existing replays (repair a crashed one, cut a frame range, filter entities)
                       -- the one module that links both vtx_reader and vtx_writer
```

All five modules are built as static libraries. Each can be toggled via CMake options (`VTX_BUILD_WRITER`, `VTX_BUILD_READER`, `VTX_BUILD_DIFFER`, `VTX_BUILD_TRANSFORM`); writer, reader and differ are independent siblings on `vtx_common`, while `vtx_transform` requires reader and writer. `vtx_common` is always built as the shared foundation.

## Module Overview

### vtx_common

Provides the shared type system, serialization infrastructure, and utilities used by all other modules.

| Area | Key files | Purpose |
|---|---|---|
| Types | `vtx_types.h` | `Vector`, `Quat`, `Transform`, `FloatRange`, `FlatArray<T>`, `PropertyContainer`, `Bucket`, `Frame`, `FileHeader`, `FileFooter`, `VtxFormat` |
| SoA containers | `vtx_types.h` | `FlatArray<T>` — contiguous data + offset arrays for Structure of Arrays layout |
| Property cache | `vtx_property_cache.h` | `PropertyAddressCache` — O(1) indexed property lookup by type and slot |
| Schema | `schema_registry.h`, `game_schema_types.h` | Schema definitions parsed from JSON; maps property names to typed indices; pre-sizes `PropertyContainer` arrays to per-type max on load |
| Schema validation | `schema_validator.h`, `schema_validation_result.h` | Rule-based pre-resolution validation (`SchemaValidator` / `SchemaIssue`); malformed schemas rejected at load. `SchemaValidationResult::ToReport()` bridges to the diagnostics model |
| Diagnostics | `vtx_diagnostics.h` | One SDK-wide error model: `Severity`, `VtxErrorCode`, `VtxDiagnostic` (`VtxError`/`VtxWarning`), `VtxResult<T>`, `ValidationReport` |
| Validation | `vtx_validation.h`, `vtx_frame_accessor.h` | `ValidateSchema` / `ValidateEntity` / `ValidateFrame`; strict accessors `FrameAccessor::TryResolve`, `EntityView::TryGet`, `EntityMutator::TrySet` |
| Compression | `vtx_deserializer_service.h` | zstd-based chunk compression/decompression |
| On-disk framing | `vtx_replay_framing.h` | Single definition of the `.vtx` block layout: size prefixes, footer trailer, the zstd-if-beneficial rule, xxHash64 chunk checksums, and a layout probe for rewriters. Shared by both sinks and the `vtx_transform` tools (`RepairReplayFile`, `CutReplayFile`, `FilterReplayFile`), so their output is byte-identical by construction |
| Logger | `vtx_logger.h` | Thread-safe singleton logger with `VTX_INFO`, `VTX_WARN`, `VTX_ERROR`, `VTX_DEBUG` macros using `std::format` syntax |
| Hashing | `vtx_types_helpers.h` | xxHash64 content hashing for fast entity comparison |
| Generated code | `src/generated/` | Protobuf (`.pb.h/.cc`) and FlatBuffers (`_generated.h`) schemas, auto-generated from `schemas/` |

### vtx_writer

Records live frame data into `.vtx` replay files (or streams the same bytes over a socket).

- **Facade**: `IVtxWriterFacade` — `RecordFrame()`, `TryRecordFrame()` (returns a `RecordResult`), `Flush()`, `Stop()`, `SetPostProcessor()`
- **Finalization & strict recording**: each frame is validated (entity types resolve to schema structs), content-hashed (after schema fields + post-processor overrides), and **frozen** (stashed mutation handles revoked) before it enters the chunk pipeline. `TryRecordFrame()` makes rejection observable via `RecordResult` (carries a `VtxError`); the writer assigns a monotonic frame index; an opt-in `retain_finalized_snapshot` keeps the last finalized frame queryable via `GetLastFinalizedFrame()` / `FindEntity()`. `RecordPipeline::Run()` returns a `PipelineReport`
- **Factory**: `CreateFlatBuffersWriterFacade()` / `CreateProtobufWriterFacade()` (file output); `CreateFlatBuffersNetworkWriterFacade()` / `CreateProtobufNetworkWriterFacade()` (TCP socket output) — same `IVtxWriterFacade` behind both, so user code is sink-agnostic
- **One-call pipeline**: `WriteReplay(config, source, format)` (`vtx_write_replay.h`) creates the writer, drains an `IFrameDataSource`, finalizes, and returns a `WriteReplayResult` (frames written / dropped, one `VtxWarning` per dropped frame, elapsed time)
- **Config**: `WriterFacadeConfig` (file path) or `NetworkWriterFacadeConfig` (host + port) — chunk size, compression, and the schema supplied as a JSON path, an in-memory JSON string (`schema_json_content`), or a pre-built `SchemaRegistry` (`schema_registry`); `create_output_dirs` (default on) auto-creates missing parent directories of the output file
- **Policy**: Template-parameterized writer policies select FlatBuffers or Protobuf serialization at compile time
- **Sinks**: `ChunkedFileSink<Policy>` (file), `ChunkedNetworkSink<Policy>` (TCP stream) — both produce the **same `.vtx` byte sequence**, so a socket receiver only has to concatenate incoming bytes into a file to get a valid replay
- **Crash durability & recovery** (file sink only): `DurableFile` (FILE* wrapper with real `fsync`; `durable_writes` on by default) makes every chunk power-loss durable, each chunk carries an xxHash64 checksum in the seek table, and `RecoveryJournal` maintains an append-only `.vtx.recovery` write-ahead sidecar (chunk commits, exact per-frame times, in-flight frames) that is deleted on clean `Stop()`. Reconstructing a crashed recording from that sidecar is `RepairReplayFile()` in `vtx_transform` (below) — the writer only produces new recordings. `IFileSinkPerfObserver` (`Config::perf_observer` / `WriterFacadeConfig::perf_observer`) exposes per-stage sink timings (serialize / compress / disk write incl. the durability flush)
- **Data-source interface**: `IFrameDataSource` (`Initialize()` / `GetNextFrame()` / `GetExpectedTotalFrames()`). Concrete implementations: `samples/advance_write.cpp` (JSON / Protobuf / FlatBuffers from disk); `PipeFrameDataSource<Adapter>` (stdin, Windows named pipes, POSIX FIFOs — including a **server mode** that creates the pipe and waits, for independent game-injector-style external producers); `WebSocketFrameDataSource<Adapter>` (`ws://` + `wss://` with TLS). Both streaming sources share the `IFramePayloadAdapter` concept, so a single adapter plugs into either transport. A `SharedMemoryFrameDataSource` (zero-copy SPSC ring, pluggable `ISharedMemoryTransport`) is present in-tree but **experimental / WIP and not yet functional** — landed unintentionally, not a supported input path
- **Dependencies**: protobuf + flatbuffers (serialization), zstd (chunk compression), xxHash/xxh3 (chunk + journal-record checksums), IXWebSocket + mbedTLS (WebSocket transport; hidden behind a PIMPL boundary in `websocket_client.cpp` — never leak into the public SDK headers)

### vtx_reader

Streams and random-accesses `.vtx` replay files with a chunk-based cache.

- **Facade**: `IVtxReaderFacade` — `GetFrame()`, `GetFrameSync()`, `GetRawFrameBytes()`, `GetSeekTable()`, etc.
- **Factory**: `OpenReplayFile(filepath)` — auto-detects format from magic bytes, creates reader, wires chunk-state events
- **Chunk state**: `ReaderChunkState` / `ReaderChunkSnapshot` — thread-safe tracker for loaded/loading/evicted chunks, automatically connected by `OpenReplayFile()`
- **Context**: `ReaderContext` — bundles reader, chunk state, format, file size, and a structured `VtxError` (`GetError()`)
- **Policy**: `ReplayReader<TPolicy>` — template over `FlatBuffersReaderPolicy` or `ProtobufReaderPolicy`
- **Caching**: Sliding window cache with configurable backward/forward chunks. Async loading via `std::async` with `std::stop_token` cancellation
- **Ready-state**: `IsReady()` / `IsReadyFailed()` / `GetReadyError()` (a `VtxError`) / `WaitUntilReady()`; `ReplayReaderEvents::OnReadyFailed(const VtxError&)` for the async chunk-0 warm
- **Validation**: `ValidateReplay(reader)` (already-open) / `ValidateReplayFile(path)` validate a replay's embedded schema + every frame into a `ValidationReport` (`vtx_replay_validation.h`)

### vtx_differ

Computes structural diffs between two serialized frames.

- **Facade**: `IVtxDifferFacade` — `DiffRawFrames(span_a, span_b, options)`
- **Factory**: `CreateDifferFacade(VtxFormat)` — creates the correct wire-format adapter
- **Engine**: `DefaultTreeDiff<TNodeView>` — recursive binary tree-diff constrained by `CBinaryNodeView` concept
- **Adapters**: `FlatbufferViewAdapter`, `FProtobufViewAdapter` — zero-copy binary node views into serialized buffers
- **Output**: `PatchIndex` — list of `DiffIndexOp` operations (Add, Remove, Replace, ReplaceRange) with binary paths

### vtx_transform

Every tool that modifies an existing `.vtx` lives here — the writer only produces new recordings, the reader only consumes them. It sits above `vtx_reader` and `vtx_writer`: cut and filter read the source through the reader facade and re-serialize chunks through the writer's formatter policies, repair rebuilds a crashed file from its journal through the writer's `RecoveryJournal`, and every byte is framed via `vtx_replay_framing.h` — which is what lets reader and writer stay independent siblings. Requires both (`VTX_BUILD_TRANSFORM`, on by default; skipped automatically when either is off).

- **Repair** (`vtx_replay_recovery.h`): `RepairReplayFile` reconstructs a recording that died before `Stop()` from its `.vtx.recovery` sidecar — drops a torn tail chunk, verifies each surviving chunk's checksum, re-appends the in-flight frames and synthesizes the footer with the exact per-frame times (at a chunk boundary the result is byte-identical to a clean `Stop()`). `ReplayNeedsRecovery` / `RecoveryJournalPath` are the user-driven detection helpers — never automatic on open. Needs no reader: it works from the journal and the raw bytes
- **Cut** (`vtx_replay_cut.h`): `PlanCutFrames` / `PlanCutChunks` resolve a frame or chunk range against a footer (cheap, for previews); `CutReplayFile` executes the plan — header copied verbatim, whole chunks copied verbatim with rebased seek entries, edge chunks re-serialized with only the kept frames, footer rebuilt with frames renumbered from 0 and the time table sliced (tick values stay absolute). Both backends; the output keeps the source format
- **Entity filter** (`vtx_replay_filter.h`): `FilterReplayFile` rewrites every chunk with entities dropped (blacklist) or exclusively kept (whitelist) by bucket, unique-id glob, struct-name glob or scalar property value. Every frame survives; header, frame count and time table are preserved and only the seek table changes. `ReplayFilterMatcher` / `GlobMatch` expose the rule engine for in-memory previews
- These are the SDK entry points behind the inspector's **File > Repair Replay**, **File > Cut Replay** and **File > Filter Entities** (`vtx_inspector` links `vtx_reader` + `vtx_transform`, not `vtx_writer`)

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
| `VTX` | vtx_common, vtx_reader, vtx_writer, vtx_transform | Core types, reader/writer facades, format detection, replay tools (`RepairReplayFile`, `CutReplayFile`, `FilterReplayFile`), integration primitives (`JsonMapping<T>`, `ProtoBinding<T>`, `FlatBufferBinding<T>`, `IFrameDataSource`) |
| `VTX::Framing` | vtx_common | The `.vtx` on-disk framing helpers (`vtx_replay_framing.h`) |
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
