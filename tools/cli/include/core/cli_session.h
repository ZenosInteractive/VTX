#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/differ/core/vtx_differ_facade.h"


namespace VtxCli {

class CliSession {
public:
    CliSession() = default;
    ~CliSession() = default;

    CliSession(const CliSession&) = delete;
    CliSession& operator=(const CliSession&) = delete;

    bool Open(const std::string& filepath);
    void Close();

    bool IsLoaded() const { return reader_context_.Loaded(); }
    const std::string& GetFilePath() const { return file_path_; }
    VTX::VtxFormat GetFormat() const { return reader_context_.GetFormat(); }
    int GetTotalFrames() const { return reader_context_->GetTotalFrames(); }
    int GetCurrentFrame() const { return current_frame_; }

    bool SetFrame(int32_t frame);

    VTX::IVtxReaderFacade* GetReader() const { return reader_context_.operator->(); }
    VTX::FileHeader GetHeader() const { return reader_context_->GetHeader(); }
    VTX::FileFooter GetFooter() const { return reader_context_->GetFooter(); }
    VTX::ContextualSchema GetContextualSchema() const { return reader_context_->GetContextualSchema(); }
    VTX::PropertyAddressCache GetPropertyCache() const { return reader_context_->GetPropertyAddressCache(); }
    float GetFileSizeMb() const { return reader_context_.size_in_mb; }
    const VTX::Frame* GetCurrentFrameData();
    const VTX::Frame* GetFrameData(int frame_index) const;

    /// Pin a frame: deep-copies it from the reader cache and recomputes
    /// content_hashes with the current hash function.  The returned pointer
    /// stays valid until UnpinFrame() is called (unaffected by reader eviction).
    const VTX::Frame* PinFrame(int frame_index);
    void UnpinFrame(int frame_index);

    /// Diff two frames using vtx_differ (binary tree-diff on raw serialized data).
    VtxDiff::PatchIndex DiffFrames(int frame_a, int frame_b,
                                   const VtxDiff::DiffOptions& opts = {});

    const std::string& GetLastError() const { return last_error_; }

private:
    std::string file_path_;
    int32_t current_frame_ = 0;
    std::string last_error_;

    VTX::ReaderContext reader_context_;
    std::unique_ptr<VtxDiff::IVtxDifferFacade> differ_;
    std::unordered_map<int, VTX::Frame> pinned_frames_;
};


struct FramePin {
    CliSession& session;
    int frame_index;
    const VTX::Frame* frame;

    FramePin(CliSession& s, int idx)
        : session(s), frame_index(idx), frame(s.PinFrame(idx)) {}
    ~FramePin() { session.UnpinFrame(frame_index); }

    FramePin(const FramePin&) = delete;
    FramePin& operator=(const FramePin&) = delete;

    explicit operator bool() const { return frame != nullptr; }
    const VTX::Frame& operator*() const { return *frame; }
    const VTX::Frame* operator->() const { return frame; }
};

} // namespace VtxCli
