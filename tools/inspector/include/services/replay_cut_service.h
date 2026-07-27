#pragma once

#include <cstdint>
#include <string>

#include "vtx/common/vtx_types.h"

namespace VtxServices {

    // Resolved cut request. Interior chunks are always copied verbatim; when the
    // requested range starts or ends inside a chunk (trims_head/trims_tail) that
    // edge chunk is re-serialized with only the kept frames, and its seek-table
    // entry (size, checksum, frame range) is regenerated.
    struct ReplayCutPlan {
        bool valid = false;
        std::string error; // set when !valid

        int first_chunk = 0; // index into footer.chunk_index
        int last_chunk = 0;
        int first_frame = 0; // exact kept range, original frame numbering
        int last_frame = 0;
        bool trims_head = false;  // first chunk starts before first_frame
        bool trims_tail = false;  // last chunk ends after last_frame
        uint64_t chunk_bytes = 0; // on-disk bytes of the involved source chunks
    };

    // Cuts a new .vtx out of an existing flatbuffers replay: header block copied
    // verbatim, whole chunks copied verbatim, partial edge chunks rewritten, and
    // the footer rebuilt (frame numbering rebased to 0, time table sliced,
    // duration recomputed). Timeline events are not carried over.
    class ReplayCutService {
    public:
        // Exact cut: keeps [start_frame, end_frame] inclusive, trimming inside
        // the edge chunks when the bounds do not fall on chunk boundaries.
        static ReplayCutPlan PlanCutFrames(const VTX::FileFooter& footer, int start_frame, int end_frame);

        // Whole-chunk cut: keeps chunks [first_chunk, last_chunk] untouched.
        static ReplayCutPlan PlanCutChunks(const VTX::FileFooter& footer, int first_chunk, int last_chunk);

        // Writes the cut to dest_path. Returns false and fills `error` on failure.
        // dest_path must differ from source_path. When the plan trims an edge
        // chunk, the source is reopened with the SDK reader to re-serialize it.
        static bool ExecuteCut(const std::string& source_path, const VTX::FileFooter& footer, const ReplayCutPlan& plan,
                               const std::string& dest_path, std::string& error);
    };

} // namespace VtxServices
