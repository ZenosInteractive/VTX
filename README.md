# VTX SDK

[![CI](https://github.com/ZenosInteractive/VTX/actions/workflows/build.yml/badge.svg)](https://github.com/ZenosInteractive/VTX/actions/workflows/build.yml)

A high-performance C++20 toolkit for recording, reading, comparing, and inspecting structured replay data. VTX serializes frame-based entity state into a compact, chunked binary format with support for **Protocol Buffers** and **FlatBuffers** backends.

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

Dependency flow:

- **Windows**: `vcpkg.json` manifest + vcpkg toolchain file (see below).  `find_package(Protobuf)` via any other mechanism works too
- **Linux / macOS**: system package manager for Protobuf (`apt install libprotobuf-dev`, `brew install protobuf`, etc.)
- FlatBuffers + zstd are always pinned via CMake `FetchContent` -- no version drift between platforms, no apt / brew / vcpkg needed for them

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
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
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

// Record frames in a loop
writer->RecordFrame(frame, game_time);
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

Interactive tool for defining VTX schemas from game data structures.

```
dist/bin/vtx_schema_creator.exe
```

## Project Structure

```
sdk/
  include/vtx/         Public API headers (vtx/common/, vtx/reader/, vtx/writer/, vtx/differ/)
  src/                 Implementation
    schemas/           Protobuf and FlatBuffers schema definitions
    vtx_common/        Core library implementation
    vtx_reader/        Reader implementation
    vtx_writer/        Writer implementation
    vtx_differ/        Differ implementation
tools/
  cli/                 Headless CLI inspector (JSON protocol)
  inspector/           GUI inspector (ImGui + GLFW)
  schema_creator/      Schema definition tool
  shared/              Shared UI library
thirdparty/            Header-only deps (nlohmann/json, xxHash)
vcpkg.json             Optional manifest for reproducible Windows package-manager builds
scripts/               Code generation (vtx_codegen.py)
cmake/                 CMake package config for downstream projects
samples/               Example code and sample replay data
```

## License

See [LICENSE](LICENSE) for details.

Copyright 2026 Zenos Interactive. Licensed under the Apache License 2.0.
