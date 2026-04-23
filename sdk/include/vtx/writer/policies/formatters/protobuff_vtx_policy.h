#pragma once
#include <string>
#include <vector>
#include <memory>

#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"

namespace cppvtx {
    class Frame;
    class ContextualSchema;
} // namespace cppvtx

namespace VTX {
    struct ProtobufVtxPolicy {
        using FrameType = cppvtx::Frame;
        using SchemaType = cppvtx::ContextualSchema;
        using HeaderType = VTX::SessionConfig;

        static std::string GetMagicBytes();

        static std::unique_ptr<FrameType> FromNative(VTX::Frame&& native);

        static size_t GetFrameSize(const FrameType& frame);

        static SchemaType CreateSchema(const SchemaRegistry& registry);

        static std::string SerializeHeader(const VTX::SessionConfig& config, const SchemaType& schema);
        static std::string SerializeChunk(const std::vector<std::unique_ptr<FrameType>>& frames, int32_t chunkIdx,
                                          bool is_compressed);
        static std::string SerializeFooter(const std::vector<ChunkIndexData>& seekTable,
                                           const SessionFooter& footerData);
    };
} // namespace VTX
