#pragma once

#include <concepts>
#include <cstddef>
#include <span>

#include "vtx/common/vtx_types.h"

namespace VTX {

    /// Shared compile-time contract for adapters used by streaming frame data
    /// sources (`PipeFrameDataSource`, `WebSocketFrameDataSource`, ...).
    ///
    /// A streaming source owns the transport and the framing; it hands each
    /// raw message payload to an adapter, which decodes those bytes into a
    /// `VTX::Frame` + `GameTimeRegister`.  This keeps the sources fully
    /// format-agnostic -- the adapter is the only place that knows the wire
    /// format (JSON, Protobuf, a custom binary layout, ...).
    ///
    /// `ParseFrame` returns true when the payload was decoded into a usable
    /// frame, and false to signal a decode error (a source treats false as a
    /// reason to end the stream).
    template <typename A>
    concept IFramePayloadAdapter = requires(A& adapter, std::span<const std::byte> payload, VTX::Frame& frame,
                                            VTX::GameTime::GameTimeRegister& time) {
                                       { adapter.ParseFrame(payload, frame, time) } -> std::convertible_to<bool>;
                                   };

} // namespace VTX
