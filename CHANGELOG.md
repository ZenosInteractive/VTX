# Changelog

All notable changes to the VTX SDK will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] - 2026-04-20

### Added

- **tests**: 47 new tests across 8 new files, driven by a targeted SDK audit.  Test suite total: 89 -> 187 passing + 1 intentionally skipped (awaiting a fixture schema with a Map field).
  - `tests/common/test_flat_array_edges.cpp` (12 tests) -- repeated OOB `PushBack`, empty-span `ReplaceSubArray` at non-zero indices, insert-at-end-equals-PushBack, erase-last-remaining-subarray, zero-length `EraseRange`, `CreateEmptySubArray` interactions, SSO-boundary operations on `FlatArray<std::string>`, `FlatBoolArray = FlatArray<uint8_t>` pin
  - `tests/reader/test_corrupt_files.cpp` (8 tests) -- empty file, file smaller than magic bytes, valid magic but truncated header, truncated before footer, corrupt `footer_size`, chunk offset beyond EOF, negative / out-of-range frame indices (A1 / A3 regression tests)
  - `tests/writer/test_writer_edges.cpp` (6 tests) -- zero-frame replay, single-frame replay, `chunk_max_frames = 1`, `RecordFrame` after `Stop`, double-`Stop` idempotency (A5 regression), frame larger than `chunk_max_bytes`
  - `tests/differ/test_diff_edges.cpp` (6 tests) -- empty buckets, differing bucket contents, byte arrays, nested `any_struct_properties`, map properties (skipped pending schema fixture), entity replaced under same unique_id
  - `tests/common/test_schema_registry_errors.cpp` (5 tests) -- empty / malformed / missing-required / duplicate-struct / unknown-typeId JSON inputs must not crash
  - `tests/common/test_vtx_game_times_state.cpp` (6 tests) -- rollback without prior snapshot, zero-frame resolve, setup-after-data documents contract, clear-then-reuse, snapshot+rollback roundtrip, `InsertLiveChunkTimes` monotonicity
  - `tests/common/test_content_hash_edges.cpp` (5 tests) -- NaN determinism, distinct NaN bit patterns, empty-vs-default equivalence, signed zero distinguishing, move stability
  - `tests/reader/test_open_replay_edges.cpp` (4 tests) -- directory path, missing file size_in_mb zeroing, relative paths resolving against cwd, non-ASCII filenames
- **samples/basic_diff.cpp**: `--fail-on-empty` flag used by the sample smoke test registered in `tests/CMakeLists.txt`.  Guards against regressions where the differ silently returns empty patches

### Build

- **cmake**: root `CMakeLists.txt` wires the new `tests/` directory through an `add_subdirectory(tests)` block guarded by `VTX_BUILD_TESTS` (default `ON`).  `sdk/src/vtx_common/CMakeLists.txt` exposes its generated-code directory as the cache variable `VTX_COMMON_GENERATED_DIR` so the test target can include the same protobuf / flatbuffers headers vtx_common compiles

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
