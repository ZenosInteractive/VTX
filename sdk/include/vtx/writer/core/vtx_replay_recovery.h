/**
 * @file vtx_replay_recovery.h
 * @brief Repair a .vtx file whose writing crashed before the footer was written.
 *
 * @details When the writer's recovery journal is enabled, a sidecar "<file>.recovery"
 * records every committed chunk. If the process crashes before Close() writes the
 * footer (and deletes the sidecar), RepairReplayFile() reconstructs a valid footer
 * from the journal: it drops any torn tail chunk, verifies each surviving chunk's
 * checksum, appends a synthesized footer, and removes the sidecar. The result is a
 * normal .vtx that the standard reader opens with no special handling.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <string>

namespace VTX {

    struct RepairResult {
        bool was_clean = false;         ///< No recovery journal present -> nothing to repair.
        bool repaired = false;          ///< A footer was reconstructed and written.
        int32_t recovered_chunks = 0;   ///< Chunks preserved in the recovered file.
        int32_t recovered_frames = 0;   ///< total_frames of the recovered file.
        std::string error;              ///< Non-empty on failure.

        bool ok() const { return error.empty(); }
    };

    /**
     * @brief Recover a crashed .vtx using its "<path>.recovery" journal.
     * @param path Path to the (footerless) main .vtx file.
     * @return Outcome. If no journal exists, was_clean is true and nothing is changed.
     */
    RepairResult RepairReplayFile(const std::string& path);

} // namespace VTX
