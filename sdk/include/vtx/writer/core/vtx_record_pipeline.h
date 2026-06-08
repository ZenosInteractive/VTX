#pragma once
#include <memory>
#include <functional>
#include "vtx_data_source.h"
#include "vtx_writer_facade.h"
#include "vtx_writer_result.h"

namespace VTX {
    class RecordPipeline {
    public:
        RecordPipeline(std::unique_ptr<IFrameDataSource> source, std::unique_ptr<IVtxWriterFacade> writer);

        /**
         * @brief Drives the source -> writer loop to completion (#8).
         * @return A PipelineReport accounting written / rejected / skipped frames,
         *         with rejections split into validation_errors vs timer_errors.
         */
        PipelineReport Run(std::function<void(float, std::string)> on_progress = nullptr);

    private:
        std::unique_ptr<IFrameDataSource> source_;
        std::unique_ptr<IVtxWriterFacade> writer_;
    };

} // namespace VTX