#pragma once
#include <memory>
#include <functional>
#include "vtx_data_source.h"
#include "vtx_writer_facade.h"

namespace VTX {
    class RecordPipeline {
    public:
        RecordPipeline(std::unique_ptr<IFrameDataSource> source, std::unique_ptr<IVtxWriterFacade> writer);
        bool Run(std::function<void(float, std::string)> on_progress = nullptr);

    private:
        std::unique_ptr<IFrameDataSource> source_;
        std::unique_ptr<IVtxWriterFacade> writer_;
    };

} // namespace VTX