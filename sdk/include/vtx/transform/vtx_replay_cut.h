/**
 * @file vtx_replay_cut.h
 * @brief Write a copy of a .vtx replay holding only a sub-range of its frames.
 *
 * @details CutReplayFile() slices a finished replay in time: the header block is
 * copied byte for byte, chunks fully inside the range are copied verbatim, the two
 * edge chunks are re-serialized with only the kept frames when the range starts or
 * ends inside them, and the footer is rebuilt -- frame numbering rebased to 0, the
 * per-frame time table sliced (tick values stay absolute), gaps and segments
 * filtered and rebased, duration recomputed from the kept stamps. Timeline events
 * are not carried over.
 *
 *   const VTX::ReplayCutPlan plan = VTX::PlanCutFrames(footer, 1200, 1799);
 *   if (plan.valid) {
 *       const auto r = VTX::CutReplayFile("match.vtx", footer, plan, "match_round2.vtx");
 *       if (!r.ok()) { ... r.error ... }
 *   }
 *
 * The plan is computed from the source footer so a UI can preview the resolved
 * chunks/frames before anything is written; CutReplayFile() executes that plan
 * against the same footer. Both FlatBuffers and Protobuf files are supported and
 * the output keeps the source format. Its sibling FilterReplayFile() slices *what*
 * is in every frame instead.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <string>

#include "vtx/common/vtx_types.h"

namespace VTX {

    /**
     * @brief Resolved cut request.
     * @details Interior chunks are copied verbatim; when the range starts or ends
     *          inside a chunk (trims_head / trims_tail) that edge chunk is
     *          re-serialized with only the kept frames and gets a fresh seek-table
     *          entry (size, checksum, frame range).
     */
    struct ReplayCutPlan {
        bool valid = false;
        std::string error; ///< Set when !valid.

        int32_t first_chunk = 0; ///< Index into footer.chunk_index.
        int32_t last_chunk = 0;
        int32_t first_frame = 0; ///< Exact kept range, source frame numbering.
        int32_t last_frame = 0;
        bool trims_head = false;  ///< first_chunk starts before first_frame.
        bool trims_tail = false;  ///< last_chunk ends after last_frame.
        uint64_t chunk_bytes = 0; ///< On-disk bytes of the involved source chunks.

        int32_t FrameCount() const { return valid ? last_frame - first_frame + 1 : 0; }
    };

    /// Exact cut: keeps [start_frame, end_frame] inclusive, trimming inside the edge
    /// chunks when the bounds do not fall on chunk boundaries. Bounds are clamped to
    /// the replay; a start that falls in a gap between chunks snaps to the next frame.
    ReplayCutPlan PlanCutFrames(const FileFooter& footer, int32_t start_frame, int32_t end_frame);

    /// Whole-chunk cut: keeps chunks [first_chunk, last_chunk] untouched.
    ReplayCutPlan PlanCutChunks(const FileFooter& footer, int32_t first_chunk, int32_t last_chunk);

    struct ReplayCutResult {
        std::string error; ///< Non-empty on failure (the destination is removed).

        int32_t total_frames = 0;     ///< Frames in the output.
        int32_t chunks_written = 0;   ///< Chunks in the output.
        int32_t chunks_rewritten = 0; ///< Edge chunks re-serialized (0, 1 or 2).
        uint64_t output_bytes = 0;

        bool ok() const { return error.empty(); }
    };

    /**
     * @brief Writes @p dest_path as the cut of @p source_path described by @p plan.
     * @param footer The source footer the plan was computed from.
     * @details The format is taken from the source's magic bytes. When the plan trims
     *          an edge chunk the source is reopened with the standard reader to
     *          re-serialize it; whole-chunk cuts never decode a frame. dest_path must
     *          differ from source_path.
     */
    ReplayCutResult CutReplayFile(const std::string& source_path, const FileFooter& footer, const ReplayCutPlan& plan,
                                  const std::string& dest_path);

} // namespace VTX
