/**
 * @file vtx_write_replay.cpp
 * @brief Implementation of the one-call WriteReplay pipeline.
 * @author Zenos Interactive
 */
#include "vtx/writer/core/vtx_write_replay.h"

#include <chrono>
#include <memory>

namespace VTX {

    WriteReplayResult WriteReplay(const WriterFacadeConfig& config, IFrameDataSource& source,
                                  SerializationFormat format) {
        WriteReplayResult result;
        result.output_path = config.output_filepath;

        const auto started = std::chrono::steady_clock::now();
        const auto stamp_elapsed = [&] {
            result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        };

        std::unique_ptr<IVtxWriterFacade> writer = (format == SerializationFormat::Protobuf)
                                                       ? CreateProtobufWriterFacade(config)
                                                       : CreateFlatBuffersWriterFacade(config);
        if (!writer) {
            result.error.code = VtxErrorCode::SchemaInvalid;
            result.error.severity = Severity::Error;
            result.error.message = "failed to create writer (schema missing or invalid)";
            result.error.source_api = "WriteReplay";
            stamp_elapsed();
            return result;
        }

        if (!source.Initialize()) {
            result.error.code = VtxErrorCode::Internal;
            result.error.severity = Severity::Error;
            result.error.message = "frame data source failed to initialize";
            result.error.source_api = "WriteReplay";
            writer->Stop(); // finalize an empty but valid replay
            stamp_elapsed();
            return result;
        }

        VTX::Frame frame;
        VTX::GameTime::GameTimeRegister time_reg;
        while (source.GetNextFrame(frame, time_reg)) {
            const RecordResult r = writer->TryRecordFrame(frame, time_reg);
            if (r.IsWritten()) {
                ++result.frames_written;
            } else {
                ++result.frames_dropped;
                result.warnings.push_back(r.error); // surface each dropped frame as a warning
            }
        }

        writer->Stop();
        result.total_frames = static_cast<int32_t>(result.frames_written);
        result.ok = true;
        stamp_elapsed();
        return result;
    }

} // namespace VTX
