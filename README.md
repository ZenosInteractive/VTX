# VTX SDK

[![CI](https://github.com/ZenosInteractive/VTX/actions/workflows/build.yml/badge.svg)](https://github.com/ZenosInteractive/VTX/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue?logo=apache&logoColor=white)](LICENSE)
[![Release](https://img.shields.io/github/v/release/ZenosInteractive/VTX?logo=github&label=Release)](https://github.com/ZenosInteractive/VTX/releases)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](#requirements)
[![Docs](https://img.shields.io/badge/Docs-Wiki-blue?logo=gitbook&logoColor=white)](https://github.com/ZenosInteractive/VTX/wiki)
[![Website](https://img.shields.io/badge/Website-zenosinteractive.com-B870FB?logoColor=white)](https://www.zenosinteractive.com/)

VTX is an open binary format for real-time per-frame state data, plus a C++20 SDK that reads, writes, and inspects it. A capture holds the complete state of what was happening on every frame (entities, transforms, skeletal bones, events, statistics), carries its own schema, and supports random access. Whatever produces the data (your game, your engine, a third-party capture tool) writes it into VTX; anything that can read a binary file reads it back.

- **Self-describing files.** Every capture embeds its own schema. A reader today decodes a file recorded years ago without external context.
- **Dual-schema access.** Read as generic (portable across titles) or contextual (game-aware) from the same file.
- **Random access by frame or timestamp.** Footer-indexed seek, not a linear scan. O(log n) lookup plus one chunk decompress.
- **Both Protobuf and FlatBuffers.** SDK supports both backends out of the box. The file announces which in its magic bytes; readers auto-detect.
- **Engine-independent C++20.** No engine dependency. Language bindings wherever Protobuf or FlatBuffers exist (Python, Go, Rust, Java, JS).
- **Open.** Apache-2.0. Spec, reference reader, and tooling all in the repo.

## What people build with VTX

Broadcast replay tooling, esports analytics and overlays, coaching platforms, second-screen fan apps, AI training pipelines for motion and decision models, 3D Game Twins. Anywhere you need frame-accurate, structured, engine-independent state data, VTX is designed to be the thing you build on top of. Full breakdown in the [wiki Use Cases](https://github.com/ZenosInteractive/VTX/wiki/Use-Cases).

## Quick start

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist
```

Other build paths (vcpkg, CMake presets, the `build_sdk.bat` one-shot wrapper) in [Getting Started](https://github.com/ZenosInteractive/VTX/wiki/Getting-Started).

### Write a replay

```cpp
#include "vtx/writer/core/vtx_writer_facade.h"

VTX::WriterFacadeConfig config;
config.output_filepath  = "output.vtx";
config.schema_json_path = "schema.json";
config.chunk_max_frames = 1000;
config.use_compression  = true;

auto writer = VTX::CreateFlatBuffersWriterFacade(config);

while (simulation_is_running) {
    VTX::Frame frame = BuildFrameFromGameState();
    VTX::GameTime::GameTimeRegister game_time = GetCurrentGameTime();
    writer->RecordFrame(frame, game_time);
}

writer->Flush();
writer->Stop();
```

### Read a replay

```cpp
#include "vtx/reader/core/vtx_reader_facade.h"

VTX::ReaderContext ctx = VTX::OpenReplayFile("replay.vtx");
if (!ctx) {
    std::cerr << "Failed to open: " << ctx.GetError() << "\n";
    return 1;
}

int32_t total = ctx->GetTotalFrames();
const VTX::Frame* frame = ctx->GetFrameSync(0);
for (const auto& bucket : frame->GetBuckets()) {
    for (const auto& entity : bucket.entities) {
        // Access properties via PropertyAddressCache for O(1) lookup
    }
}
```

`ReaderContext` closes the file on destruction. Frame pointers are valid until the next reader call that could evict the underlying chunk. See [Runtime Contracts](https://github.com/ZenosInteractive/VTX/wiki/Runtime-Contracts) before wiring the reader into a game loop.

## Performance at a glance

Measured on the CS2 (92 MB, 10 656 frames) and Rocket League (5 MB, ~21 k frames) fixtures on a modern dev laptop.

| Task | Number | Notes |
|---|---|---|
| Writing frames end-to-end | ~82 k frames/s | 30-min 60 fps match ≈ 1.3 s of CPU. |
| Full sequential read (CS2, 92 MB) | ~5.6 s (median) | ~16 MB/s decoded. |
| Preview first 1 000 frames | ~1 s | Fast feedback for preview UI. |
| `EntityView::Get` (realistic hot loop) | ~80 ns (13 M/s) | What real integrations observe. |
| Diff consecutive frames | 4 µs (267 k/s) | Instant in a playback loop. |

The reader's chunk cache is the single sharpest performance edge in the SDK. A well-sized cache speeds random access dramatically; a mis-sized cache can be up to 59% slower than no cache at all. Full methodology, caveats, and sizing guidance in [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) and the [wiki Performance page](https://github.com/ZenosInteractive/VTX/wiki/Performance).

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

Four static libraries. Enable or disable each with a CMake option. Full module breakdown and extension model in [SDK Architecture](https://github.com/ZenosInteractive/VTX/wiki/SDK-Architecture).

## Tools

- **`vtx_inspector`** — ImGui-based visual inspector for browsing replay files. Requires OpenGL.
- **`vtx_cli`** — Headless, JSON-based command-line inspector. Designed for scripting and AI-agent consumption.
- **`vtx_schema_creator`** — GUI tool for authoring the `schema.json` the writer consumes.

Details, flags, and walkthroughs in the [wiki Tools page](https://github.com/ZenosInteractive/VTX/wiki/Tools).

## Requirements

| Requirement | Version |
|---|---|
| C++ Standard | C++20 |
| CMake | >= 3.15 |
| Compiler | MSVC, clang, or gcc with C++20 support |
| Platform | Windows, Linux |

Dependencies are pulled via `vcpkg.json` on Windows or system packages on Linux. Header-only deps (`nlohmann/json`, `xxHash`) stay bundled in `thirdparty/`.

## Documentation

- **[Home](https://github.com/ZenosInteractive/VTX/wiki)** — wiki landing page and navigation.
- **[Getting Started](https://github.com/ZenosInteractive/VTX/wiki/Getting-Started)** — every build path, full write and read examples.
- **[Concepts](https://github.com/ZenosInteractive/VTX/wiki/Concepts)** — data model, dual-schema, generic property container.
- **[File Format](https://github.com/ZenosInteractive/VTX/wiki/File-Format)** — on-disk layout, serialisation backends, compression.
- **[SDK Architecture](https://github.com/ZenosInteractive/VTX/wiki/SDK-Architecture)** — module layout and extension system.
- **[Runtime Contracts](https://github.com/ZenosInteractive/VTX/wiki/Runtime-Contracts)** — thread safety, ownership, error handling, limits.
- **[Stability and Versioning](https://github.com/ZenosInteractive/VTX/wiki/Stability)** — API, format, and schema compatibility policy.
- **[Performance](https://github.com/ZenosInteractive/VTX/wiki/Performance)** — full numbers, methodology, and sizing guidance.
- **[Use Cases](https://github.com/ZenosInteractive/VTX/wiki/Use-Cases)** — what people build on top of VTX.

In-tree reference: [`docs/`](docs/) includes `ARCHITECTURE.md`, `BUILD.md`, `FILE_FORMAT.md`, `PERFORMANCE.md`, `SAMPLES.md`, `SDK_API.md`.

## License

VTX is licensed under the Apache License 2.0. See [LICENSE](LICENSE).

Copyright 2026 Zenos Interactive.

VTX bundles or links against third-party components under MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, and zlib/libpng licenses. See [NOTICE](NOTICE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for full attribution and license texts.
