# Changelog

All notable changes to the VTX SDK will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
