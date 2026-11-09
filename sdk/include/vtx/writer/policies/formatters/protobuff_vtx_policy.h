#pragma once
#include <string>
#include <vector>
#include <memory>

#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/writer/serialization/vtx_proto_encode.h"

namespace cppvtx {
    class Frame;
    class ContextualSchema;
} // namespace cppvtx

namespace VTX {
    struct ProtobufVtxPolicy {
        using FrameType = cppvtx::Frame;
        using SchemaType = cppvtx::ContextualSchema;
        using HeaderType = VTX::SessionConfig;
        /// Encodes a native frame straight to the bytes of its FrameType (ReplayWriter's direct path).
        using DirectEncoder = VTX::Serialization::ProtoFrameEncoder;

        static std::string GetMagicBytes();

        static std::unique_ptr<FrameType> FromNative(VTX::Frame&& native);

        static size_t GetFrameSize(const FrameType& frame);

        static SchemaType CreateSchema(const SchemaRegistry& registry);

        static std::string SerializeHeader(const VTX::SessionConfig& config, const SchemaType& schema);
        /// @brief Serializes @p frames as one chunk. The frames are left as they were, so a writer can reuse them.
        static std::string SerializeChunk(const std::vector<std::unique_ptr<FrameType>>& frames, int32_t chunkIdx,
                                          bool is_compressed);
        /// @brief A chunk of frames already serialized (each a FrameType's bytes, as DirectEncoder writes them):
        ///        byte for byte what SerializeChunk writes for the same frames, without serializing them again.
        static std::string SerializeChunkFromFrames(const std::vector<std::string>& frames, int32_t chunkIdx,
                                                    bool is_compressed);

        static std::string SerializeFooter(const std::vector<ChunkIndexData>& seekTable,
                                           const SessionFooter& footerData);
    };
} // namespace VTX
