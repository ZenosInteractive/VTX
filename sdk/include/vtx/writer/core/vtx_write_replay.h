/**
 * @file vtx_write_replay.h
 * @brief One-call recording pipeline: drive a data source into a finalized .vtx.

 * @author Zenos Interactive
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vtx/common/vtx_diagnostics.h"
#include "vtx/writer/core/vtx_data_source.h"
#include "vtx/writer/core/vtx_writer_facade.h"

namespace VTX {

    /**
     * @brief Aggregated outcome of a one-call WriteReplay().
     */
    struct WriteReplayResult {
        bool ok = false;
        VtxError error;
        std::vector<VtxWarning> warnings;

        size_t frames_written = 0;
        size_t frames_dropped = 0;

        std::string output_path;
        double elapsed_seconds = 0.0;
        int32_t total_frames = 0;
        explicit operator bool() const { return ok; }
    };

    /**
     * @brief Create a writer from @p config, drive every frame from @p source into
     *        it, finalize, and report the outcome -- the whole record loop in one call.
     * @param format Serialization backend (default FlatBuffers).
     */
    WriteReplayResult WriteReplay(const WriterFacadeConfig& config, IFrameDataSource& source,
                                  SerializationFormat format = SerializationFormat::Flatbuffers);

} // namespace VTX
