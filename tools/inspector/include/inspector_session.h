#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "gui/gui_types.h"
#include "services/entity_inspector_view_service.h"
#include "services/reader_chunk_state_service.h"
#include "services/schema_view_service.h"
#include "session/vtx_session_base.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"


class InspectorSession : public VtxSessionBase {
public:
    InspectorSession() = default;
    ~InspectorSession() override;

    bool LoadReplay(const std::string& filepath);
    void CloseReplay();
    void RequestSchemaHighlight(const std::string& struct_n, const std::string& prop_n);

    bool HasLoadedReplay() const { return (reader_context_.Loaded() && is_file_loaded_); }
    float GetFileSizeMb() const { return reader_context_.size_in_mb; }
    VTX::VtxFormat GetFormat() const { return reader_context_.format; }
    int32_t GetCurrentFrame() const { return current_frame_; }
    int32_t GetTotalFrames() const { return reader_context_->GetTotalFrames(); }
    bool IsScrubbingTimeline() const { return is_scrubbing_timeline_; }
    TimeDisplayFormat GetTimeDisplayFormat() const { return time_display_format_; }

    void SetCurrentFrame(int32_t frame) { current_frame_ = frame; }
    void SetScrubbingTimeline(bool scrubbing) { is_scrubbing_timeline_ = scrubbing; }
    void SetTimeDisplayFormat(TimeDisplayFormat format) { time_display_format_ = format; }

    VTX::IVtxReaderFacade* GetReader() const { return reader_context_.operator->(); }
    const VTX::FileHeader& GetHeader() const { return header_; }
    const VTX::FileFooter& GetFooter() const { return footer_; }
    const VTX::ContextualSchema& GetContextualSchema() const { return contextual_schema_; }

    const VtxServices::SchemaHighlightState& GetSchemaHighlight() const { return schema_highlight_; }
    VtxServices::SchemaHighlightState& MutableSchemaHighlight() { return schema_highlight_; }
    const VtxServices::EntityInspectorState& GetEntityInspectorState() const { return entity_inspector_state_; }
    VtxServices::EntityInspectorState& MutableEntityInspectorState() { return entity_inspector_state_; }
    bool GetShowSchemaNames() const { return show_schema_names_; }
    bool& MutableShowSchemaNames() { return show_schema_names_; }

    /// Chunk state snapshot from the SDK reader (events wired automatically).
    VTX::ReaderChunkSnapshot GetReaderChunkSnapshot() const {
        if (reader_context_.chunk_state) return reader_context_.chunk_state->GetSnapshot();
        return {};
    }

private:
    VTX::ReaderContext reader_context_;
    VTX::FileHeader header_;
    VTX::FileFooter footer_;
    VTX::ContextualSchema contextual_schema_;

    int32_t current_frame_ = -1;
    bool is_scrubbing_timeline_ = false;
    TimeDisplayFormat time_display_format_ = TimeDisplayFormat::Ticks;
    bool show_schema_names_ = false;
    VtxServices::SchemaHighlightState schema_highlight_;
    VtxServices::EntityInspectorState entity_inspector_state_;
};
