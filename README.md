# VTX SDK

[![CI](https://github.com/ZenosInteractive/VTX/actions/workflows/build.yml/badge.svg)](https://github.com/ZenosInteractive/VTX/actions/workflows/build.yml)

A high-performance C++20 toolkit for recording, reading, comparing, and inspecting structured replay data. VTX serializes frame-based entity state into a compact, chunked binary format with support for **Protocol Buffers** and **FlatBuffers** backends.

## Performance at a glance

Measured on the CS2 (92 MB, 10 656 frames) and Rocket League (5 MB, ~21 k frames) real-world fixtures on a modern dev laptop. Full breakdown in [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

| Task | Number | Notes |
|---|---|---|
| Writing frames end-to-end | **~82 k frames/s** | 30-min 60 fps match ≈ 1.3 s of CPU. |
| Full sequential read (CS2, 92 MB) | **~5.6 s** (median) | Competitive with peer tooling. |
| Preview first 1 000 frames | **~1 s** | Below UX perceptibility. |
| `EntityView::Get` (isolated) | **1.70 ns** (585 M/s) | Theoretical ceiling. |
| `EntityView::Get` (realistic hot loop) | **~80 ns** (13 M/s) | What real integrations observe. |
| Diff consecutive frames | **4 µs** (267 k/s) | Instant from a user perspective. |

### Health check

| Area | Verdict | Why |
|---|---|---|
| Writer throughput | 🟢 Fast | ~10× headroom over real-time recording. |
| Full sequential read | 🟢 Fast enough | 5.6 s for a 92 MB replay (median). |
| Preview + seek-play | 🟢 Fast | Sub-second scrubbing. |
| Cache-window **well-sized** | 🟢 Fast | <4 s for 50 random jumps. |
| Cache-window **mis-sized** | 🔴 **Actively bad** | 59 % slower than no cache — needs docs guidance. |
| `FrameAccessor` / `PropertyKey` setup | 🟢 Negligible | 6.77 µs + 74 ns/key. Once per integration. |
| `EntityView` hot loop | 🟢 Excellent | 1.70 ns isolated / ~80 ns realistic. |
| Diff + short-circuit | 🟢 Instant | 4 µs consecutive; 10×/2× shortcut. |
| Schema parse | 🟢 Negligible | 200 µs, one-time. |
| Format choice (FBS vs Proto) | 🟡 Depends | No universal winner — fixture-shape dictates. |

**Honest caveats** (detail in [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md)):
- One machine, one run. Ratios hold across hardware; absolute numbers vary.
- No direct comparison to competitor libraries.
- `items_per_second` is CPU-time based; use wall time for user-observable claims.
- One known fixture counter inflated 2× (`BM_AccessorRandomWithinBucket`); flagged for fix.

## Architecture

```
vtx_common          Core types, schemas, serialization adapters, compression (zstd)
   |
   +--- vtx_writer  Record live frame data into .vtx replay files
   |
   +--- vtx_reader  Stream and random-access .vtx replay files with chunk-based caching
   |
   +--- vtx_differ  Compute structural diffs between frames or replay trees
```

All four modules are static libraries. Enable or disable each with CMake options.

## Requirements

| Requirement | Version |
|---|---|
| C++ Standard | C++20 |
| CMake | >= 3.15 |
| Compiler | MSVC / clang / gcc with C++20 support |
| Platform | Windows, Linux, macOS |

Preferred dependency flow:

- **Windows**: use the provided `vcpkg.json` manifest with `-DVTX_DEPENDENCY_SOURCE=PACKAGE_MANAGER`
- **Linux / macOS**: use system packages / package manager installs
- **Legacy fallback**: the prebuilt binaries under `thirdparty/` still work on Windows via `-DVTX_DEPENDENCY_SOURCE=BUNDLED`, but they are no longer the only supported path

Header-only dependencies remain bundled in `thirdparty/`:

- **nlohmann/json** -- schema parsing
- **xxHash** -- fast content hashing

## Quick Start

### Using the build script

```bat
build_sdk.bat
```

This configures, builds (Release), and installs to `dist/`.

### Using CMake directly

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist
```

### Using vcpkg on Windows

```bat
vcpkg install
cmake -S . -B build -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVTX_DEPENDENCY_SOURCE=PACKAGE_MANAGER
cmake --build build --config Release --parallel
```

### Using CMake Presets (VS 2022)

```bat
cmake --preset windows-release
cmake --build --preset windows-release
```

## CMake Options

| Option | Default | Description |
|---|---|---|
| `VTX_BUILD_WRITER` | `ON` | Build the vtx_writer module |
| `VTX_BUILD_READER` | `ON` | Build the vtx_reader module |
| `VTX_BUILD_DIFFER` | `ON` | Build the vtx_differ module |
| `BUILD_VTX_TOOL` | `ON` | Build the tool suite (requires reader + writer) |
| `BUILD_VTX_INSPECTOR` | `ON` | Build the GUI inspector (ImGui) |
| `BUILD_VTX_CLI` | `ON` | Build the headless CLI inspector |
| `BUILD_VTX_SCHEMA_CREATOR` | `ON` | Build the Schema Creator |
| `BUILD_VTX_SAMPLES` | `ON` | Build the sample programs |

To build only the SDK libraries (no tools):

```bat
cmake -S . -B build -A x64 -DBUILD_VTX_TOOL=OFF
```

## Usage Example

### Reading a replay

```cpp
#include "vtx/reader/core/vtx_reader_facade.h"

// Auto-detects Protobuf (VTXP) or FlatBuffers (VTXF) format from magic bytes
auto result = VTX::OpenReplayFile("replay.vtx");
if (!result) {
    std::cerr << "Failed to open: " << result.error << "\n";
    return 1;
}

auto& reader = result.reader;
int32_t total = reader->GetTotalFrames();
auto schema   = reader->GetContextualSchema();
auto cache    = reader->GetPropertyAddressCache();

// Random access to any frame
const VTX::Frame* frame = reader->GetFrameSync(0);
for (const auto& bucket : frame->GetBuckets()) {
    for (const auto& entity : bucket.entities) {
        // Access properties via PropertyAddressCache for O(1) lookup
    }
}
```

### Writing a replay

```cpp
#include "vtx/writer/core/vtx_writer_facade.h"

VTX::WriterFacadeConfig config;
config.output_filepath  = "output.vtx";
config.schema_json_path = "schema.json";
config.chunk_max_frames = 1000;
config.use_compression  = true;

auto writer = VTX::CreateWriterFacade(config, VTX::SerializationFormat::Flatbuffers);

// Record one frame per tick of your game loop.  RecordFrame is the only
// call inside the loop -- Flush and Stop run once after the loop ends.
while (simulation_is_running) {
    VTX::Frame frame = BuildFrameFromGameState();
    int64_t  game_time = GetCurrentGameTimeTicks();
    writer->RecordFrame(frame, game_time);
}

// Flush any buffered frames to disk, then finalise the file (writes
// the footer with the seek table + schema).  Both run exactly once.
writer->Flush();
writer->Stop();
```

## Tools

### VTX Inspector (GUI)

ImGui-based visual inspector for browsing replay files. Requires OpenGL.

```
dist/bin/vtx_inspector.exe
```

### VTX CLI (Headless)

JSON-based command-line inspector designed for scripting and AI agent consumption.

```
dist/bin/vtx_cli.exe --help
dist/bin/vtx_cli.exe --json-only
```

Supports: `open`, `close`, `frame`, `entities`, `entity`, `property`, `buckets`, `types`, `diff`, `track`, `search`, and more.


### Schema Creator

GUI tool for building and maintaining the `schema.json` file that the
writer consumes (via `WriterFacadeConfig::schema_json_path`).  A schema
declares the entity structs, their properties, property types, and the
bucket layout the recorder will group entities into.

```
dist/bin/vtx_schema_creator.exe
```

Typical flow:

1. **New Schema** -- creates an empty document with a default bucket list.
2. **Add a struct** (e.g. `Player`), then add properties to it -- for each
   property pick a `typeId` (`Int32`, `Float`, `Vector3`, `String`, ...),
   a display name, and a category.
3. **Map structs to buckets** in the Buckets window so the writer knows
   where each entity type is stored in a frame.
4. **Validate** -- the tool flags missing types, duplicate names,
   mismatched bucket mappings, etc.
5. **Save** -- produces a JSON file you point the writer at.
6. **Evolve** -- loading an existing schema captures it as the baseline.
   Changes are diffed against the baseline; saving "as next generation"
   bumps the version and refuses to proceed if the diff contains breaking
   changes (removed fields, incompatible type changes).

A concrete schema ready to drop into the writer is
[`samples/content/writer/arena/arena_schema.json`](samples/content/writer/arena/arena_schema.json) --
open it in Schema Creator to see the output format.

## Project Structure

```
sdk/
  include/vtx/         Public API headers (vtx/common/, vtx/reader/, vtx/writer/, vtx/differ/)
  src/
    schemas/           Protobuf and FlatBuffers schema definitions
    vtx_common/        Core library implementation
    vtx_reader/        Reader implementation
    vtx_writer/        Writer implementation
    vtx_differ/        Differ implementation
tools/
  cli/                 Headless CLI inspector (JSON protocol)
  inspector/           GUI inspector (ImGui + GLFW)
  schema_creator/      Schema definition tool
  shared/              Shared UI library used by inspector + schema_creator
tests/                 GoogleTest suite -- reader/writer/differ/common + fixtures
benchmarks/            google/benchmark suite (gated by -DVTX_BUILD_BENCHMARKS=ON)
samples/               Example programs + sample replay/schema content
docs/                  ARCHITECTURE, BUILD, PERFORMANCE, FILE_FORMAT, SAMPLES, SDK_API
scripts/               Code generation (vtx_codegen.py) and build-side helpers
cmake/                 CMake package config (VtxDependencies, install rules)
thirdparty/            Header-only deps + legacy Windows binary fallback
vcpkg.json             Optional manifest for reproducible Windows package-manager builds
build_sdk.bat/.sh      One-shot configure + build + install wrappers (Windows / Linux)
CHANGELOG.md           Release notes, grouped by version
SECURITY.md            Vulnerability reporting policy and supported versions
CONTRIBUTING.md        Contribution guide
LICENSE / NOTICE       Apache-2.0 licence + third-party attribution
THIRD_PARTY_LICENSES.md  Full text of bundled third-party licences
```

## License

VTX is licensed under the Apache License 2.0 — see [LICENSE](LICENSE).

Copyright 2026 Zenos Interactive.

VTX bundles or links against third-party components under MIT, BSD-2-Clause,
BSD-3-Clause, Apache-2.0, and zlib/libpng licenses.  See
[NOTICE](NOTICE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for
the complete attribution and license texts.
