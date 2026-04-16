
#include <algorithm>
#include <chrono>

#include "vtx_schema_generated.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/writer/serialization/vtx_to_flatbuffer.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"

std::string VTX::FlatBuffersVtxPolicy::GetMagicBytes() {
    return "VTXF"; 
}

std::unique_ptr<VTX::FlatBuffersVtxPolicy::FrameType> VTX::FlatBuffersVtxPolicy::FromNative(VTX::Frame&& native) {
    auto sorted_frame = std::make_unique<VTX::Frame>();
    
    const auto& native_buckets = native.GetBuckets();
    if (native_buckets.empty()) return sorted_frame;

    const auto& native_data = native_buckets[0];
    auto& sorted_data = sorted_frame->GetBucket("data");

    const auto& entities = native_data.entities;
    const auto& ids = native_data.unique_ids;

    if (entities.empty()) {
        sorted_data = native_data;
        return sorted_frame;
    }

    int32_t max_type = -1;
    for (const auto& ent : entities) {
        max_type = std::max(ent.entity_type_id, max_type);
    }
    
    if (max_type < 0) {
        sorted_data = native_data;
        return sorted_frame;
    }

    std::vector<std::vector<size_t>> indices_by_type(max_type + 1);
    for (size_t i = 0; i < entities.size(); ++i) {
        int32_t t_id = entities[i].entity_type_id;
        if (t_id >= 0 && t_id <= max_type) {
            indices_by_type[t_id].push_back(i);
        }
    }

    sorted_data.type_ranges.resize(max_type + 1, {0, 0});
    sorted_data.entities.reserve(entities.size());
    sorted_data.unique_ids.reserve(ids.size());

    int32_t current_index = 0;

    for (int32_t type_id = 0; type_id <= max_type; ++type_id) {
        const auto& indices = indices_by_type[type_id];
        
        sorted_data.type_ranges[type_id].start_index = current_index;
        sorted_data.type_ranges[type_id].count = static_cast<int32_t>(indices.size());

        for (size_t orig_idx : indices) {
            sorted_data.entities.push_back(entities[orig_idx]);
            if (orig_idx < ids.size()) {
                sorted_data.unique_ids.push_back(ids[orig_idx]);
            }
            current_index++;
        }
    }

    if (native_buckets.size() > 1) {
        auto& sorted_bones = sorted_frame->GetBucket("bone_data");
        sorted_bones = native_buckets[1];
    }
    
    return sorted_frame;
}

size_t VTX::FlatBuffersVtxPolicy::GetFrameSize(const FrameType& /*frame*/) {
    // FlatBuffers doesnt know the size until is serialized
    return 0; 
}

VTX::FlatBuffersVtxPolicy::SchemaType VTX::FlatBuffersVtxPolicy::CreateSchema(const SchemaRegistry& registry) {
    auto property_schema = std::make_unique<fbsvtx::ContextualSchemaT>();

    property_schema->data_identifier = "VTX_GameData";
    property_schema->data_version = 0;
    property_schema->data_version_string = "15.23 prod";

    property_schema->schema = registry.GetContentAsString();

    return property_schema;
}

std::string VTX::FlatBuffersVtxPolicy::SerializeHeader(const VTX::SessionConfig& config, const SchemaType& schema) {
    flatbuffers::FlatBufferBuilder builder(1024);
    
    auto replay_name = builder.CreateString(config.replay_name);
    auto uuid       = builder.CreateString(config.replay_uuid.empty() ? "uuid_placeholder" : config.replay_uuid);
    auto meta       = builder.CreateString(config.custom_json_metadata);
    
    auto release    = builder.CreateString("1.0.0"); 
    auto hash       = builder.CreateString("UNKNOWN_HASH");

    auto version = fbsvtx::CreateVersionInfo(builder, 
        1, 0, // Major, Minor
        config.schema_version);

    auto now = std::chrono::system_clock::now();
    int64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    
    auto schema_off = fbsvtx::ContextualSchema::Pack(builder, schema.get());
    
    auto header_off = fbsvtx::CreateFileHeader(builder,
        version, 
        schema_off, 
        uuid, 
        replay_name, 
        timestamp,
        meta);

    builder.Finish(header_off);
    return {reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize()};
}


std::string VTX::FlatBuffersVtxPolicy::SerializeChunk(const std::vector<std::unique_ptr<FrameType>>& frames, int32_t chunk_idx, bool is_compressed) {
    if (frames.empty()) return "";

    flatbuffers::FlatBufferBuilder builder(1024 * 1024); 

    std::vector<flatbuffers::Offset<fbsvtx::Frame>> frame_offsets;
    frame_offsets.reserve(frames.size());

    for(const auto& f : frames) {
        if(f) {
            frame_offsets.push_back(VTX::Serialization::ToFlat(builder, *f));
        }
    }
    auto frames_vector = builder.CreateVector(frame_offsets);
    
    auto chunk_off = fbsvtx::CreateChunk(builder, 
        chunk_idx, 
        is_compressed,
        0,
        frames_vector, 
        0
    );

    builder.Finish(chunk_off);
    return {reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize()};
}


std::string VTX::FlatBuffersVtxPolicy::SerializeFooter(const std::vector<ChunkIndexData>& seek_table, const SessionFooter& footer_data) {
    flatbuffers::FlatBufferBuilder builder(2048);

    flatbuffers::Offset<fbsvtx::ReplayTimeData> time_data_offsets = 0;
    
    if(footer_data.game_times || footer_data.created_utc || footer_data.gaps || footer_data.segments) {
        auto gt   = (footer_data.game_times)  ? builder.CreateVector(*footer_data.game_times)  : 0;
        auto cu   = (footer_data.created_utc) ? builder.CreateVector(*footer_data.created_utc) : 0;
        auto gaps = (footer_data.gaps)        ? builder.CreateVector(*footer_data.gaps)        : 0;
        auto segs = (footer_data.segments)    ? builder.CreateVector(*footer_data.segments)    : 0;
        
        time_data_offsets = fbsvtx::CreateReplayTimeData(builder, gt, cu, gaps, segs);
    }

    std::vector<flatbuffers::Offset<fbsvtx::ChunkIndexEntry>> index_offsets;
    index_offsets.reserve(seek_table.size());
    
    for(const auto& e : seek_table) {
        index_offsets.push_back(fbsvtx::CreateChunkIndexEntry(builder, 
            e.chunk_index, 
            e.start_frame, 
            e.end_frame, 
            static_cast<uint64_t>(e.file_offset), 
            e.chunk_size_bytes));
    }
    auto index_vector = builder.CreateVector(index_offsets);

    auto events_vec = builder.CreateVector(std::vector<flatbuffers::Offset<fbsvtx::TimelineEvent>>{});

    auto footer_offset = fbsvtx::CreateFileFooter(builder, 
        footer_data.total_frames, 
        static_cast<float>(footer_data.duration_seconds), 
        time_data_offsets, 
        index_vector, 
        events_vec, 
        0);

    builder.Finish(footer_offset);
    return {reinterpret_cast<const char*>(builder.GetBufferPointer()), builder.GetSize()};
}
