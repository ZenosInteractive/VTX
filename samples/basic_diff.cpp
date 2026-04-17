// basic_diff.cpp -- Structural diff between two consecutive frames.
//
// Uses the real VtxDiff::IVtxDifferFacade.  The differ takes the raw
// serialized frame bytes (one span per frame) and walks both binary trees in
// parallel, emitting Add / Remove / Replace / ReplaceRange operations keyed
// by binary path and entity unique_id.
//
// Default input
//   content/reader/arena/arena_from_fbs_ds.vtx
//   (produced by: vtx_sample_generate -> vtx_sample_advance_write)
//
// Build
//   Link against vtx_reader and vtx_differ (vtx_common is transitive).

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/differ/core/vtx_differ_facade.h"
#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/common/vtx_types.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace {

const char* OpName(VtxDiff::DiffOperation op) {
    switch (op) {
        case VtxDiff::DiffOperation::Add:          return "Add";
        case VtxDiff::DiffOperation::Remove:       return "Remove";
        case VtxDiff::DiffOperation::Replace:      return "Replace";
        case VtxDiff::DiffOperation::ReplaceRange: return "ReplaceRange";
    }
    return "?";
}

std::string PathToString(const VtxDiff::DiffIndexPath& p) {
    std::string s = "[";
    for (size_t i = 0; i < p.count; ++i) {
        if (i > 0) s += ",";
        s += std::to_string(p.indices[i]);
    }
    s += "]";
    return s;
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string filepath = (argc > 1)
        ? argv[1]
        : "content/reader/arena/arena_from_fbs_ds.vtx";

    // ---- Open the replay -------------------------------------------------
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

    // ---- Create a differ matching the wire format ------------------------
    auto differ = VtxDiff::CreateDifferFacade(result.format);
    if (!differ) {
        VTX_ERROR("Unsupported format for differ.");
        return 1;
    }

    // ---- Grab raw bytes of frames 0 and 1 --------------------------------
    // Copy frame A's bytes because loading frame B may evict A's chunk from
    // the reader's sliding-window cache.
    std::span<const std::byte> raw_a = reader->GetRawFrameBytes(0);
    if (raw_a.empty()) {
        VTX_ERROR("Frame 0 raw bytes unavailable.");
        return 1;
    }
    std::vector<std::byte> bytes_a(raw_a.begin(), raw_a.end());

    std::span<const std::byte> raw_b = reader->GetRawFrameBytes(1);
    if (raw_b.empty()) {
        VTX_ERROR("Frame 1 raw bytes unavailable.");
        return 1;
    }

    // ---- Run the diff ----------------------------------------------------
    VtxDiff::DiffOptions opts;
    opts.compare_floats_with_epsilon = true;
    opts.float_epsilon               = 1e-5f;

    VtxDiff::PatchIndex patch = differ->DiffRawFrames(bytes_a, raw_b, opts);

    // ---- Report ----------------------------------------------------------
    VTX_INFO("Diff frame 0 -> 1: {} operation(s)", patch.operations.size());

    // Show the first few ops in detail; summarise the rest by container type.
    constexpr size_t kDetailLimit = 20;
    for (size_t i = 0; i < patch.operations.size() && i < kDetailLimit; ++i) {
        const auto& op = patch.operations[i];
        VTX_INFO("  {:>12} container={} path={} actor='{}'",
                 OpName(op.Operation),
                 VtxDiff::TypeToFieldName(op.ContainerType),
                 PathToString(op.Path),
                 op.ActorId);
    }
    if (patch.operations.size() > kDetailLimit) {
        VTX_INFO("  ... {} more operation(s) suppressed",
                 patch.operations.size() - kDetailLimit);
    }

    return 0;
}
