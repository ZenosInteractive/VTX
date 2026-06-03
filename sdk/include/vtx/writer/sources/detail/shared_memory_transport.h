#pragma once

// ISharedMemoryTransport -- compile-time contract for transports plugged
// into VTX::SharedMemoryFrameDataSource.
//
// A "transport" owns the wire layout of the shared-memory segment + the
// publish/subscribe protocol.  The frame source owns the platform code
// (Win32 CreateFileMapping / POSIX shm_open + mmap) and the polling loop,
// and delegates ALL knowledge of "how is the segment structured" to the
// transport.
//
// This is the v2 extensibility surface documented in
// docs/SHARED_MEMORY_INPUT.md section 20.  The default transport
// (SpscRingTransport) preserves backwards-compatible behaviour --
// anyone using SharedMemoryFrameDataSource<Adapter> with no explicit
// transport gets the same SPSC ring + per-slot seq + drop-oldest
// protocol that was the v1 implementation.
//
// To add a new transport (e.g. UGI's StreamControlBlockV1 + seqlock,
// or an Aeron-compatible logbuffer): write a struct that satisfies
// this concept, instantiate
// `SharedMemoryFrameDataSource<MyAdapter, MyTransport>`, and the same
// public API works.

#include "vtx/writer/sources/detail/shm_ring.h"

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace VTX {

    /// A transport must expose the operations the frame source needs.
    /// The semantics are:
    ///   * `IsValid()`          -- caller-provided transport config is
    ///                             internally consistent (e.g. capacity
    ///                             is a power of two for ring-based
    ///                             transports).  Returns false on
    ///                             invalid config; the frame source
    ///                             bails out of `Initialize()` early.
    ///   * `SegmentBytes()`     -- total bytes the segment must be sized
    ///                             to by the platform allocator.
    ///   * `SlotPayloadCapacity()` -- bytes available for the payload of
    ///                             a single frame (i.e. the maximum the
    ///                             adapter will see).  Used to size the
    ///                             frame source's scratch buffer once.
    ///   * `InitSegment(base)`  -- server-mode: write any wire-format
    ///                             metadata (magic, version, cursors, ...)
    ///                             into the freshly mapped segment.
    ///   * `ValidateSegment(base)` -- client-mode: verify the segment
    ///                             attached to matches the transport's
    ///                             expected layout (magic, version,
    ///                             capacity, ...).
    ///   * `Pop(base, out, out_capacity, out_size, dropped)` -- read one
    ///                             frame.  Same PopResult semantics as
    ///                             the SPSC primitives: Ok / Empty /
    ///                             Closed / TornRead.  `dropped` is an
    ///                             in/out counter the transport adds to.
    ///
    /// The frame source never calls anything else on the transport.
    template <typename T>
    concept ISharedMemoryTransport = requires(T& t, const T& ct, void* base, const void* cbase, std::byte* out,
                                              uint32_t out_capacity, uint32_t& out_size, uint64_t& dropped) {
                                         { ct.IsValid() } -> std::convertible_to<bool>;
                                         { ct.SegmentBytes() } -> std::convertible_to<std::size_t>;
                                         { ct.SlotPayloadCapacity() } -> std::convertible_to<uint32_t>;
                                         { t.InitSegment(base) } -> std::same_as<void>;
                                         { ct.ValidateSegment(cbase) } -> std::convertible_to<bool>;
                                         {
                                             t.Pop(base, out, out_capacity, out_size, dropped)
                                             } -> std::convertible_to<shm_detail::PopResult>;
                                     };

} // namespace VTX
