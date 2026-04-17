# Build Guide

> Looking for runnable example programs? See [SAMPLES.md](SAMPLES.md) -- five
> targets shipped under `samples/`, from a 30-line reader demo to a full
> arena-data pipeline using `IFrameDataSource`.

## Requirements

| Requirement | Version |
|---|---|
| C++ Standard | C++20 |
| CMake | >= 3.15 |
| Compiler | MSVC 19.29+ (Visual Studio 2022 recommended) |
| Platform | Windows x64 |

All third-party dependencies are pre-built and bundled in `thirdparty/`:

| Dependency | Purpose | Linkage |
|---|---|---|
| Protocol Buffers | Serialization backend | Static (`libprotobuf.lib`) |
| FlatBuffers | Serialization backend | Static (`flatbuffers.lib`) |
| zstd | Chunk compression | Dynamic (`zstd.dll`) |
| nlohmann/json | Schema JSON parsing | Header-only |
| xxHash | Content hashing | Header-only (inline) |

Tools additionally fetch at build time (via CMake `FetchContent`):
- **Dear ImGui** — GUI framework (inspector)
- **GLFW** — Window/input (inspector)

## Quick Start

### Build Script

```bat
build_sdk.bat
```

Configures, builds (Release), and installs to `dist/`.

### CMake (Manual)

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist
```

### CMake Presets (VS 2022)

```bat
cmake --preset windows-release
cmake --build --preset windows-release
```

Available presets are defined in `CMakePresets.json`.

## CMake Options

| Option | Default | Description |
|---|---|---|
| `VTX_BUILD_WRITER` | `ON` | Build the vtx_writer module |
| `VTX_BUILD_READER` | `ON` | Build the vtx_reader module |
| `VTX_BUILD_DIFFER` | `ON` | Build the vtx_differ module |
| `BUILD_VTX_TOOL` | `ON` | Build the tool suite (requires reader + writer) |
| `BUILD_VTX_SAMPLES` | `ON` | Build sample programs (five targets -- see [SAMPLES.md](SAMPLES.md)) |

### SDK Only (No Tools)

```bat
cmake -S . -B build -A x64 -DBUILD_VTX_TOOL=OFF
cmake --build build --config Release --parallel
```

This skips ImGui/GLFW fetching and builds only the 4 static libraries.

### Minimal Build (Common Only)

```bat
cmake -S . -B build -A x64 -DVTX_BUILD_WRITER=OFF -DVTX_BUILD_READER=OFF -DVTX_BUILD_DIFFER=OFF -DBUILD_VTX_TOOL=OFF -DBUILD_VTX_SAMPLES=OFF
```

## Build Outputs

After `cmake --install`:

```
dist/
  include/vtx/          Public SDK headers
    common/             Core types, logger, helpers
    reader/             Reader facade and types
    writer/             Writer facade and types
    differ/             Differ facade and types
  lib/
    vtx_common.lib      Core library
    vtx_writer.lib      Writer library
    vtx_reader.lib      Reader library
    vtx_differ.lib      Differ library
    cmake/VTX/          CMake package config
  bin/
    vtx_inspector.exe   GUI inspector (if tools enabled)
    vtx_cli.exe         CLI inspector (if tools enabled)
    vtx_schema_creator.exe  Schema tool (if tools enabled)
    vtx_sample_read.exe         reader smoke test       (if samples enabled)
    vtx_sample_write.exe        writer smoke test       (if samples enabled)
    vtx_sample_diff.exe         hash-based frame diff   (if samples enabled)
    vtx_sample_generate.exe     arena simulator         (if samples enabled)
    vtx_sample_advance_write.exe  arena data-source pipeline (if samples enabled)
    zstd.dll            Runtime dependency
  scripts/
    vtx_codegen.py      Code generation utility
```

## Integration via CMake

After installing, downstream projects can use `find_package`:

```cmake
find_package(VTX REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE VTX::vtx_reader VTX::vtx_differ)
```

Available imported targets:

| Target | Module | Always available? |
|---|---|---|
| `VTX::vtx_common` | Core types and utilities | Yes |
| `VTX::vtx_writer` | Replay writer | If `VTX_BUILD_WRITER=ON` |
| `VTX::vtx_reader` | Replay reader | If `VTX_BUILD_READER=ON` |
| `VTX::vtx_differ` | Frame differ | If `VTX_BUILD_DIFFER=ON` |

The `VTX_DIR` or `CMAKE_PREFIX_PATH` must point to the install prefix:

```bat
cmake -S . -B build -DCMAKE_PREFIX_PATH=path/to/vtx/dist
```

## Project Structure

```
VTX/
  CMakeLists.txt          Root build file
  CMakePresets.json       VS 2022 build presets
  cmake/
    VTXConfig.cmake.in    Package config template for find_package()
  sdk/
    include/vtx/          Public API headers
    src/
      vtx_common/         Core library sources + generated code
      vtx_reader/         Reader implementation
      vtx_writer/         Writer implementation
      vtx_differ/         Differ implementation
      schemas/            .proto and .fbs schema definitions
  tools/
    cli/                  Headless JSON CLI inspector
    inspector/            ImGui GUI inspector
    schema_creator/       Interactive schema definition tool
    shared/               Shared UI utilities (session base, logger sink)
  thirdparty/             Pre-built dependencies (Windows x64)
  samples/                Five example programs -- see docs/SAMPLES.md
    schemas/              Arena wire schemas (.proto + .fbs) used by advanced samples
    content/              VTX schema + runtime-generated data sources + .vtx outputs
  scripts/                Code generation (vtx_codegen.py)
  docs/                   Documentation
```

## Running the Samples

After a successful build (samples default to `ON`), the five sample executables land in `build/bin/Release/`. The full arena pipeline runs end-to-end with:

```bat
cd samples

..\build\bin\Release\vtx_sample_generate.exe        REM simulate -> 3 data sources
..\build\bin\Release\vtx_sample_advance_write.exe   REM data sources -> 3 .vtx replays
..\build\bin\Release\vtx_sample_read.exe            REM inspect the default replay
..\build\bin\Release\vtx_sample_diff.exe            REM diff frames 0 vs 1
```

Each sample sets its VS debugger working directory to `samples/`, so relative paths like `content/reader/arena/arena_from_fbs_ds.vtx` resolve correctly when running from an IDE. See [SAMPLES.md](SAMPLES.md) for a per-target walkthrough.

## Troubleshooting

### zstd.dll not found at runtime

The build copies `zstd.dll` next to each executable via post-build commands. If running from a different directory, ensure `zstd.dll` is on the PATH or in the working directory.

### Protobuf link errors

VTX links protobuf statically. Ensure `PROTOBUF_STATIC_LIBRARY` is defined (the root CMakeLists.txt sets this globally). Do not link a different protobuf version in the same process.

### Schema generation fails

`protoc.exe` and `flatc.exe` must exist at `thirdparty/protobuf/bin/protoc.exe` and `thirdparty/flatbuffers/bin/flatc.exe`. These are checked by the vtx_common CMakeLists.txt.
