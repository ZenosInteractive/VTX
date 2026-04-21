# Changelog

All notable changes to the VTX SDK will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] - 2026-04-21

### Changed

- **build**: `find_package(VTX)` is now self-contained.  `VTXConfig.cmake` calls `find_dependency(Protobuf)` so consumers automatically pull a matching protobuf runtime, and the zstd static library is installed alongside VTX (exported as `VTX::libzstd_static`) and hooked into `vtx_common` via `$<INSTALL_INTERFACE:...>`.  A downstream project can link `VTX::vtx_reader` with nothing more than `find_package(VTX REQUIRED)` + a reachable Protobuf (vcpkg / apt / brew)
- **build**: `thirdparty/zstd/` and `thirdparty/flatbuffers/` deleted from the repo -- both libraries now come exclusively from FetchContent (pinned `v1.5.6` and `v24.12.23` respectively).  `thirdparty/protobuf/` stays as the Windows CI / local-dev fast path; Protobuf from vcpkg / system package managers continues to work on any platform via the existing `VTX_DEPENDENCY_SOURCE=PACKAGE_MANAGER` / `AUTO` code path

### Notes

- An earlier revision of this entry also moved Windows CI onto vcpkg manifest mode and deleted the bundled `thirdparty/protobuf/`.  That change was reverted: vcpkg on GitHub's Windows runners builds Protobuf + abseil from source on every cache miss (~15-30 min per job), which made CI timeouts the common case.  `vcpkg.json` is still committed for contributors who prefer the package-manager flow locally, but CI sticks with the bundled-binary fast path

## [Unreleased] - 2026-04-20

### Added

- **ci**: dedicated `clang-format` job in `.github/workflows/build.yml` that runs `clang-format --dry-run --Werror` against every C++ file added or modified by a PR / push (vs base branch on PR, vs `HEAD~1` on push).  Pre-existing files are not checked -- about 90% of the codebase predates `.clang-format`, so a full-repo strict check would be permanently red.  This "diff gate" catches new formatting regressions without requiring a blame-destroying style sweep.  Uses `clang-format-15` pinned for reproducibility.  `docs/BUILD.md` documents the one-liner for running a full-tree sweep locally when the team is ready
- **ci**: `.github/workflows/build.yml` expanded from a single Windows build (no tests) into a six-job matrix: Windows and Linux, each in Release-static / Release-shared / Debug-static.  Every push and every pull request runs the full ctest suite plus the sample smoke test on all six configurations.  Concurrency group cancels superseded runs; failed jobs upload `build/Testing/` and sample `test_output/` artefacts for inspection.  GUI tools (vtx_inspector, vtx_schema_creator) are disabled in CI for speed -- the SDK, vtx_cli, and samples are fully covered
- **docs**: README gets a CI status badge; `docs/BUILD.md` gets a "Continuous Integration" section describing the matrix and failure-artefact workflow
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

## [Unreleased] - 2026-04-18

### Added

- **build**: cross-platform support for Linux and macOS.  Core SDK (vtx_common, vtx_writer, vtx_reader, vtx_differ), CLI tool (vtx_cli), all five sample programs, and the full test suite now build and run on Linux.  macOS builds the SDK + CLI; GUI tools (inspector, schema_creator) remain Windows-only until their INI-based settings persistence is ported to XDG
- **build**: `cmake/VtxDependencies.cmake` -- central dependency resolution module exposing `VTX::deps::protobuf`, `VTX::deps::flatbuffers`, `VTX::deps::zstd` imported targets plus `VTX_PROTOC_EXE` / `VTX_FLATC_EXE` cache variables and the `vtx_copy_runtime_deps()` helper.  On Windows, `VTX_DEPENDENCY_SOURCE` (`AUTO` / `PACKAGE_MANAGER` / `BUNDLED`) picks between the vcpkg manifest and the legacy `thirdparty/protobuf/` bundle; on Linux/macOS, Protobuf comes from the system package manager (with a CONFIG-then-MODULE fallback -- Ubuntu 22.04's `libprotobuf-dev` doesn't ship `ProtobufConfig.cmake`).  FlatBuffers + zstd are unconditionally fetched from pinned source (`FetchContent`: `v24.12.23` and `v1.5.6`) so the wire format version and compression library are identical on every platform
- **build**: `VTX_BUILD_SHARED` option (default `OFF`) -- when enabled the four SDK libraries build as shared libraries (`.dll` / `.so` / `.dylib`) instead of static.  The generated protobuf/flatbuffers sources live in an OBJECT library (`vtx_generated_code`) that each Windows DLL embeds, because `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` cannot re-export protobuf's `_default_instance_` data globals -- letting each SDK DLL compile its own copy makes each a self-contained linking unit.  Includes `RUNTIME DESTINATION bin` in the install export and a static-vs-shared comparison in `docs/BUILD.md`
- **build**: `vcpkg.json` manifest for Windows package-manager builds.  Currently lists only `protobuf`; FlatBuffers and zstd never need vcpkg because FetchContent covers them
- **build**: `build_sdk.sh` -- Linux/macOS counterpart to `build_sdk.bat`.  Honours `BUILD_TYPE`, `CLEAN`, `SKIP_TESTS`, `JOBS`, `INSTALL_PREFIX` env overrides.  Runs the full pipeline: configure -> build -> ctest -> install
- **build**: `clean.sh` -- Linux/macOS counterpart to `clean.bat`
- **docs/BUILD.md**: Linux / macOS dependency lists per distro (Ubuntu/Debian/Fedora/macOS), environment-variable-driven script usage, `VTX_DEPENDENCY_SOURCE` documentation, and platform-specific troubleshooting

### Changed

- **build**: root `CMakeLists.txt` is now platform-aware.  MSVC-specific settings (`CMAKE_MSVC_RUNTIME_LIBRARY`, `PROTOBUF_STATIC_LIBRARY`) are gated by `if(MSVC)` + `VTX_NEEDS_PROTOBUF_STATIC_DEFINE`.  Non-MSVC builds enable `CMAKE_POSITION_INDEPENDENT_CODE` and set `$ORIGIN/../lib` (Linux) / `@loader_path/../lib` (macOS) RPATH so installed binaries find bundled shared libraries
- **build**: per-target hardcoded `thirdparty/protobuf/include` / `thirdparty/flatbuffers/include` entries removed -- headers now reach every consumer transitively through `vtx_common`'s PUBLIC interface and `VTX::deps::*` imported targets
- **build**: generated protobuf / FlatBuffers C++ code now lives under `${CMAKE_CURRENT_BINARY_DIR}/generated` (build tree) instead of `sdk/src/vtx_common/src/generated` (source tree).  The old location caused cross-platform crashes when the source tree got shared between machines with different `flatc` versions -- sharing between Windows (`flatc 25.x`) and Linux (`flatc 2.x` from apt) produced headers that hardcoded `FLATBUFFERS_VERSION_MAJOR == 25` and static-assert-failed against libflatbuffers 2.x.  Consumers (`vtx_reader`, `vtx_writer`, `vtx_differ`, `samples`, `tests`, `tools/cli`, `tools/shared`) reference the `VTX_COMMON_GENERATED_DIR` INTERNAL cache variable instead of hardcoding the old path
- **build**: FlatBuffers is now consumed **header-only via FetchContent** (pinned `v24.12.23`) on every platform.  Previously Windows linked a static `flatbuffers.lib` from `thirdparty/flatbuffers/`, while Linux tried `find_package(flatbuffers)` against `libflatbuffers-dev` (v2.x on Ubuntu 22.04) -- a version mismatch that produced "Non-compatible flatbuffers version included" static-assert failures.  `flatc` is built from source as part of the CMake graph (`$<TARGET_FILE:flatc>`).  Removed `thirdparty/flatbuffers/bin/` and `thirdparty/flatbuffers/lib/` (dead code).  Ubuntu no longer needs `flatbuffers-compiler` or `libflatbuffers-dev` apt packages
- **build**: zstd is consumed via **FetchContent** (pinned `v1.5.6`, `facebook/zstd`), built as a static library and linked into the SDK modules.  No runtime DLL / `.so` dependency on any platform; no `libzstd-dev` / Homebrew `zstd` requirement.  `thirdparty/zstd/` stays in the repo for now as an unused fallback that a later cleanup commit will delete
- **build**: `target_link_libraries(vtx_common ... $<BUILD_INTERFACE:VTX::deps::...>)` keeps bundled thirdparty imports out of the install export, so `find_package(VTX)` consumers are expected to bring their own protobuf (documented in `docs/BUILD.md`)
- **build**: per-target zstd DLL copy commands replaced with the central `vtx_copy_runtime_deps(target)` helper.  With zstd now statically linked from FetchContent, the helper is effectively a no-op stub -- kept at all call sites so reintroducing a runtime dep is a one-line change in `VtxDependencies.cmake`
- **tools**: `BUILD_VTX_INSPECTOR` and `BUILD_VTX_SCHEMA_CREATOR` default to `OFF` on Linux/macOS (Windows-only glue around INI settings persistence and dialog integration).  `BUILD_VTX_CLI` stays `ON` everywhere
- **tools**: `tools/CMakeLists.txt` only fetches ImGui + GLFW (and only adds `tools/shared`) when at least one GUI tool is enabled.  Headless Linux builds no longer pull in X11 as a build requirement -- `docs/BUILD.md` lists the X11 packages needed if you opt the GUI tools back in
- **tools/shared**: platform link libraries -- `opengl32` on Windows, `GL/dl/pthread` on Linux, `-framework OpenGL` on macOS.  `NOMINMAX` / `WIN32_LEAN_AND_MEAN` now gated to Windows
- **tools/shared/src/gui/gui_app.cpp**: previously-unguarded `#define GLFW_EXPOSE_NATIVE_WIN32` + `#include <GLFW/glfw3native.h>` wrapped in `#if defined(_WIN32)` so the file compiles on Linux
- **ci**: `.github/workflows/build.yml` Linux jobs (Release static, Release shared, Debug static) are now active -- they were scaffolded but commented out in the previous push because the VTX sources didn't yet build on Linux.  Workflow apt install now covers just `cmake g++ ninja-build protobuf-compiler libprotobuf-dev` (FlatBuffers + zstd via FetchContent)
- **README.md**: requirements row lists Windows + Linux + macOS; adds a "Using vcpkg on Windows" quick-start block; `thirdparty/` described as "Header-only deps + legacy Windows binary fallback"

## [Unreleased] - 2026-04-17

### Added

- **samples**: `vtx_sample_generate` target -- simulates a 5v5 arena match (3600 frames @ 60 FPS) and exports three data-source files (`arena_replay_data.{json,proto.bin,fbs.bin}`) representing raw game telemetry
- **samples**: `vtx_sample_advance_write` target -- demonstrates the full data-source pipeline with three `IFrameDataSource` implementations (JSON / Protobuf / FlatBuffers) driving the writer through SDK-native mapping primitives
- **samples**: `arena_mappings.h` (JSON data model + `VTX::JsonMapping<T>` specialisations), `arena_generated.h` (autogenerated schema-field constants + typed views), `schemas/arena_data.proto` + `schemas/arena_data.fbs` (arena game-side schemas in `arena_pb::` / `arena_fb::`)
- **samples**: CMake codegen rules for `protoc` + `flatc --gen-object-api`, wired into `samples/CMakeLists.txt` so schema edits rebuild automatically
- **docs**: new `docs/SAMPLES.md` -- per-sample walkthrough, folder layout, mapping-strategy comparison, codegen explanation
- **docs/SDK_API.md**: new "Integration Primitives" section documenting `IFrameDataSource`, `JsonMapping<T>`, `ProtoBinding<T>`, `FlatBufferBinding<T>`

### Changed

- **samples/basic_read.cpp**, **samples/basic_diff.cpp**: default replay path updated to `content/reader/arena/arena_from_fbs_ds.vtx`
- **samples/basic_diff.cpp**: rewritten to exercise `VtxDiff::IVtxDifferFacade::DiffRawFrames` instead of manually hash-comparing entities (the target named "diff" previously never touched the differ module it links against)
- **samples/generate_replay.cpp**: removed the `+30s` UTC workaround now that the underlying SDK bug is fixed; replaced with a fixed historical timestamp (2025-04-19) for reproducible output
- **content/ layout**: arena schema + three data sources live under `content/writer/arena/`; generated `.vtx` replays land under `content/reader/arena/`
- **sdk/include/vtx/differ/core/vtx_default_tree_diff.h**: standardised xxhash include to `<xxh3.h>` -- now consistent with `vtx_types_helpers.h`
- **docs/ARCHITECTURE.md**, **docs/BUILD.md**: `IFrameDataSource` documented in the writer section; build outputs list all five sample executables; cross-reference to `SAMPLES.md`

### Fixed

- **vtx_common** (`vtx_types.h`): `VTXGameTimes::AddTimeRegistry` no longer rejects valid historical UTC timestamps -- the regression check now correctly requires a prior frame to exist before flagging a repeat
- **vtx_common** (`vtx_types.h`): `VTXGameTimes()` constructor no longer seeds `start_utc_` with `GetUtcNowTicks()`; field starts at 0 and is populated when real data arrives.  `Clear()` updated for consistency
- **vtx_common** (`vtx_types.h`): `OnlyIncreasing` / `OnlyDecreasing` game-time filters no longer reject the very first frame of a replay whose `game_time` is 0
- **vtx_common** (`vtx_types.h`): three warning messages in `AddTimeRegistry` used printf specifiers (`%lld`, `%f`) inside `std::format` calls -- the values were never printed.  Converted to `{}` placeholders
- **vtx_reader** (`vtx_reader_facade.h`): swapped member declaration order in `ReaderContext` so `reader` is destroyed before `chunk_state`.  The reader's async chunk-load callbacks capture a raw pointer into `chunk_state`; the previous order created a potential use-after-free during context teardown
- **vtx_reader** (`vtx_reader.h`): `PerformHeavyLoading` no longer silently swallows deserialization exceptions -- logs the exception message before returning an empty chunk
- **sdk/src/schemas/vtx_schema.proto**: removed six stray `// Ale` author comments from the public Protobuf schema

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
