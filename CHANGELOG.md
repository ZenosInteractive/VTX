# Changelog

All notable changes to the VTX SDK will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] - 2026-04-20

### Fixed

5 new correctness bugs surfaced by a targeted SDK audit:

- **A1 -- `vtx_reader.h` `ReadFooter()`**: `footer_size` was read from the stream but used without checking `stream.gcount()` or `stream.fail()`.  On a truncated file the uninitialised value drove a seek to garbage and crashed the reader (SEH access violation in tests).  Now validates stream reads, checks file size, and rejects implausibly large footer sizes
- **A1 extended -- `vtx_reader.h` `ReadHeader()`**: same hardening applied (bounds check on declared header size against remaining file)
- **A2 -- `vtx_reader.h` `PerformHeavyLoading()`**: `stream.read()` can produce partial reads; the previous code only checked `raw_buffer.size() <= 4` which doesn't reflect actual bytes read.  Now validates via `stream.gcount()` and logs the specific shortfall
- **A3 -- `vtx_reader.h` `PerformHeavyLoading()`**: corrupt seek tables with `file_offset + chunk_size_bytes > file_size` previously seeked past EOF and crashed the deserialiser.  Now validates the chunk extent against the actual file size before seeking
- **A4 -- `vtx_reader.h` `SetEvents()` + callback call-sites**: `events_` was read (via `events_.OnChunkLoadFinished` etc.) without synchronisation while `SetEvents()` could overwrite it from another thread.  Reading a `std::function` while another thread writes is UB.  Introduced `events_mutex_` + a `GetEventsSnapshot()` helper used at every callback site; the actual callback invocations happen on the local snapshot outside the lock
- **A5 -- `vtx_writer_facade.cpp` `WriterFacadeImpl`**: `RecordFrame`, `Flush`, and subsequent `Stop` calls after the initial `Stop()` could overwrite the already-finalised file and truncate previously-recorded frames.  `WriterFacadeImpl` now tracks a `stopped_` flag and silently no-ops all three methods after `Stop()`.  `Stop()` itself is idempotent

## [0.0.1] - 2026-04-16

### Added

- **vtx_common**: Core type system with SoA-based `PropertyContainer`, `Frame`, `Bucket`, and `Transform` types
- **vtx_common**: Dual serialization backend support (Protocol Buffers and FlatBuffers)
- **vtx_common**: Schema registry with JSON-based schema definitions and `PropertyAddressCache` for O(1) property lookup
- **vtx_common**: zstd compression for chunks and headers
- **vtx_common**: xxHash-based content hashing for fast frame comparison
- **vtx_common**: Thread-safe logger with configurable sinks (`VTX_INFO`, `VTX_WARN`, `VTX_ERROR`, `VTX_DEBUG`)
- **vtx_writer**: `IVtxWriterFacade` for recording frame data into chunked `.vtx` files
- **vtx_writer**: `ChunkedFileSink` with configurable chunk size, compression, and seek table generation
- **vtx_writer**: Protobuf and FlatBuffers serialization policies
- **vtx_reader**: `IVtxReaderFacade` with random-access and streaming frame access
- **vtx_reader**: Async chunk-based caching with configurable cache window
- **vtx_reader**: `FrameAccessor` for type-safe, O(1) property access via `PropertyKey<T>`
- **vtx_reader**: Seek table for O(1) chunk lookup by frame index
- **vtx_differ**: `DefaultTreeDiff<TNodeView>` for structural comparison of replay trees
- **vtx_differ**: `PatchIndex` and `DiffIndexOp` for tracking add/remove/modify operations
- **vtx_differ**: Configurable float epsilon for approximate comparisons
- **vtx_differ**: Protobuf and FlatBuffers view adapters
- **Tools**: VTX Inspector -- ImGui-based GUI for browsing replay files
- **Tools**: VTX CLI -- Headless JSON inspector for scripting and AI agents
- **Tools**: Schema Creator -- Interactive schema definition tool
- **Build**: CMake build system with modular options and CMake Presets support
- **Build**: `build_sdk.bat` one-click build script for Windows
