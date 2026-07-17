/**
 * @file vtx_replay_recovery.h
 * @brief Repair a .vtx file whose writing crashed before the footer was written.
 *
 * @details When the writer's recovery journal is enabled, a sidecar "<file>.recovery"
 * records every committed chunk and every in-flight frame. If the process crashes
 * before Close() writes the footer (and deletes the sidecar), RepairReplayFile()
 * reconstructs a valid footer from the journal: it drops any torn tail chunk, verifies
 * each surviving chunk's checksum, re-appends the in-flight frames, rebuilds the exact
 * per-frame times, and removes the sidecar. The result is a normal .vtx that the
 * standard reader opens with no special handling.
 *
 * Recovery is deliberately NOT automatic: opening a replay never repairs it behind the
 * caller's back. The intended flow is user-driven --
 *
 *   if (VTX::ReplayNeedsRecovery(path)) {
 *       const auto r = VTX::RepairReplayFile(path);
 *       // inspect r (was_clean / repaired / recovered_frames / error) and decide
 *   }
 *   auto ctx = VTX::OpenReplayFile(path);
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <string>

namespace VTX {

    struct RepairResult {
        bool was_clean = false;       ///< No recovery journal present -> nothing to repair.
        bool repaired = false;        ///< A footer was reconstructed and written.
        int32_t recovered_chunks = 0; ///< Chunks preserved in the recovered file.
        int32_t recovered_frames = 0; ///< total_frames of the recovered file.
        std::string error;            ///< Non-empty on failure.

        bool ok() const { return error.empty(); }
    };

    /**
     * @brief The recovery sidecar path for a given main .vtx path ("<path>.recovery").
     * @details Lets callers locate / inspect / delete the sidecar without depending on
     *          the internal journal header.
     */
    std::string RecoveryJournalPath(const std::string& path);

    /**
     * @brief Cheap check for whether @p path was left by an unclean shutdown.
     * @return true if the "<path>.recovery" sidecar exists. A leftover sidecar over an
     *         already-complete file still returns true here; RepairReplayFile() makes the
     *         final determination (and reports was_clean if the footer was in fact intact).
     */
    bool ReplayNeedsRecovery(const std::string& path);

    /**
     * @brief Recover a crashed .vtx using its "<path>.recovery" journal.
     * @param path Path to the (footerless) main .vtx file.
     * @return Outcome. If no journal exists, was_clean is true and nothing is changed.
     */
    RepairResult RepairReplayFile(const std::string& path);

} // namespace VTX
