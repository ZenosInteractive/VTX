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
    // Returns the recording-gap map for the loaded replay, rebuilding the cache
    // when the expected fps or the loaded file changed.
    const VtxServices::DroppedFrameMap& GetDroppedFrameMap(int total_frames);

private:
    VtxServices::TimelineBarState timeline_bar_state_ {};
    std::shared_ptr<InspectorSession> inspector_session_;

    float drop_detect_fps_ = 30.0f;
    VtxServices::DroppedFrameMap dropped_map_ {};
    float dropped_map_fps_ = 0.0f;
    int dropped_map_frames_ = -1;
    size_t dropped_map_stamp_count_ = 0;
};
