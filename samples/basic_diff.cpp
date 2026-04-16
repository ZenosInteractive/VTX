// basic_diff.cpp — Open a replay and diff two consecutive frames.
//
// Build:
//   Link against vtx_reader, vtx_differ, and vtx_common.
//   See samples/CMakeLists.txt for a full example.

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/differ/core/vtx_diff_types.h"
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

    if (reader->GetTotalFrames() < 2) {
        VTX_WARN("Need at least 2 frames to diff.");
        return 1;
    }

    // Read two consecutive frames.
    const VTX::Frame* frame_a = reader->GetFrameSync(0);
    const VTX::Frame* frame_b = reader->GetFrameSync(1);
    if (!frame_a || !frame_b) {
        VTX_ERROR("Could not read frames.");
        return 1;
    }

    // Compare bucket counts.
    VTX_INFO("Frame 0 buckets: {}", frame_a->GetBuckets().size());
    VTX_INFO("Frame 1 buckets: {}", frame_b->GetBuckets().size());

    // Walk each bucket and compare entity counts.
    for (const auto& [name, idx] : frame_a->bucket_map) {
        const auto& bucket_a = frame_a->GetBuckets()[idx];

        auto it_b = frame_b->bucket_map.find(name);
        if (it_b == frame_b->bucket_map.end()) {
            VTX_WARN("[{}] removed in frame 1", name);
            continue;
        }

        const auto& bucket_b = frame_b->GetBuckets()[it_b->second];

        // Quick content hash comparison per entity.
        int changed = 0;
        size_t common = std::min(bucket_a.entities.size(), bucket_b.entities.size());
        for (size_t i = 0; i < common; ++i) {
            if (bucket_a.entities[i].content_hash != bucket_b.entities[i].content_hash) {
                ++changed;
            }
        }

        int added   = static_cast<int>(bucket_b.entities.size()) - static_cast<int>(common);
        int removed = static_cast<int>(bucket_a.entities.size()) - static_cast<int>(common);

        VTX_INFO("[{}] entities: {} -> {} | changed: {}", name,
                 bucket_a.entities.size(), bucket_b.entities.size(), changed);
        if (added > 0)   VTX_INFO("  added: {}", added);
        if (removed > 0) VTX_INFO("  removed: {}", removed);
    }

    return 0;
}
