/**
 * @file vtx_writer_result.h
 * @brief Per-frame and per-pipeline outcome types for the writer.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "vtx/common/vtx_diagnostics.h"

namespace VTX {

    /**
     * @brief Outcome of a single TryRecordFrame() call.
     */
    struct RecordResult {
        bool written = false;
        VtxError error {};
        uint32_t frame_index = -1;
        bool IsWritten() const { return written; }

        static RecordResult MadeWritten(int32_t index) {
            RecordResult r;
            r.written = true;
            r.frame_index = index;
            return r;
        }

        static RecordResult MadeRejected(VtxError error) {
            RecordResult r;
            r.written = false;
            r.error = std::move(error);
            return r;
        }

        static RecordResult MadeRejected(VtxErrorCode code, std::string message,
                                         const char* source_api = "TryRecordFrame") {
            VtxError error;
            error.code = code;
            error.severity = Severity::Error;
            error.message = std::move(message);
            error.source_api = source_api;
            return MadeRejected(std::move(error));
        }
    };

    /**
     * @brief Aggregated outcome of a one-call RecordPipeline::Run().
     */
    struct PipelineReport {
        size_t written = 0;
        size_t rejected = 0;
        size_t skipped = 0;
        size_t validation_errors = 0;
        size_t timer_errors = 0;
        std::vector<VtxError> errors;

        size_t Total() const { return written + rejected + skipped; }

        void Account(const RecordResult& result) {
            if (result.written) {
                ++written;
                return;
            }
            ++rejected;
            if (result.error.code == VtxErrorCode::GameTimeRejected) {
                ++timer_errors;
            } else {
                ++validation_errors;
            }
            errors.push_back(result.error);
        }
    };

} // namespace VTX
