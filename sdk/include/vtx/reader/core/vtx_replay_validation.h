/**
 * @file vtx_replay_validation.h
 * @brief Whole-replay validation entry points (reader layer).
 *
 * @details Validates a replay's embedded schema and its frames, returning one
 * aggregated ValidationReport.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <string>

#include "vtx/common/vtx_diagnostics.h"

namespace VTX {

    class IVtxReaderFacade;

    struct ReplayValidationOptions {
        bool validate_schema = true;
        bool validate_frames = true;
        int32_t max_frames = -1; ///-1 = all frames; otherwise cap the count.
    };

    /**
     * @brief Validate an already-open replay (no file I/O).
     * @param reader A reader for the replay; this waits for readiness and reports
     *        ReplayNotReady if it never becomes ready.
     * @details Reports SchemaMissing (warning) when there is no embedded schema,
     * plus any schema and per-frame diagnostics.
     */
    ValidationReport ValidateReplay(IVtxReaderFacade& reader, const ReplayValidationOptions& options = {});

    /**
     * @brief Open @p filepath and validate it.
     * @details Thin convenience wrapper that opens the file (reporting
     * ReplayOpenFailed on failure) and delegates to ValidateReplay(reader).
     */
    ValidationReport ValidateReplayFile(const std::string& filepath, const ReplayValidationOptions& options = {});

} // namespace VTX
