#include "services/timeline_view_service.h"

#include <algorithm>

namespace VtxServices {

    float TimelineViewService::ComputePlaybackFps(int total_frames, float duration_seconds, float fallback_fps) {
        if (total_frames <= 0 || duration_seconds <= 0.0f) {
            return fallback_fps;
        }
        return static_cast<float>(total_frames) / duration_seconds;
    }

    ClockTime TimelineViewService::ToClockTime(float total_seconds) {
        const float safe_seconds = std::max(total_seconds, 0.0f);
        const int whole_seconds = static_cast<int>(safe_seconds);
        return ClockTime {.minutes = whole_seconds / 60, .seconds = whole_seconds % 60};
    }

    DurationSplit TimelineViewService::SplitDuration(float total_seconds) {
        const float safe_seconds = std::max(total_seconds, 0.0f);
        const int minutes = static_cast<int>(safe_seconds) / 60;
        const float seconds = safe_seconds - (minutes * 60.0f);
        return DurationSplit {.minutes = minutes, .seconds = seconds};
    }

    TimelineClockSpan TimelineViewService::BuildTimelineClockSpan(int current_frame, int total_frames,
                                                                  float duration_seconds, float fallback_fps) {
        const float fps = ComputePlaybackFps(total_frames, duration_seconds, fallback_fps);
        const float current_time_sec = static_cast<float>(current_frame) / fps;
        return TimelineClockSpan {
            .current = ToClockTime(current_time_sec), .total = ToClockTime(duration_seconds), .fps = fps};
    }

    int TimelineViewService::ClampFrame(int frame, int total_frames) {
        if (total_frames <= 0) {
            return 0;
        }
        return std::clamp(frame, 0, total_frames - 1);
    }

    float TimelineViewService::ComputeItemFullWidth(const TimelineBarState& bar_state) {
        return bar_state.bar_width + bar_state.bar_spacing;
    }

    float TimelineViewService::ComputeCenteredScroll(int frame_index, float item_full_width, float view_width) {
        const float target_x = frame_index * item_full_width;
        const float desired_scroll = target_x - (view_width * 0.5f) + (item_full_width * 0.5f);
        return std::max(0.0f, desired_scroll);
    }

    TimelineVisibleRange TimelineViewService::ComputeVisibleRange(float scroll_x, float view_width, int total_frames,
                                                                  const TimelineBarState& bar_state) {
        TimelineVisibleRange range;
        range.item_full_width = ComputeItemFullWidth(bar_state);
        range.total_content_width = total_frames * range.item_full_width;

        int first_visible_idx = static_cast<int>(scroll_x / range.item_full_width);
        int last_visible_idx = static_cast<int>((scroll_x + view_width) / range.item_full_width) + 2;

        range.first_visible_idx = std::clamp(first_visible_idx, 0, total_frames);
        range.last_visible_idx = std::clamp(last_visible_idx, 0, total_frames);
        return range;
    }

    float TimelineViewService::ComputeZoomedScroll(float wheel, float mouse_x_in_child, float scroll_x,
                                                   TimelineBarState& bar_state) {
        const float item_full_width = ComputeItemFullWidth(bar_state);
        const float local_mouse_x = mouse_x_in_child + scroll_x;
        const float anchor_frame_pos = local_mouse_x / item_full_width;

        bar_state.bar_width += wheel * (bar_state.bar_width * 0.2f);
        bar_state.bar_width = std::clamp(bar_state.bar_width, bar_state.min_bar_width, bar_state.max_bar_width);

        const float target_item_full_width = ComputeItemFullWidth(bar_state);
        const float target_local_mouse_x = anchor_frame_pos * target_item_full_width;
        const float desired_scroll = target_local_mouse_x - mouse_x_in_child;
        return std::max(0.0f, desired_scroll);
    }

    int TimelineViewService::ResolveHoveredFrame(float mouse_x, float mouse_y, float origin_x, float origin_y,
                                                 float timeline_height, const TimelineVisibleRange& range) {
        for (int i = range.first_visible_idx; i < range.last_visible_idx; ++i) {
            const float x_start = origin_x + (i * range.item_full_width);
            const float x_end = x_start + range.item_full_width;
            const float y_end = origin_y + timeline_height;

            if (mouse_x >= x_start && mouse_x < x_end && mouse_y >= origin_y && mouse_y <= y_end) {
                return i;
            }
        }
        return -1;
    }

} // namespace VtxServices
