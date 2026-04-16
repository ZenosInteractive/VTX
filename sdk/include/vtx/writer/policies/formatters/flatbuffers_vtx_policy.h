#pragma once
#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/writer/serialization/vtx_to_flatbuffer.h"

namespace fbsvtx
{
    struct ContextualSchemaT;
}

namespace VTX {

    struct  FlatBuffersVtxPolicy {
        using HeaderType      = VTX::SessionConfig;
        using FrameType       = VTX::Frame; 
        using SchemaType = std::unique_ptr<fbsvtx::ContextualSchemaT>;

        static std::string GetMagicBytes();
        static std::unique_ptr<FrameType> FromNative(VTX::Frame&& native);
        static size_t GetFrameSize(const FrameType& /*frame*/);
        static SchemaType CreateSchema(const SchemaRegistry& registry);
        static std::string SerializeHeader(const VTX::SessionConfig& config, const SchemaType& schema);
        static std::string SerializeChunk(const std::vector<std::unique_ptr<FrameType>>& frames, int32_t chunk_idx, bool is_compressed);
        static std::string SerializeFooter(const std::vector<ChunkIndexData>& seek_table, const SessionFooter& footer_data);
    };
}
