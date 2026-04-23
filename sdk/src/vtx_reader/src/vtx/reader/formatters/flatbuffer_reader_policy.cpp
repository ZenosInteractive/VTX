
#include "vtx/reader/policies/formatters/flatbuffer_reader_policy.h"
#include <string>
#include <vector>

#include "vtx_schema_generated.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/serialization/flatbuffers_to_vtx.h"
#include "vtx/reader/core/vtx_deserializer_service.h"


VTX::FlatBuffersReaderPolicy::HeaderType VTX::FlatBuffersReaderPolicy::ParseHeader(const std::string& buffer) {
    HeaderType header;
    auto* root = fbsvtx::GetFileHeader(buffer.data());
    if (root) {
        root->UnPackTo(&header);
    }
    return header;
}

std::string VTX::FlatBuffersReaderPolicy::GetMagicBytes() {
    return "VTXF";
}

VTX::FlatBuffersReaderPolicy::FooterType VTX::FlatBuffersReaderPolicy::ParseFooter(const std::string& buffer) {
    FooterType footer;
    auto* root = flatbuffers::GetRoot<fbsvtx::FileFooter>(buffer.data());
    if (root) {
        root->UnPackTo(&footer);
    }
    return footer;
}

void VTX::FlatBuffersReaderPolicy::ProcessChunkData(int chunk_index, const std::string& compressed, std::stop_token st,
                                                    std::vector<VTX::Frame>& out_native_frames,
                                                    std::vector<uint8_t>& out_decompressed_blob,
                                                    std::vector<std::span<const std::byte>>& out_raw_frames_spans) {
    std::string decompressed = VTX::ReplayUnpacker::Decompress(compressed);
    auto* chunk = flatbuffers::GetRoot<fbsvtx::Chunk>(decompressed.data());

    if (!chunk || !chunk->frames())
        return;

    size_t num_frames = chunk->frames()->size();
    out_native_frames.resize(num_frames);
    out_raw_frames_spans.reserve(num_frames);

    flatbuffers::FlatBufferBuilder fbb;
    std::vector<size_t> frame_sizes;
    frame_sizes.reserve(num_frames);

    //serialize and concatenate frmaes in a single block
    for (size_t i = 0; i < num_frames; ++i) {
        if (st.stop_requested())
            return;
        VTX::Serialization::FromFlat(chunk->frames()->Get(i), out_native_frames[i]);

        //pack for differ
        auto* data_obj = chunk->frames()->Get(i)->data()->Get(0);
        std::unique_ptr<fbsvtx::BucketT> dataT(data_obj->UnPack());

        fbb.Clear();
        fbb.Finish(fbsvtx::Bucket::Pack(fbb, dataT.get()));

        out_decompressed_blob.insert(out_decompressed_blob.end(), fbb.GetBufferPointer(),
                                     fbb.GetBufferPointer() + fbb.GetSize());
        frame_sizes.push_back(fbb.GetSize());
    }

    //genrate spans for each frame, zero copy
    size_t offset = 0;
    for (size_t size : frame_sizes) {
        out_raw_frames_spans.emplace_back(reinterpret_cast<const std::byte*>(out_decompressed_blob.data() + offset),
                                          size);
        offset += size;
    }
}

void VTX::FlatBuffersReaderPolicy::PopulateIndexTable(const FooterType& footer,
                                                      std::vector<ChunkIndexEntry>& chunk_index_table) {
    chunk_index_table.reserve(footer.chunk_index.size());

    for (const auto& e : footer.chunk_index) {
        if (!e)
            continue;

        ChunkIndexEntry ce;
        ce.chunk_index = e->chunk_index;
        ce.start_frame = e->start_frame;
        ce.end_frame = e->end_frame;
        ce.file_offset = e->file_offset;
        ce.chunk_size_bytes = e->chunk_size_bytes;

        chunk_index_table.push_back(ce);
    }
}

void VTX::FlatBuffersReaderPolicy::PopulateGameTimes(const FooterType& footer,
                                                     VTX::GameTime::VTXGameTimes& game_times) {
    if (footer.times) {
        const auto& t = *footer.times;
        game_times.SetGameTime(t.game_time);
        game_times.SetCreatedUtc(t.created_utc);
        game_times.SetTimelineGaps(t.gaps);
        game_times.SetGameSegments(t.segments);
    }
}

int32_t VTX::FlatBuffersReaderPolicy::GetTotalFrames(const FooterType& footer) {
    return footer.total_frames;
}

const VTX::FlatBuffersReaderPolicy::SchemaType& VTX::FlatBuffersReaderPolicy::GetSchema(const HeaderType& header) {
    if (header.contextual_schema) {
        return *header.contextual_schema;
    }
    static SchemaType dummy;
    return dummy;
}

VTX::ContextualSchema VTX::FlatBuffersReaderPolicy::GetVTXContextualSchema(const HeaderType& header) {
    VTX::ContextualSchema contextual_schema;
    contextual_schema.data_identifier = header.contextual_schema->data_identifier;
    contextual_schema.data_version = header.contextual_schema->data_version;
    contextual_schema.data_version_string = header.contextual_schema->data_version_string;
    contextual_schema.property_mapping = header.contextual_schema->schema;
    return contextual_schema;
}

VTX::FileHeader VTX::FlatBuffersReaderPolicy::GetVTXHeader(const HeaderType& fbs_header) {
    VTX::FileHeader out;
    out.replay_name = fbs_header.replay_name;
    out.replay_uuid = fbs_header.replay_uuid;
    out.recorded_utc_timestamp = fbs_header.recorded_utc_timestamp;
    out.custom_json_metadata = fbs_header.custom_json_metadata;

    if (fbs_header.version) {
        out.version.format_major = fbs_header.version->format_major;
        out.version.format_minor = fbs_header.version->format_minor;
        out.version.schema_version = fbs_header.version->schema_version;
    }
    return out;
}

VTX::FileFooter VTX::FlatBuffersReaderPolicy::GetVTXFooter(const FooterType& fbs_footer) {
    VTX::FileFooter out;
    out.total_frames = fbs_footer.total_frames;
    out.duration_seconds = fbs_footer.duration_seconds;
    out.payload_checksum = fbs_footer.payload_checksum;

    out.chunk_index.reserve(fbs_footer.chunk_index.size());
    for (const auto& chunk : fbs_footer.chunk_index) {
        if (chunk) {
            VTX::ChunkIndexEntry entry;
            entry.chunk_index = chunk->chunk_index;
            entry.chunk_size_bytes = chunk->chunk_size_bytes;
            entry.start_frame = chunk->start_frame;
            entry.end_frame = chunk->end_frame;

            entry.file_offset = chunk->file_offset;
            out.chunk_index.push_back(entry);
        }
    }

    out.events.reserve(fbs_footer.events.size());
    for (const auto& ev : fbs_footer.events) {
        if (ev) {
            VTX::TimelineEvent out_ev;
            out_ev.game_time = ev->game_time;
            out_ev.event_type = ev->event_type;
            out_ev.label = ev->label;
            out.events.push_back(out_ev);
        }
    }

    if (fbs_footer.times) {
        const auto& fbs_times = fbs_footer.times;
        out.times.game_time.assign(fbs_times->game_time.begin(), fbs_times->game_time.end());
        out.times.created_utc.assign(fbs_times->created_utc.begin(), fbs_times->created_utc.end());
        out.times.gaps.assign(fbs_times->gaps.begin(), fbs_times->gaps.end());
        out.times.segments.assign(fbs_times->segments.begin(), fbs_times->segments.end());
    }
    return out;
}
