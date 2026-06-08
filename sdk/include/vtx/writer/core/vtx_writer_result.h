/**
 * @file vtx_writer_result.h
 * @brief Per-frame and per-pipeline outcome types for the writer.

 * @details Makes frame rejection observable.
 * @author Zenos Interactive
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace VTX {

    /**
     * @brief Why a frame did not enter the chunk pipeline.
     */
    enum class FrameRejectReason {
        None = 0,
        GameTimeRejected,
        ValidationFailed,
    };

    inline const char* ToString(FrameRejectReason reason) {
        switch (reason) {
        case FrameRejectReason::None:
            return "None";
        case FrameRejectReason::GameTimeRejected:
            return "GameTimeRejected";
        case FrameRejectReason::ValidationFailed:
            return "ValidationFailed";
        }
        return "Unknown";
    }

    /**
     * @brief Outcome of a single TryRecordFrame() call.
     */
    struct RecordResult {
        bool written = false;
        FrameRejectReason reason = FrameRejectReason::None;
        std::string detail;
        int32_t frame_index = -1;

        bool IsWritten() const { return written; }

        static RecordResult MadeWritten(int32_t index) {
            RecordResult r;
            r.written = true;
            r.reason = FrameRejectReason::None;
            r.frame_index = index;
            return r;
        }

        static RecordResult MadeRejected(FrameRejectReason reason, std::string detail) {
            RecordResult r;
            r.written = false;
            r.reason = reason;
            r.detail = std::move(detail);
            return r;
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

        size_t Total() const { return written + rejected + skipped; }

        void Account(const RecordResult& result) {
            if (result.written) {
                ++written;
                return;
            }
            ++rejected;
            if (result.reason == FrameRejectReason::ValidationFailed) {
                ++validation_errors;
            } else if (result.reason == FrameRejectReason::GameTimeRejected) {
                ++timer_errors;
            }
        }
    };

} // namespace VTX
