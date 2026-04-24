#!/usr/bin/env bash
# ==============================================================================
#   VTX SDK: Release Build (Linux / macOS)
# ==============================================================================
#
# Mirrors scripts/release_sdk.bat for non-Windows hosts.  Builds the SDK libraries
# plus the CLI tool in Release mode and installs them into ./dist.
#
# The GUI tools (inspector, schema_creator) carry Windows-only glue and
# default OFF on non-Windows, so the release package on Linux/macOS ships
# the libraries + vtx_cli + samples + headers.
#
# Ubuntu/Debian:
#   sudo apt install cmake g++ protobuf-compiler libprotobuf-dev
#
# Fedora/RHEL:
#   sudo dnf install cmake gcc-c++ protobuf-compiler protobuf-devel
#
# macOS (Homebrew):
#   brew install cmake protobuf
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

echo "=========================================="
echo "     VTX SDK: Release Build"
echo "=========================================="

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
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

echo "[INFO] Cleaning previous build + dist..."
rm -rf "$BUILD_DIR" "$INSTALL_PREFIX"

# --- 2. Configure -----------------------------------------------------------

echo "[INFO] Configuring release build..."
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DBUILD_VTX_TOOL=ON

# --- 3. Build release targets ----------------------------------------------

echo "[INFO] Compiling release targets (${JOBS} jobs)..."
# vtx_cli is the only tool that builds cross-platform.
# The SDK libraries (vtx_reader, vtx_differ, vtx_common) come along for free.
cmake --build "$BUILD_DIR" --config Release --target vtx_cli --parallel "$JOBS"

# --- 4. Install -------------------------------------------------------------

echo "[INFO] Installing release output into ${INSTALL_PREFIX}..."
cmake --install "$BUILD_DIR" --config Release

echo
echo "[SUCCESS] Release dist is ready."
echo "   Libraries: ${INSTALL_PREFIX}/lib/"
echo "   Binaries:  ${INSTALL_PREFIX}/bin/"
echo "   Headers:   ${INSTALL_PREFIX}/include/"
