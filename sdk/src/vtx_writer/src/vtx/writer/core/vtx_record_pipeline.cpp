#include "vtx/writer/core/vtx_record_pipeline.h"

VTX::RecordPipeline::RecordPipeline(std::unique_ptr<IFrameDataSource> source, std::unique_ptr<IVtxWriterFacade> writer)
    : source_(std::move(source))
    , writer_(std::move(writer)) {}

bool VTX::RecordPipeline::Run(std::function<void(float, std::string)> on_progress) {
    if (!source_ || !writer_)
        return false;

    if (!source_->Initialize()) {
        return false;
    }

    size_t total_frames = source_->GetExpectedTotalFrames();
    size_t frames_processed = 0;
    int last_percent = -1;

    VTX::Frame native_frame;
    VTX::GameTime::GameTimeRegister time_register;


    while (source_->GetNextFrame(native_frame, time_register)) {
        writer_->RecordFrame(native_frame, time_register);
        frames_processed++;

        if (total_frames > 0) {
            int percent = static_cast<int>((frames_processed * 100) / total_frames);
            if (percent > last_percent) {
                if (on_progress) {
                    on_progress(percent / 100.0f, "Converting... " + std::to_string(percent) + "%");
                }
                last_percent = percent;
            }
        } else {
            if (frames_processed % 100 == 0) {
                if (on_progress) {
                    on_progress(0.0f, "Processed " + std::to_string(frames_processed) + " frames...");
                }
            }
        }
    }

    writer_->Stop();
    if (on_progress)
        on_progress(1.0f, "");
    return frames_processed > 0;
}
