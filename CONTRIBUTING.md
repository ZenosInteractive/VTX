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
scripts\build_sdk.bat
```

## Code Style

### General

- **Indentation**: 4 spaces (no tabs)
- **Line length**: 120 characters max
- **Header guards**: `#pragma once` (no `#ifndef` guards)
- **Braces**: K&R style (opening brace on same line)
- **Encoding**: UTF-8, CRLF line endings

A `.clang-format` file is provided for automatic formatting of new code. Do not mass-reformat existing files.

### Validate formatting before pushing

CI runs a clang-format diff-gate on every PR: only the **lines** you've modified vs the base branch get checked (pre-existing files predate the style file and are exempt). To catch violations locally before pushing, use the helper script:

```bash
# Check (read-only) -- same logic as CI
python scripts/check_clang_format.py

# Auto-fix in place
python scripts/check_clang_format.py --fix
```

There are `.sh` and `.bat` wrappers if you prefer them (`scripts/check_clang_format.sh` / `.bat`). The script auto-detects `clang-format-diff.py` on Windows under `Program Files\LLVM\share\clang\` if it's not on `PATH`.

### Pre-push hook (optional, opt-in)

To run the format check automatically before every `git push`, activate the versioned hook directory once in your clone:

```bash
git config core.hooksPath scripts/git-hooks
```

That activates `scripts/git-hooks/pre-push`, which calls the format check and aborts the push if it fails. Bypass for a one-off push with `git push --no-verify`. Deactivate with `git config --unset core.hooksPath`.

No external dependencies -- `core.hooksPath` is built into git ≥ 2.9.

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
3. Run `python scripts/check_clang_format.py` before pushing (or set up the pre-push hook -- see [Validate formatting before pushing](#validate-formatting-before-pushing))
4. Ensure the project builds without warnings
5. Test with sample replay files when possible
6. Write clear commit messages describing _why_, not just _what_

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
thirdparty/       Header-only deps (json, xxhash) + legacy Windows protobuf binary fallback
scripts/          Build and code generation scripts
cmake/            CMake package configuration
samples/          Example code and sample replay data
```
