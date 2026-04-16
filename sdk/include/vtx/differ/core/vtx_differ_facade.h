#pragma once

#include <memory>
#include <span>
#include <cstddef>

#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/common/vtx_types.h"

namespace VtxDiff {
    class IVtxDifferFacade {
    public:
        virtual ~IVtxDifferFacade() = default;

        virtual PatchIndex DiffRawFrames(std::span<const std::byte> frame_a,std::span<const std::byte> frame_b,const DiffOptions& options = {}) = 0;
    };

    // Returns nullptr only if format is Unknown.
    std::unique_ptr<IVtxDifferFacade> CreateDifferFacade(VTX::VtxFormat format);
} // namespace VtxDiff
