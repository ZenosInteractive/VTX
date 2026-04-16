#include "inspector_session.h"

#include <memory>
#include <string>

#include "services/inspector_replay_service.h"
#include "services/schema_view_service.h"

InspectorSession::~InspectorSession() = default;

bool InspectorSession::LoadReplay(const std::string& filepath) {
    AddGuiInfoLog("Opening replay: " + filepath);

    reader_context_ = VTX::OpenReplayFile(filepath);
    if (!reader_context_.Loaded()) {
        AddGuiErrorLog(reader_context_.GetError());
        is_file_loaded_ = false;
        return false;
    }


    current_file_path_ = filepath;
    current_frame_ = 0;
    is_file_loaded_ = true;
    RecordRecentFile(filepath);

    AddGuiInfoLog("Replay loaded successfully.");
    return true;
}

void InspectorSession::CloseReplay() {
    if (is_file_loaded_) {
        AddGuiInfoLog("Closing replay: " + current_file_path_);
    }

    reader_context_.Reset();
    is_file_loaded_ = false;
    current_file_path_.clear();
    current_frame_ = 0;
    is_scrubbing_timeline_ = false;
}

void InspectorSession::RequestSchemaHighlight(const std::string& struct_n, const std::string& prop_n) {
    VtxServices::SchemaViewService::RequestHighlight(schema_highlight_, struct_n, prop_n);
}
