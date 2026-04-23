#pragma once

namespace VtxServices {

    struct ClockTime {
        int minutes = 0;
        int seconds = 0;
    };

    struct DurationSplit {
        int minutes = 0;
        float seconds = 0.0f;
    };

    struct TimelineClockSpan {
        ClockTime current;
        ClockTime total;
        float fps = 30.0f;
    };

    struct TimelineBarState {
        float bar_width = 2.0f;
        float bar_spacing = 1.0f;
        float min_bar_width = 0.5f;
        float max_bar_width = 50.0f;
        int last_tracked_frame = -1;
    };

    struct TimelineVisibleRange {
        int first_visible_idx = 0;
        int last_visible_idx = 0;
        float item_full_width = 0.0f;
        float total_content_width = 0.0f;
    };

    class TimelineViewService {
    public:
        static float ComputePlaybackFps(int total_frames, float duration_seconds, float fallback_fps = 30.0f);
        static ClockTime ToClockTime(float total_seconds);
        static DurationSplit SplitDuration(float total_seconds);
        static TimelineClockSpan BuildTimelineClockSpan(int current_frame, int total_frames, float duration_seconds,
                                                        float fallback_fps = 30.0f);
        static int ClampFrame(int frame, int total_frames);
        static float ComputeItemFullWidth(const TimelineBarState& bar_state);
        static float ComputeCenteredScroll(int frame_index, float item_full_width, float view_width);
        static TimelineVisibleRange ComputeVisibleRange(float scroll_x, float view_width, int total_frames,
                                                        const TimelineBarState& bar_state);
        static float ComputeZoomedScroll(float wheel, float mouse_x_in_child, float scroll_x,
                                         TimelineBarState& bar_state);
        static int ResolveHoveredFrame(float mouse_x, float mouse_y, float origin_x, float origin_y,
                                       float timeline_height, const TimelineVisibleRange& range);
    };

} // namespace VtxServices
