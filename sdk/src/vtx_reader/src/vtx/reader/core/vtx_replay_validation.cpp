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
            VtxDiagnostic diagnostic;
            diagnostic.code = VtxErrorCode::ReplayNotReady;
            diagnostic.severity = Severity::Error;
            diagnostic.message = "replay is not ready: " + reader.GetReadyError();
            diagnostic.source_api = "ValidateReplay";
            report.Add(std::move(diagnostic));
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

        // Frames are validated against the reader's resolved property cache, which
        // is always populated even when the embedded JSON document is absent.
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
            VtxDiagnostic diagnostic;
            diagnostic.code = VtxErrorCode::ReplayOpenFailed;
            diagnostic.severity = Severity::Error;
            diagnostic.message = "failed to open replay '" + filepath + "': " + ctx.GetError();
            diagnostic.source_api = "ValidateReplayFile";
            report.Add(std::move(diagnostic));
            return report;
        }

        return ValidateReplay(*ctx.reader, options);
    }

} // namespace VTX
