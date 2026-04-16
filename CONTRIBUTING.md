# Contributing to VTX SDK

## Building from Source

### Prerequisites

- Visual Studio 2022 (or MSVC v143+ toolset)
- CMake 3.15+
- Python 3.x (for schema code generation)

### Build

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

Or use the provided script:

```bat
build_sdk.bat
```

## Code Style

### General

- **Indentation**: 4 spaces (no tabs)
- **Line length**: 120 characters max
- **Header guards**: `#pragma once` (no `#ifndef` guards)
- **Braces**: K&R style (opening brace on same line)
- **Encoding**: UTF-8, CRLF line endings

A `.clang-format` file is provided for automatic formatting of new code. Do not mass-reformat existing files.

### Naming Conventions

| Element | Convention | Example |
|---|---|---|
| Namespaces | PascalCase | `VTX`, `VtxDiff` |
| Classes / Structs | PascalCase | `ReplayReader`, `PropertyContainer` |
| Methods | PascalCase | `GetFrame()`, `LoadFromJson()` |
| Member variables | snake_case with trailing `_` | `chunk_cache_`, `header_` |
| Local variables | snake_case | `frame_index`, `total_frames` |
| Constants | `k` prefix + PascalCase | `kCliVersion`, `kMaxChunkSize` |
| Macros | ALL_CAPS | `VTX_INFO`, `PROTOBUF_STATIC_LIBRARY` |
| Template params | PascalCase | `TNodeView`, `Policy` |

### Namespace Map

| Namespace | Scope |
|---|---|
| `VTX` | Core SDK: types, reader, writer, common utilities |
| `VtxDiff` | Diff engine and sub-namespaces (`VtxDiff::Flatbuffers`, `VtxDiff::Protobuf`) |
| `VtxCli` | CLI tool: engine, commands, transports |
| `VtxServices` | Inspector GUI services |
| `cppvtx` | Auto-generated Protobuf code (do not modify) |
| `fbsvtx` | Auto-generated FlatBuffers code (do not modify) |
| `DiffUtils` | Diff utility functions |

### Include Paths

The SDK uses flat include paths from `sdk/include/`:

```cpp
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/differ/core/vtx_diff_types.h"
```

All public headers live under `sdk/include/vtx/`. The CMake targets expose this directory automatically.

### Logging

Use the VTX logger macros instead of `std::cout` / `std::cerr`:

```cpp
VTX_INFO("Loaded {} frames", frame_count);
VTX_WARN("Struct '{}' not found", name);
VTX_ERROR("Failed to open: {}", path);
VTX_DEBUG("Cache hit for chunk {}", idx);
```

These use C++20 `std::format` syntax (`{}`), **not** printf-style (`%s`, `%d`).

## Pull Request Process

1. Create a feature branch from `main`
2. Make focused, incremental changes
3. Ensure the project builds without warnings
4. Test with sample replay files when possible
5. Write clear commit messages describing _why_, not just _what_

## Project Structure

```
sdk/
  include/vtx/    Public API headers
  src/            Implementation (schemas/, vtx_common/, vtx_reader/, vtx_writer/, vtx_differ/)
tools/
  cli/            Headless CLI inspector
  inspector/      GUI inspector (ImGui)
  schema_creator/ Schema definition tool
  shared/         Shared UI library
thirdparty/       Bundled dependencies (protobuf, flatbuffers, zstd, json, xxhash)
scripts/          Build and code generation scripts
cmake/            CMake package configuration
samples/          Example code and sample replay data
```
