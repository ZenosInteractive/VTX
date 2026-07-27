#pragma once

#include "vtx/common/vtx_types.h"

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

    // Per-frame recording-gap map derived from the footer time table against an
    // expected capture rate. A frame is flagged when the wall-clock span from the
    // previous stamped frame exceeds 1.5x the expected interval; every frame
    // inside that span carries the gap duration and the estimated missing count.
    // One recording gap as a wall-clock span, for time-proportional painting.
    struct DroppedFrameGap {
        int first_frame = 0;        // first flagged frame
        int last_frame = 0;         // stamped frame that ends the gap
        float start_seconds = 0.0f; // elapsed seconds where the gap begins
        float duration_ms = 0.0f;
    };

    struct DroppedFrameMap {
        std::vector<uint8_t> flagged; // 1 = frame sits in a recording gap
        std::vector<float> gap_ms;    // wall-clock gap the frame belongs to (0 when none)
        std::vector<int32_t> missing; // estimated frames missing in that gap
        std::vector<DroppedFrameGap> gaps;
        int gap_count = 0;         // number of distinct gaps
        int64_t total_missing = 0; // estimated frames missing across all gaps
    };

    class TimelineViewService {
    public:
        static float ComputePlaybackFps(int total_frames, float duration_seconds, float fallback_fps = 30.0f);
        static ClockTime ToClockTime(float total_seconds);
        static DurationSplit SplitDuration(float total_seconds);

        // Elapsed seconds since capture start for `frame`, read from the footer
        // per-frame time table (created_utc preferred, game_time fallback).
        // Falls back to linear frame/avg-fps mapping when the table is absent,
        // so captures with recording stalls resolve to wall-clock positions
        // instead of drifting by the stalled time.
        static float FrameToElapsedSeconds(int frame, const VTX::ReplayTimeData& times, int total_frames,
                                           float duration_seconds, float fallback_fps = 30.0f);

        // Inverse of FrameToElapsedSeconds: the last frame stamped at or before
        // `seconds`, found by binary search over the footer time table (zero
        // entries are skipped). Falls back to linear time*avg-fps mapping when
        // the table is absent.
        static int FrameAtElapsedSeconds(float seconds, const VTX::ReplayTimeData& times, int total_frames,
                                         float duration_seconds, float fallback_fps = 30.0f);

        // Builds the recording-gap map for the whole replay from the footer time
        // table (created_utc preferred, game_time fallback). Returns an empty map
        // (no frames flagged) when the file carries no usable time table.
        static DroppedFrameMap BuildDroppedFrameMap(const VTX::ReplayTimeData& times, int total_frames,
                                                    float expected_fps);

        static TimelineClockSpan BuildTimelineClockSpan(int current_frame, int total_frames, float duration_seconds,
                                                        float fallback_fps = 30.0f);
        static TimelineClockSpan BuildTimelineClockSpan(int current_frame, int total_frames, float duration_seconds,
                                                        const VTX::ReplayTimeData& times, float fallback_fps = 30.0f);
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
