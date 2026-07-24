#pragma once

#include <cstdint>
#include <string>

#include "vtx/common/vtx_types.h"

namespace VtxServices {

    // Resolved cut request, snapped to chunk boundaries. Chunks are copied
    // verbatim (no rebalancing), so the kept range is the union of the chunks
    // containing the requested first and last frame.
    struct ReplayCutPlan {
        bool valid = false;
        std::string error; // set when !valid

        int first_chunk = 0; // index into footer.chunk_index
        int last_chunk = 0;
        int first_frame = 0; // snapped range, original frame numbering
        int last_frame = 0;
        uint64_t chunk_bytes = 0; // total on-disk bytes of the kept chunks
    };

    // Cuts a new .vtx out of an existing flatbuffers replay: header block copied
    // verbatim, kept chunks copied verbatim, footer rebuilt (frame numbering
    // rebased to 0, time table sliced, duration recomputed). Timeline events are
    // not carried over.
    class ReplayCutService {
    public:
        // Snaps [start_frame, end_frame] (original numbering, inclusive) to the
        // chunks that contain them.
        static ReplayCutPlan PlanCut(const VTX::FileFooter& footer, int start_frame, int end_frame);

        // Writes the cut to dest_path. Returns false and fills `error` on failure.
        // dest_path must differ from source_path.
        static bool ExecuteCut(const std::string& source_path, const VTX::FileFooter& footer, const ReplayCutPlan& plan,
                               const std::string& dest_path, std::string& error);
    };

} // namespace VtxServices
