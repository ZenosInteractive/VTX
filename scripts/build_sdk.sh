#!/usr/bin/env bash
# ==============================================================================
#   VTX SDK: Build (Linux / macOS)
# ==============================================================================
#
# Mirrors scripts/build_sdk.bat for non-Windows hosts.  Requires:
#
#   cmake >= 3.15
#   C++20 compiler (gcc >= 11, clang >= 13)
#   protobuf-compiler + libprotobuf-dev  (Debian/Ubuntu)
#
# FlatBuffers and zstd are NOT system dependencies -- both are fetched and
# built from pinned source via CMake FetchContent.  See
# cmake/VtxDependencies.cmake.
#
# Ubuntu/Debian:
#   sudo apt install cmake g++ protobuf-compiler libprotobuf-dev
#
# Fedora/RHEL:
#   sudo dnf install cmake gcc-c++ protobuf-compiler protobuf-devel
#
# macOS (Homebrew):
#   brew install cmake protobuf
#
# The GUI tools (inspector, schema_creator) still carry Windows-only glue
# and default OFF on non-Windows.  The SDK libs, the CLI tool, the samples,
# and the test suite build and run on Linux.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# --- 0. Sanity checks -------------------------------------------------------

if ! command -v cmake >/dev/null 2>&1; then
    echo "[ERROR] cmake not found.  Install with your package manager."
    echo "        Ubuntu/Debian:  sudo apt install cmake"
    echo "        Fedora/RHEL:    sudo dnf install cmake"
    echo "        macOS:          brew install cmake"
    exit 1
fi

if ! command -v protoc >/dev/null 2>&1; then
    echo "[ERROR] protoc not found.  Install protobuf-compiler."
    exit 1
fi

if ! command -v flatc >/dev/null 2>&1; then
    echo "[ERROR] flatc not found.  Install flatbuffers-compiler."
    exit 1
fi

# --- 1. Clean previous artefacts -------------------------------------------

BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_PREFIX="${INSTALL_PREFIX:-$REPO_ROOT/dist}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

if [[ "${CLEAN:-0}" == "1" ]]; then
    echo "[INFO] Cleaning previous build + dist..."
    rm -rf "$BUILD_DIR" "$INSTALL_PREFIX"
fi

# --- 2. Configure -----------------------------------------------------------

echo "[INFO] Configuring (${BUILD_TYPE})..."
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DBUILD_VTX_TOOL=ON

# --- 3. Build ---------------------------------------------------------------

echo "[INFO] Compiling SDK (${JOBS} jobs)..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$JOBS"

# --- 4. Run the test suite --------------------------------------------------

if [[ "${SKIP_TESTS:-0}" != "1" ]]; then
    echo "[INFO] Running tests..."
    ctest --test-dir "$BUILD_DIR" -C "$BUILD_TYPE" --output-on-failure --parallel "$JOBS"
fi

# --- 5. Install -------------------------------------------------------------

echo "[INFO] Installing into ${INSTALL_PREFIX}..."
cmake --install "$BUILD_DIR" --config "$BUILD_TYPE"

echo
echo "[SUCCESS] Build completed."
echo "   Libraries: ${INSTALL_PREFIX}/lib/"
echo "   Binaries:  ${INSTALL_PREFIX}/bin/"
echo "   Headers:   ${INSTALL_PREFIX}/include/"
