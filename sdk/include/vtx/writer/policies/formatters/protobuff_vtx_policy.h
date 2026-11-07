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

        /// @brief FromNative into @p reuse, a frame message a previous chunk is done with: it is cleared and
        ///        filled again, so the nested messages and strings it already holds are reused instead of
        ///        allocated anew. A null @p reuse allocates a new frame.
        static std::unique_ptr<FrameType> FromNative(VTX::Frame&& native, std::unique_ptr<FrameType> reuse);

        static size_t GetFrameSize(const FrameType& frame);

        static SchemaType CreateSchema(const SchemaRegistry& registry);

        static std::string SerializeHeader(const VTX::SessionConfig& config, const SchemaType& schema);
        /// @brief Serializes @p frames as one chunk. The frames are left as they were, so a writer can reuse them.
        static std::string SerializeChunk(const std::vector<std::unique_ptr<FrameType>>& frames, int32_t chunkIdx,
                                          bool is_compressed);
        /// @brief Serializes one frame into @p out, reusing its capacity, and returns the frame's size: the
        ///        same bytes SerializeChunk writes for that frame, and the same size GetFrameSize reports.
        static size_t SerializeFrame(const FrameType& frame, std::string& out);

        /// @brief A chunk of frames already serialized by SerializeFrame: byte for byte what SerializeChunk
        ///        writes for the same frames, without serializing them again.
        static std::string SerializeChunkFromFrames(const std::vector<std::string>& frames, int32_t chunkIdx,
                                                    bool is_compressed);

        static std::string SerializeFooter(const std::vector<ChunkIndexData>& seekTable,
                                           const SessionFooter& footerData);
    };
} // namespace VTX
