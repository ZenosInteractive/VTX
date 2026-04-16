#include <algorithm>

#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"
#include "vtx/writer/serialization/vtx_to_proto.h"
#include "vtx_schema.pb.h"

std::string VTX::ProtobufVtxPolicy::GetMagicBytes() {
    return "VTXP"; 
}

std::unique_ptr<VTX::ProtobufVtxPolicy::FrameType> VTX::ProtobufVtxPolicy::FromNative(VTX::Frame&& native) {
    const auto& native_buckets = native.GetBuckets();
    VTX::Frame sorted_native;
    sorted_native.GetMutableBuckets().resize(native_buckets.size());
    
    for (size_t b_idx = 0; b_idx < native_buckets.size(); ++b_idx) {
        const auto& src_bucket = native_buckets[b_idx];
        auto& dst_bucket = sorted_native.GetBucket(static_cast<int>(b_idx));

        if (b_idx == 0 && !src_bucket.entities.empty())
        {
            const auto& entities = src_bucket.entities;
            const auto& ids = src_bucket.unique_ids;
            int32_t max_type = -1;
            for (const auto& ent : entities) {
                max_type = std::max(ent.entity_type_id, max_type);
            }

            if (max_type >= 0 && !entities.empty()) {
                std::vector<std::vector<size_t>> indices_by_type(max_type + 1);
                for (size_t i = 0; i < entities.size(); ++i) {
                    int32_t t_id = entities[i].entity_type_id;
                    if (t_id >= 0 && t_id <= max_type) {
                        indices_by_type[t_id].push_back(i);
                    }
                }

                dst_bucket.type_ranges.assign(max_type + 1, {0, 0});
                dst_bucket.entities.reserve(entities.size());
                dst_bucket.unique_ids.reserve(ids.size());

                int32_t current_index = 0;

                for (int32_t type_id = 0; type_id <= max_type; ++type_id) {
                    const auto& indices = indices_by_type[type_id];
                    dst_bucket.type_ranges[type_id].start_index = current_index;
                    dst_bucket.type_ranges[type_id].count = static_cast<int32_t>(indices.size());

                    for (size_t orig_idx : indices) {
                        dst_bucket.entities.push_back(entities[orig_idx]);
                        if (orig_idx < ids.size()) {
                            dst_bucket.unique_ids.push_back(ids[orig_idx]);
                        }
                        current_index++;
                    }
                }
            } else {
                dst_bucket = src_bucket;
            }
        }
        else
        {
            dst_bucket = src_bucket;
        }
    }
        
    auto proto = std::make_unique<cppvtx::Frame>();
    VTX::Serialization::ToProto(sorted_native, proto.get());
    return proto;
}
        
size_t VTX::ProtobufVtxPolicy::GetFrameSize(const VTX::ProtobufVtxPolicy::FrameType& frame) {
    return frame.ByteSizeLong();
}

VTX::ProtobufVtxPolicy::SchemaType VTX::ProtobufVtxPolicy::CreateSchema(const SchemaRegistry& registry) {
    
    SchemaType proto_schema;
    proto_schema.set_data_indentifier("VTX_GameData"); // Default VTX data format identifier
    proto_schema.set_data_version(1);
    proto_schema.set_data_version_string("1.0.0");
    proto_schema.set_schema(registry.GetContentAsString());

    return proto_schema;
}

std::string VTX::ProtobufVtxPolicy::SerializeHeader(const VTX::SessionConfig& config, const SchemaType& schema) {
    cppvtx::FileHeader proto_header;
    proto_header.set_replay_name(config.replay_name);
    proto_header.set_replay_uuid(config.replay_uuid.empty() ? "uuid_placeholder" : config.replay_uuid);
    proto_header.set_custom_json_metadata(config.custom_json_metadata);
    
    auto* v = proto_header.mutable_version();
    v->set_format_major(1); v->set_format_minor(0);
    v->set_schema_version(config.schema_version);

    auto now = std::chrono::system_clock::now();
    proto_header.set_recorded_utc_timestamp(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count()
    );

    proto_header.mutable_prop_schema()->CopyFrom(schema);

    std::string payload;
    proto_header.SerializeToString(&payload);
    return payload;
}

std::string VTX::ProtobufVtxPolicy::SerializeChunk(const std::vector<std::unique_ptr<FrameType>>& frames, int32_t chunkIdx, bool is_compressed) {
    if (frames.empty()) return "";

    cppvtx::FrameChunk chunk_msg;
    chunk_msg.set_chunk_index(chunkIdx);
    chunk_msg.set_is_compressed(is_compressed); 
    
    for (const auto& frame_ptr : frames) {
        if(frame_ptr) {
            chunk_msg.add_frames()->Swap(frame_ptr.get());
        }
    }

    std::string uncompressed_payload;
    if (!chunk_msg.SerializeToString(&uncompressed_payload)) {
        throw std::runtime_error("VTX [ProtobufPolicy]: Error serializing Chunk.");
    }

    return uncompressed_payload; 
}

std::string VTX::ProtobufVtxPolicy::SerializeFooter(const std::vector<ChunkIndexData>& seekTable, 
                                   const SessionFooter& footerData) 
{
    cppvtx::FileFooter footer_msg;
    
    footer_msg.set_total_frames(footerData.total_frames);
    footer_msg.set_duration_seconds(static_cast<float>(footerData.duration_seconds));
    
   if (footerData.game_times || footerData.created_utc || footerData.gaps || footerData.segments) {
        
        auto* times_msg = footer_msg.mutable_times();

        if (footerData.game_times && !footerData.game_times->empty()) {
            const auto& vec = *footerData.game_times;
            times_msg->mutable_game_time()->Reserve(static_cast<int>(vec.size()));
            for (const auto& val : vec) {
                times_msg->add_game_time(val);
            }
        }

        if (footerData.created_utc && !footerData.created_utc->empty()) {
            const auto& vec = *footerData.created_utc;
            times_msg->mutable_created_utc()->Reserve(static_cast<int>(vec.size()));
            for (const auto& val : vec) {
                times_msg->add_created_utc(val);
            }
        }

        if (footerData.gaps && !footerData.gaps->empty()) {
            const auto& vec = *footerData.gaps;
            times_msg->mutable_gaps()->Reserve(static_cast<int>(vec.size()));
            for (const auto& val : vec) {
                times_msg->add_gaps(val);
            }
        }

        if (footerData.segments && !footerData.segments->empty()) {
            const auto& vec = *footerData.segments;
            times_msg->mutable_segments()->Reserve(static_cast<int>(vec.size()));
            for (const auto& val : vec) {
                times_msg->add_segments(val);
            }
        }
    }
    
    footer_msg.mutable_chunk_index()->Reserve(static_cast<int>(seekTable.size()));
    
    for (const auto& entry : seekTable) {
        auto* proto_entry = footer_msg.add_chunk_index();
        proto_entry->set_chunk_index(entry.chunk_index);
        proto_entry->set_start_frame(entry.start_frame);
        proto_entry->set_end_frame(entry.end_frame);
        proto_entry->set_file_offset(entry.file_offset);
        proto_entry->set_chunk_size_bytes(entry.chunk_size_bytes);
    }

    std::string payload;
    if (!footer_msg.SerializeToString(&payload)) {
         throw std::runtime_error("VTX [ProtobufPolicy]: Failed to serialize Footer.");
    }
    
    return payload;
}
