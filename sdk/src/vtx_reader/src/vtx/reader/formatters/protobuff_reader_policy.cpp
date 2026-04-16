#include "vtx/reader/policies/formatters/protobuff_reader_policy.h"
#include <string>
#include <vector>
#include "vtx_schema.pb.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/reader/serialization/proto_to_vtx.h"
#include "vtx/reader/core/vtx_deserializer_service.h"

std::string VTX::ProtobufReaderPolicy::GetMagicBytes() {
    return "VTXP";
}

VTX::ProtobufReaderPolicy::HeaderType VTX::ProtobufReaderPolicy::ParseHeader(const std::string& buffer) {
    HeaderType header;
    if (!header.ParseFromString(buffer)) {
        throw std::runtime_error("VTX Reader [Proto]: Failed to parse FileHeader.");
    }
    return header;
}

 VTX::ProtobufReaderPolicy::FooterType VTX::ProtobufReaderPolicy::ParseFooter(const std::string& buffer) {
    FooterType footer;
    if (!footer.ParseFromString(buffer)) {
        throw std::runtime_error("VTX Reader [Proto]: Failed to parse FileFooter.");
    }
    return footer;
}

void VTX::ProtobufReaderPolicy::ProcessChunkData(int chunk_index,const std::string& compressed, std::stop_token st,std::vector<VTX::Frame>& out_native_frames,std::vector<uint8_t>& out_decompressed_blob,std::vector<std::span<const std::byte>>& out_raw_frames_spans)
 {
    std::string decompressed = VTX::ReplayUnpacker::Decompress(compressed);

    cppvtx::FrameChunk proto;
    proto.ParseFromString(decompressed);

    size_t num_frames = proto.frames_size();
    out_native_frames.resize(num_frames);
    out_raw_frames_spans.reserve(num_frames);

    //precalculate data size
    size_t total_raw_size = 0;
    for(int i = 0; i < num_frames; ++i) {
        total_raw_size += proto.frames(i).data(0).ByteSizeLong();
    }
    out_decompressed_blob.reserve(total_raw_size);

    std::vector<size_t> frame_sizes;
    frame_sizes.reserve(num_frames);

    //serialize and concatenate frmaes in a single block
    for(int i = 0; i < num_frames; ++i) {
        if (st.stop_requested()) return;
        Serialization::FromProto(proto.frames(i), out_native_frames[i]);

        // Eextract root for the differ
        std::string rawBytes = proto.frames(i).data(0).SerializeAsString();
        out_decompressed_blob.insert(out_decompressed_blob.end(), rawBytes.begin(), rawBytes.end());
        frame_sizes.push_back(rawBytes.size());
    }

    // Genrate single frame spana
    // done here because if we do it before , if vector grows pointers are invalid
    size_t offset = 0;
    for (size_t size : frame_sizes) {
        out_raw_frames_spans.emplace_back(
            reinterpret_cast<const std::byte*>(out_decompressed_blob.data() + offset),
            size
        );
        offset += size;
    }
}

void VTX::ProtobufReaderPolicy::PopulateIndexTable(const FooterType& footer,std::vector<ChunkIndexEntry>& chunk_index_table)
{
    chunk_index_table.reserve(footer.chunk_index_size());

    for (const auto& protoEntry : footer.chunk_index()) {
        ChunkIndexEntry nativeEntry;
        nativeEntry.chunk_index     = protoEntry.chunk_index();
        nativeEntry.start_frame     = protoEntry.start_frame();
        nativeEntry.end_frame       = protoEntry.end_frame();
        nativeEntry.file_offset     = protoEntry.file_offset();
        nativeEntry.chunk_size_bytes = protoEntry.chunk_size_bytes();

        chunk_index_table.push_back(nativeEntry);
    }
}

void VTX::ProtobufReaderPolicy::PopulateGameTimes(const VTX::ProtobufReaderPolicy::FooterType& footer, VTX::GameTime::VTXGameTimes& game_times)
{
    const auto& times_proto = footer.times();
    game_times.SetGameTime({times_proto.game_time().begin(), times_proto.game_time().end()});
    game_times.SetCreatedUtc({times_proto.created_utc().begin(), times_proto.created_utc().end()});
    game_times.SetTimelineGaps({times_proto.gaps().begin(), times_proto.gaps().end()});
    game_times.SetGameSegments({times_proto.segments().begin(), times_proto.segments().end()});
}

int32_t VTX::ProtobufReaderPolicy::GetTotalFrames(const VTX::ProtobufReaderPolicy::FooterType& footer) {
    return footer.total_frames();
}

const VTX::ProtobufReaderPolicy::SchemaType& VTX::ProtobufReaderPolicy::GetSchema(const HeaderType& header) {
    return header.prop_schema();
}

VTX::ContextualSchema VTX::ProtobufReaderPolicy::GetVTXContextualSchema(const HeaderType& header)
{
    VTX::ContextualSchema contextual_schema;
    contextual_schema.data_identifier = header.prop_schema().data_indentifier();
    contextual_schema.data_version = header.prop_schema().data_version();
    contextual_schema.data_version_string = header.prop_schema().data_version_string();
    contextual_schema.property_mapping = header.prop_schema().schema();
    return contextual_schema;
}

VTX::FileHeader VTX::ProtobufReaderPolicy::GetVTXHeader(const HeaderType& pb_header)
{
    VTX::FileHeader out;
    out.replay_name = pb_header.replay_name();
    out.replay_uuid = pb_header.replay_uuid();
    out.recorded_utc_timestamp = pb_header.recorded_utc_timestamp();
    out.custom_json_metadata = pb_header.custom_json_metadata();

    if (pb_header.has_version()) {
        out.version.format_major = pb_header.version().format_major();
        out.version.format_minor = pb_header.version().format_minor();
        out.version.schema_version = pb_header.version().schema_version();
    }

    return out;
}

VTX::FileFooter VTX::ProtobufReaderPolicy::GetVTXFooter(const FooterType& pb_footer)
{
    VTX::FileFooter out;
    out.total_frames = pb_footer.total_frames();
    out.duration_seconds = pb_footer.duration_seconds();
    out.payload_checksum = pb_footer.payload_checksum();

    out.chunk_index.reserve(pb_footer.chunk_index_size());
    for (int i = 0; i < pb_footer.chunk_index_size(); ++i) {
        const auto& pb_chunk = pb_footer.chunk_index(i);
        VTX::ChunkIndexEntry entry;

        entry.chunk_index = pb_chunk.chunk_index();
        entry.start_frame = pb_chunk.start_frame();
        entry.end_frame = pb_chunk.end_frame();
        entry.file_offset = pb_chunk.file_offset();
        entry.chunk_size_bytes = pb_chunk.chunk_size_bytes();

        out.chunk_index.push_back(entry);
    }

    out.events.reserve(pb_footer.events_size());
    for (int i = 0; i < pb_footer.events_size(); ++i) {
        const auto& pb_ev = pb_footer.events(i);
        VTX::TimelineEvent out_ev;

        out_ev.game_time = pb_ev.game_time();
        out_ev.event_type = pb_ev.event_type();
        out_ev.label = pb_ev.label();
        out_ev.entity_unique_id = pb_ev.entity_unique_id();


        if (pb_ev.has_location()) {
            out_ev.location.x = pb_ev.location().x();
            out_ev.location.y = pb_ev.location().y();
            out_ev.location.z = pb_ev.location().z();
        }

        out.events.push_back(out_ev);
    }

    if (pb_footer.has_times()) {
        const auto& pb_times = pb_footer.times();
        out.times.game_time.assign(pb_times.game_time().begin(), pb_times.game_time().end());
        out.times.created_utc.assign(pb_times.created_utc().begin(), pb_times.created_utc().end());
        out.times.gaps.assign(pb_times.gaps().begin(), pb_times.gaps().end());
        out.times.segments.assign(pb_times.segments().begin(), pb_times.segments().end());
    }

    return out;
}
