#include "core/cli_session.h"
#include <chrono>
#include <thread>

#include "vtx/differ/core/vtx_default_tree_diff.h"

namespace VtxCli {

bool CliSession::Open(const std::string& filepath) {
    Close();

    reader_context_ = VTX::OpenReplayFile(filepath);
    if (!reader_context_) {
        last_error_ = reader_context_.error;
        return false;
    }

    // Init diff factory for the detected wire format
    differ_ = VtxDiff::CreateDifferFacade(reader_context_.format);

    current_frame_ = 0;
    file_path_ = filepath;
    last_error_.clear();
    return true;
}

void CliSession::Close() {
    reader_context_.Reset();
    pinned_frames_.clear();

    differ_.reset();

    file_path_.clear();
    current_frame_ = 0;
    last_error_.clear();
}

bool CliSession::SetFrame(int32_t frame) {
    if (!reader_context_.Loaded()) {
        last_error_ = "No file loaded.";
        return false;
    }
    if (frame < 0 || frame >= reader_context_->GetTotalFrames()) {
        last_error_ = "Frame out of range [0, " + std::to_string(reader_context_->GetTotalFrames() - 1) + "].";
        return false;
    }
    current_frame_ = frame;
    return true;
}

const VTX::Frame* CliSession::GetCurrentFrameData() {
    return GetFrameData(current_frame_);
}

const VTX::Frame* CliSession::GetFrameData(int frame_index) const {

    if (!reader_context_.Loaded())
    {
        return nullptr;
    }

    if (frame_index < 0 || frame_index >= reader_context_->GetTotalFrames())
    {
        return nullptr;
    }

    return reader_context_->GetFrameSync(frame_index);
}

const VTX::Frame* CliSession::PinFrame(int frame_index) {
    // Already pinned — return existing copy
    auto it = pinned_frames_.find(frame_index);
    if (it != pinned_frames_.end()) return &it->second;

    const VTX::Frame* src = GetFrameData(frame_index);
    if (!src) return nullptr;

    // Deep-copy and recompute hashes with the current (fixed) hash function
    auto& copy = pinned_frames_[frame_index];
    copy = *src;

    for (auto& bucket : copy.GetMutableBuckets()) {
        for (auto& entity : bucket.entities) {
            entity.content_hash = VTX::Helpers::CalculateContainerHash(entity);
        }
    }

    return &copy;
}

void CliSession::UnpinFrame(int frame_index) {
    pinned_frames_.erase(frame_index);
}

VtxDiff::PatchIndex CliSession::DiffFrames(int frame_a, int frame_b,
                                            const VtxDiff::DiffOptions& opts)
{
    if (!reader_context_.Loaded()) return {};

    // Get raw binary for frame A and copy (loading B may evict A's chunk)
    auto span_a = reader_context_->GetRawFrameBytes(frame_a);
    if (span_a.empty()) return {};
    std::vector<std::byte> bytes_a(span_a.begin(), span_a.end());

    auto span_b = reader_context_->GetRawFrameBytes(frame_b);
    if (span_b.empty()) return {};

    differ_->DiffRawFrames(span_a, span_b, opts);
    return {};
}

} // namespace VtxCli
