// basic_read.cpp — Open a .vtx replay file and print frame/entity info.
//
// Build:
//   Link against vtx_reader and vtx_common.
//   See samples/CMakeLists.txt for a full example.

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_types.h"

#include <string>

int main(int argc, char* argv[])
{
    const std::string filepath = (argc > 1)
        ? argv[1]
        : "content/reader/rl/rl_proto.vtx";


    auto result = VTX::OpenReplayFile(filepath);
    if (!result) {
        VTX_ERROR("Failed to open: {}", result.error);
        return 1;
    }

    auto& reader = result.reader;

    // --- Header ---
    VTX::FileHeader header = reader->GetHeader();
    VTX_INFO("Replay : {}", header.replay_name);
    VTX_INFO("UUID   : {}", header.replay_uuid);
    VTX_INFO("Format : {}", (result.format == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf"));

    // --- Footer (summary) ---
    VTX::FileFooter footer = reader->GetFooter();
    VTX_INFO("Frames : {}", footer.total_frames);
    VTX_INFO("Duration: {} s", footer.duration_seconds);
    VTX_INFO("Chunks : {}", footer.chunk_index.size());
    VTX_INFO("Events : {}", footer.events.size());

    // --- Read first frame ---
    const VTX::Frame* frame = reader->GetFrameSync(0);
    if (!frame) {
        VTX_ERROR("Could not read frame 0");
        return 1;
    }

    VTX_INFO("Frame 0 buckets: {}", frame->GetBuckets().size());
    for (const auto& [name, idx] : frame->bucket_map) {
        const auto& bucket = frame->GetBuckets()[idx];
        VTX_INFO("  [{}] entities: {}", name, bucket.entities.size());
    }

    // --- Schema and property access ---
    VTX::ContextualSchema schema = reader->GetContextualSchema();
    VTX_INFO("Schema: {}", schema.data_identifier);

    return 0;
}
