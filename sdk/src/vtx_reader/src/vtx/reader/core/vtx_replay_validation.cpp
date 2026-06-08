/**
 * @file vtx_replay_validation.cpp
 * @brief Implementation of ValidateReplay / ValidateReplayFile (reader layer).
 * @author Zenos Interactive
 */
#include "vtx/reader/core/vtx_replay_validation.h"

#include <algorithm>

#include "vtx/common/vtx_validation.h"
#include "vtx/reader/core/vtx_reader_facade.h"

namespace VTX {

    ValidationReport ValidateReplay(IVtxReaderFacade& reader, const ReplayValidationOptions& options) {
        ValidationReport report;

        if (!reader.WaitUntilReady()) {
            // The reader already reports the failure as a structured VtxError;
            VtxError error = reader.GetReadyError();
            if (error.code == VtxErrorCode::None) {
                error.code = VtxErrorCode::ReplayNotReady;
                error.severity = Severity::Error;
                error.message = "replay did not become ready";
            }
            error.source_api = "ValidateReplay";
            report.Add(std::move(error));
            return report;
        }

        // Embedded schema (the writer stores the full schema JSON in the header).
        if (options.validate_schema) {
            const VTX::ContextualSchema contextual = reader.GetContextualSchema();
            if (contextual.property_mapping.empty()) {
                VtxDiagnostic diagnostic;
                diagnostic.code = VtxErrorCode::SchemaMissing;
                diagnostic.severity = Severity::Warning;
                diagnostic.message = "replay has no embedded schema document";
                diagnostic.source_api = "ValidateReplay";
                report.Add(std::move(diagnostic));
            } else {
                report.Absorb(ValidateSchema(contextual.property_mapping));
            }
        }

        if (options.validate_frames) {
            const PropertyAddressCache schema = reader.GetPropertyAddressCache();
            const int32_t total = reader.GetTotalFrames();
            const int32_t limit = options.max_frames < 0 ? total : std::min(total, options.max_frames);
            for (int32_t i = 0; i < limit; ++i) {
                const VTX::Frame* frame = reader.GetFrameSync(i);
                if (frame == nullptr) {
                    VtxDiagnostic diagnostic;
                    diagnostic.code = VtxErrorCode::NotFound;
                    diagnostic.severity = Severity::Error;
                    diagnostic.message = "frame could not be read";
                    diagnostic.frame_index = i;
                    diagnostic.source_api = "ValidateReplay";
                    report.Add(std::move(diagnostic));
                    continue;
                }
                report.Absorb(ValidateFrame(*frame, schema, i));
            }
        }

        return report;
    }

    ValidationReport ValidateReplayFile(const std::string& filepath, const ReplayValidationOptions& options) {
        ValidationReport report;

        ReaderContext ctx = OpenReplayFile(filepath);
        if (!ctx) {
            VtxError error = ctx.GetError();
            if (error.code == VtxErrorCode::None) {
                error.code = VtxErrorCode::ReplayOpenFailed;
                error.severity = Severity::Error;
                error.message = "failed to open replay '" + filepath + "'";
            }
            error.source_api = "ValidateReplayFile";
            report.Add(std::move(error));
            return report;
        }

        return ValidateReplay(*ctx.reader, options);
    }

} // namespace VTX
