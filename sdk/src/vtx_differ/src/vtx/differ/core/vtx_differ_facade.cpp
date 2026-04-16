#include "vtx/differ/core/vtx_differ_facade.h"

#include "vtx/differ/core/vtx_default_tree_diff.h"
#include "vtx/differ/adapters/flatbuffers/vtx_flatbuffer_view_adapter.h"
#include "vtx/differ/adapters/protobuff/vtx_protobuff_view_adapter.h"

namespace VtxDiff {


class FlatBuffersDifferFacade final : public IVtxDifferFacade {
public:
    FlatBuffersDifferFacade() {
        factory_.InitFromMemory(nullptr, 0, "fbsvtx.Bucket");
    }

    PatchIndex DiffRawFrames(
        std::span<const std::byte> frame_a,
        std::span<const std::byte> frame_b,
        const DiffOptions& options) override
    {
        auto node_a = factory_.CreateRoot(frame_a);
        auto node_b = factory_.CreateRoot(frame_b);
        if (!node_a || !node_b) return {};

        DefaultTreeDiff<Flatbuffers::FlatbufferViewAdapter> engine;
        return engine.ComputeDiff(*node_a, *node_b, options);
    }

private:
    Flatbuffers::FbViewFactory factory_;
};


class ProtobufDifferFacade final : public IVtxDifferFacade {
public:
    ProtobufDifferFacade() {
        factory_.InitFromMemory(nullptr, 0, "cppvtx.Bucket");
    }

    PatchIndex DiffRawFrames(
        std::span<const std::byte> frame_a,
        std::span<const std::byte> frame_b,
        const DiffOptions& options) override
    {
        auto node_a = factory_.CreateRoot(frame_a);
        auto node_b = factory_.CreateRoot(frame_b);
        if (!node_a || !node_b) return {};

        DefaultTreeDiff<Protobuf::FProtobufViewAdapter> engine;
        return engine.ComputeDiff(*node_a, *node_b, options);
    }

private:
    Protobuf::PbViewFactory factory_;
};


std::unique_ptr<IVtxDifferFacade> CreateDifferFacade(VTX::VtxFormat format) {
    switch (format) {
        case VTX::VtxFormat::FlatBuffers:
            return std::make_unique<FlatBuffersDifferFacade>();
        case VTX::VtxFormat::Protobuf:
            return std::make_unique<ProtobufDifferFacade>();
        default:
            return nullptr;
    }
}

} // namespace VtxDiff
