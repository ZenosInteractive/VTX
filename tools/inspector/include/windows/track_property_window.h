#pragma once

#include <memory>
#include <string>
#include <vector>

#include "gui/gui_window.h"
#include "services/analysis_scanner.h"

class InspectorSession;

class TrackPropertyWindow : public ImGuiWindow {
public:
    TrackPropertyWindow(std::shared_ptr<InspectorSession> session, int instance_id);

protected:
    void DrawContent() override;

private:
    void DrawConfigPanel();
    void DrawProgressPanel();
    void DrawResultsPanel();
    void PopulateBucketNames();

    std::shared_ptr<InspectorSession> inspector_session_;
    VtxServices::AnalysisScanner scanner_;

    // Config state
    int start_frame_ = 0;
    int end_frame_ = 0;
    int selected_bucket_index_ = 0;
    std::vector<std::string> bucket_names_;
    char uid_filter_input_[512] = {};
    char property_names_input_[512] = {};
    bool has_populated_buckets_ = false;

    // Results filter
    char search_filter_[256] = {};
};
