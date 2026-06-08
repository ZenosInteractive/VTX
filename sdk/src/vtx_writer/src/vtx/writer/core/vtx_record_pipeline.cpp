#include "vtx/writer/core/vtx_record_pipeline.h"

VTX::RecordPipeline::RecordPipeline(std::unique_ptr<IFrameDataSource> source, std::unique_ptr<IVtxWriterFacade> writer)
    : source_(std::move(source))
    , writer_(std::move(writer)) {}

VTX::PipelineReport VTX::RecordPipeline::Run(std::function<void(float, std::string)> on_progress) {
    PipelineReport report;
    if (!source_ || !writer_)
        return report;

    if (!source_->Initialize()) {
        return report;
    }

    const size_t total_frames = source_->GetExpectedTotalFrames();
    int last_percent = -1;

    VTX::Frame native_frame;
    VTX::GameTime::GameTimeRegister time_register;

    while (source_->GetNextFrame(native_frame, time_register)) {
        const RecordResult result = writer_->TryRecordFrame(native_frame, time_register);
        report.Account(result);

        const size_t processed = report.Total();
        if (total_frames > 0) {
            const int percent = static_cast<int>((processed * 100) / total_frames);
            if (percent > last_percent) {
                if (on_progress) {
                    on_progress(percent / 100.0f, "Converting... " + std::to_string(percent) + "%");
                }
                last_percent = percent;
            }
        } else if (processed % 100 == 0) {
            if (on_progress) {
                on_progress(0.0f, "Processed " + std::to_string(processed) + " frames...");
            }
        }
    }

    writer_->Stop();
    if (on_progress)
        on_progress(1.0f, "");
    return report;
}
