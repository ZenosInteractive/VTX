#pragma once
#include "vtx/common/vtx_types.h"
#include <optional>

namespace VTX {
    class IFrameDataSource {
    public:
        virtual ~IFrameDataSource() = default;
        virtual bool Initialize() = 0;
        virtual bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) = 0;
        virtual size_t GetExpectedTotalFrames() const = 0;
    };

} // namespace VTX