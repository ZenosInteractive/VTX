# Build Guide

> Looking for runnable example programs? See [SAMPLES.md](SAMPLES.md).

## Continuous Integration

Every push to `main` / `master` and every pull request triggers the CI workflow at `.github/workflows/build.yml`. It runs a clang-format gate plus a six-job build-and-test matrix.

### Formatting gate

A dedicated `clang-format` job checks every C++ file **added or modified** by the PR / push against `.clang-format`. Pre-existing files are not checked -- about 90% of the codebase predates the style file, so a strict full-repo check would be permanently red. The gate catches *new* formatting regressions without forcing a one-time blame-destroying style sweep.

If you want to apply the style to the whole tree in one commit (consider recording that SHA in `.git-blame-ignore-revs` afterwards):

```bash
clang-format -i $(find sdk tools samples tests \
    -type f \( -name "*.cpp" -o -name "*.cc" -o -name "*.h" -o -name "*.hpp" \) \
    -not -path "*/generated/*" \
    -not -path "*arena_generated.h" \
    -not -path "*portable-file-dialogs.h")
```

The CI uses `clang-format-15` for reproducibility across runs. Locally, any recent clang-format (14+) works with the config.

### Build + test matrix

| # | OS | Build type | Library type | Dependency source |
|---|---|---|---|---|
| 1 | Windows | Release | static | bundled thirdparty |
| 2 | Windows | Release | shared (DLL) | bundled thirdparty |
| 3 | Windows | Debug | static | bundled thirdparty |
| 4 | Linux | Release | static | apt (protobuf) + FetchContent (flatbuffers, zstd) |
| 5 | Linux | Release | shared (.so) | apt + FetchContent |
| 6 | Linux | Debug | static | apt + FetchContent |

Each job runs the full 187-test ctest suite plus the sample smoke tests. GUI tools (`vtx_inspector`, `vtx_schema_creator`) are disabled in CI because they fetch ImGui + GLFW and still contain Windows-only glue; they get tested manually.

A failing CI run uploads `build/Testing/` and any dropped test artefacts as a job artefact (retained for 7 days) so the failure can be inspected without reproducing locally.

If you add a new platform target or dependency, extend the matrix in `build.yml` -- don't create a parallel workflow file.

## Requirements

| Requirement | Version |
|---|---|
| C++ Standard | C++20 |
| CMake | >= 3.15 |
| Compiler | MSVC 19.29+ (Visual Studio 2022) or gcc 11+ / clang 13+ |
| Platform | Windows x64, Linux x86_64, macOS (SDK + CLI only) |

## Dependency Resolution

Header-only dependencies always come from the repo:

- `thirdparty/jsonlohmann`
- `thirdparty/xxhash`

Binary dependencies are resolved through `cmake/VtxDependencies.cmake`:

- Protocol Buffers (`protoc` + `libprotobuf`) -- system package on Linux / macOS, bundled `thirdparty/protobuf/` or vcpkg on Windows
- FlatBuffers (`flatc` + headers) -- always FetchContent, pinned to `v24.12.23`
- zstd -- always FetchContent, pinned to `v1.5.6`, linked statically

### Windows

Preferred flow: package-managed dependencies with the included `vcpkg.json` manifest.

```bat
vcpkg install
cmake -S . -B build -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVTX_DEPENDENCY_SOURCE=PACKAGE_MANAGER
cmake --build build --config Release --parallel
```

Legacy fallback:

```bat
cmake -S . -B build -A x64 -DVTX_DEPENDENCY_SOURCE=BUNDLED
cmake --build build --config Release --parallel
```

`VTX_DEPENDENCY_SOURCE` accepts:

| Value | Meaning |
|---|---|
| `AUTO` | Try package-manager resolution first, then fall back to `thirdparty/` |
| `PACKAGE_MANAGER` | Require dependencies from vcpkg / installed packages |
| `BUNDLED` | Require the legacy prebuilt artifacts under `thirdparty/` |

### Linux / macOS

Install packages through the system package manager:

```bash
# Ubuntu / Debian
sudo apt install cmake g++ protobuf-compiler libprotobuf-dev

# Fedora / RHEL / Rocky
sudo dnf install cmake gcc-c++ protobuf-compiler protobuf-devel

# macOS (Homebrew)
brew install cmake protobuf
```

Neither FlatBuffers nor zstd is a system dependency -- the build fetches pinned source releases via CMake's `FetchContent`, compiles `flatc` + the zstd static library, and consumes everything in-tree.  This keeps the wire format and the compression library on the exact same version across Windows, Linux, and macOS regardless of what the distro packages.  First configure takes +30-90s while those two build from source; subsequent configures are instant thanks to the `build/_deps` cache.

**Linux GUI tools (optional)** — the Inspector and Schema Creator additionally need the X11 dev stack because GLFW links against it. The default Linux build skips both tools (`BUILD_VTX_INSPECTOR` and `BUILD_VTX_SCHEMA_CREATOR` default to `OFF` on non-Windows), and the `tools/CMakeLists.txt` orchestrator only fetches GLFW when at least one GUI tool is enabled -- so a headless Linux build has zero X11 requirements. If you opt them back in:

```bash
# Ubuntu / Debian
sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
```

## Quick Start

### Windows build script

```bat
scripts\build_sdk.bat
```

### Linux / macOS build script

```bash
./scripts/build_sdk.sh
```

Environment overrides:

```bash
BUILD_TYPE=Debug ./scripts/build_sdk.sh
SKIP_TESTS=1 ./scripts/build_sdk.sh
CLEAN=1 ./scripts/build_sdk.sh
JOBS=16 ./scripts/build_sdk.sh
INSTALL_PREFIX=/opt/vtx ./scripts/build_sdk.sh
```

### Manual CMake

Windows:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist
```

Linux / macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist
```

### CMake Presets

```bat
cmake --preset windows-release
cmake --build --preset windows-release
```

## CMake Options

| Option | Default | Description |
|---|---|---|
| `VTX_BUILD_WRITER` | `ON` | Build the writer module |
| `VTX_BUILD_READER` | `ON` | Build the reader module |
| `VTX_BUILD_DIFFER` | `ON` | Build the differ module |
| `BUILD_VTX_TOOL` | `ON` | Build tools |
| `BUILD_VTX_SAMPLES` | `ON` | Build sample programs |
| `VTX_BUILD_TESTS` | `ON` | Build the unit test suite |
| `VTX_BUILD_SHARED` | `OFF` | Build shared SDK libraries instead of static |
| `VTX_DEPENDENCY_SOURCE` | `AUTO` | Windows dependency source selection |

SDK-only build:

```bat
cmake -S . -B build -A x64 -DBUILD_VTX_TOOL=OFF
cmake --build build --config Release --parallel
```

Minimal common-only build:

```bat
cmake -S . -B build -A x64 ^
  -DVTX_BUILD_WRITER=OFF ^
  -DVTX_BUILD_READER=OFF ^
  -DVTX_BUILD_DIFFER=OFF ^
  -DBUILD_VTX_TOOL=OFF ^
  -DBUILD_VTX_SAMPLES=OFF
```

## Static vs Shared

Static is the default. Shared builds are enabled with:

```bat
cmake -S . -B build -A x64 -DVTX_BUILD_SHARED=ON
cmake --build build --config Release --parallel
```

Shared-build caveats:

1. Windows still uses `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`.
2. If your process also links another protobuf runtime, ABI duplication is your problem.
3. Installed Linux binaries rely on `$ORIGIN/../lib` RPATH.

## Outputs

After `cmake --install`:

```text
dist/
  include/vtx/
  lib/
    vtx_common.lib
    vtx_writer.lib
    vtx_reader.lib
    vtx_differ.lib
    cmake/VTX/
  bin/
    vtx_inspector.exe
    vtx_cli.exe
    vtx_schema_creator.exe
    vtx_sample_read.exe
    vtx_sample_write.exe
    vtx_sample_diff.exe
    vtx_sample_generate.exe
    vtx_sample_advance_write.exe
  scripts/
    vtx_codegen.py
```

## Samples and Validation

`basic_diff.cpp` is now validated through `ctest` via a smoke test that runs the
compiled sample against `samples/content/reader/arena/arena_from_fbs_ds.vtx`
with `--fail-on-empty`.

Manual sample flow:

```bat
cd samples

..\build\bin\Release\vtx_sample_generate.exe
..\build\bin\Release\vtx_sample_advance_write.exe
..\build\bin\Release\vtx_sample_read.exe
..\build\bin\Release\vtx_sample_diff.exe
```

## Integration via CMake

```cmake
find_package(VTX REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE VTX::vtx_reader VTX::vtx_differ)
```

Imported targets:

| Target | Module |
|---|---|
| `VTX::vtx_common` | Core types and utilities |
| `VTX::vtx_writer` | Replay writer |
| `VTX::vtx_reader` | Replay reader |
| `VTX::vtx_differ` | Frame differ |

## Project Structure

```text
VTX/
  CMakeLists.txt
  CMakePresets.json
  cmake/
  sdk/
  tools/
  samples/
  docs/
  scripts/
  thirdparty/   Header-only deps + legacy Windows binary fallback
  vcpkg.json    Windows package-manager manifest
```
## Running benchmarks locally

The benchmark binary is gated behind `VTX_BUILD_BENCHMARKS` and uses `google/benchmark` (fetched via `FetchContent`). Release builds only -- debug numbers are not meaningful.

```bash
# Configure + build
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DVTX_BUILD_BENCHMARKS=ON
cmake --build build-bench --target vtx_benchmarks --config Release --parallel

# Run the full suite (writes JSON + console side-by-side for diffing over time)
build-bench/bin/Release/vtx_benchmarks.exe \
    --benchmark_out=docs/benchmarks/bench_$(date +%Y%m%d_%H%M%S).json \
    --benchmark_out_format=json \
    --benchmark_counters_tabular=true

# Filter to a single family
build-bench/bin/Release/vtx_benchmarks.exe \
    --benchmark_filter='BM_FrameAccessor_Creation|BM_EntityView_SingleGet|BM_AccessorKeyResolution'
```

Fixtures required at run time:

- `samples/content/reader/{cs,rl,arena}/*.vtx` -- real replays; checked in.
- `benchmarks/fixtures/synth_10k.vtx` -- small synthetic fixture generated by `vtx_sample_write` when `VTX_BUILD_BENCHMARKS=ON`.

Narrative results (what the numbers mean, not just the tables) live in [`docs/PERFORMANCE.md`](PERFORMANCE.md). The raw output of the canonical run is committed under `docs/benchmarks/` as JSON + console text for regression tracking and graphing.

## Running sanitizers locally

VTX's root `CMakeLists.txt` exposes a `VTX_SANITIZE` option that enables gcc/clang runtime sanitizers.  Useful before pushing a change that touches threading or memory-ownership code.

| Value | What it catches |
|---|---|
| `address` | Heap/stack/global out-of-bounds, use-after-free, double-free |
| `undefined` | Integer overflow, null deref, misaligned access, UB conversions |
| `address,undefined` | ASan + UBsan combined (they're compatible, single build) |
| `thread` | Data races, deadlocks (incompatible with ASan -- separate build) |

### ASan + UBsan

```bash
cmake -S . -B build-asan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DVTX_SANITIZE=address,undefined \
    -DBUILD_VTX_INSPECTOR=OFF \
    -DBUILD_VTX_SCHEMA_CREATOR=OFF
cmake --build build-asan --parallel

ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ctest --test-dir build-asan --output-on-failure --timeout 180
```

### TSan (separate build)
```bash
cmake -S . -B build-tsan \
    -DCMAKE_BUILD_TYPE=Debug \
    -DVTX_SANITIZE=thread \
    -DBUILD_VTX_INSPECTOR=OFF \
    -DBUILD_VTX_SCHEMA_CREATOR=OFF
cmake --build build-tsan --parallel

TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
ctest --test-dir build-tsan --output-on-failure --timeout 180
```

If TSan aborts at startup with FATAL: ThreadSanitizer: unexpected memory mapping, the host kernel's ASLR entropy is too high for TSan's shadow-memory layout. Fix for the current session:

```bash
 sudo sysctl vm.mmap_rnd_bits=28
```

### When a sanitizer reports a finding
For each finding, decide:

- Real bug in VTX -- fix it in `sdk/...` and add a regression test in `tests/....`
- False positive or noise from a third-party library -- add a rule to the matching file under `tests/sanitizer_suppressions/` with a comment explaining why. The CI workflow already points the sanitizers at those files via `LSAN_OPTIONS=suppressions=`... etc.

These sanitizer jobs also run on every PR via `.github/workflows/build.yml` (two extra Linux matrix entries: `ASan+UBsan` and `TSan`).

## Troubleshooting

### Windows package-manager configure cannot find dependencies

If you selected `-DVTX_DEPENDENCY_SOURCE=PACKAGE_MANAGER`, CMake expects:

- `protoc`
- `protobuf::libprotobuf`

`flatc` + FlatBuffers headers and the zstd static library are built by
FetchContent on every platform, so they are not probed on the package-
manager path.

Make sure you configured with the vcpkg toolchain file, or that `protoc`
and `libprotobuf` are discoverable through `PATH` / `CMAKE_PREFIX_PATH`.

### Windows bundled fallback configure fails

If you selected `-DVTX_DEPENDENCY_SOURCE=BUNDLED`, the legacy artifacts must
still exist under `thirdparty/protobuf/`. FlatBuffers and zstd are no longer
read from `thirdparty/` on any platform -- `flatc`, the FlatBuffers headers,
and the zstd static library are all built from source via CMake
`FetchContent` into `build/_deps/`.

### Linux cannot find Protobuf

Install the development packages shown above. If they live in a non-standard
prefix, provide `CMAKE_PREFIX_PATH`. FlatBuffers + zstd never need to be
installed system-wide -- FetchContent handles them regardless of what the
distro packages.

### zstd at runtime

There is no runtime zstd dependency on any platform. The library is linked
statically from the FetchContent build, so binaries do not ship a
`zstd.dll` / `libzstd.so.*` alongside them.
