#pragma once
#include "gui/gui_window.h"
#include "services/timeline_view_service.h"

class InspectorSession;

class TimelineWindow : public ImGuiWindow {

public:
    TimelineWindow(const std::shared_ptr<InspectorSession>& session);

protected:
    void DrawContent() override;

private:
    void DrawTimelineSlider(int total_frames, float duration_seconds);
    void DrawTimeAndFrameInfo(int total_frames, float duration);
    
    //dynamic timeline
    void DrawFrameStripTimeline(int total_frames);
    
private:
    VtxServices::TimelineBarState timeline_bar_state_{};
    std::shared_ptr<InspectorSession> inspector_session_;
};
