#include "services/timeline_view_service.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

    constexpr double kTicksPerSecond = 10'000'000.0;

    // First nonzero tick in the table; 0 when the table has no usable entries.
    uint64_t FirstNonZeroTick(const std::vector<uint64_t>& ticks) {
        for (const uint64_t tick : ticks) {
            if (tick != 0) {
                return tick;
            }
        }
        return 0;
    }

    // Tick for `frame`, walking back to the nearest earlier stamped frame when
    // the entry is missing/zero (frames without a recorded value store 0).
    uint64_t TickAtOrBefore(const std::vector<uint64_t>& ticks, int frame) {
        if (ticks.empty() || frame < 0) {
            return 0;
        }
        int index = std::min(frame, static_cast<int>(ticks.size()) - 1);
        for (; index >= 0; --index) {
            if (ticks[index] != 0) {
                return ticks[index];
            }
        }
        return 0;
    }

    // Elapsed seconds for `frame` from a per-frame tick table (100ns ticks).
    // Returns a negative value when the table cannot answer for this frame.
    float ElapsedFromTickTable(const std::vector<uint64_t>& ticks, int frame) {
        const uint64_t base_tick = FirstNonZeroTick(ticks);
        if (base_tick == 0) {
            return -1.0f;
        }
        const uint64_t frame_tick = TickAtOrBefore(ticks, frame);
        if (frame_tick == 0 || frame_tick < base_tick) {
            return -1.0f;
        }
        return static_cast<float>(static_cast<double>(frame_tick - base_tick) / kTicksPerSecond);
    }

} // namespace

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

    float TimelineViewService::FrameToElapsedSeconds(int frame, const VTX::ReplayTimeData& times, int total_frames,
                                                     float duration_seconds, float fallback_fps) {
        const float utc_elapsed = ElapsedFromTickTable(times.created_utc, frame);
        if (utc_elapsed >= 0.0f) {
            return utc_elapsed;
        }
        const float game_elapsed = ElapsedFromTickTable(times.game_time, frame);
        if (game_elapsed >= 0.0f) {
            return game_elapsed;
        }
        const float fps = ComputePlaybackFps(total_frames, duration_seconds, fallback_fps);
        return static_cast<float>(frame) / fps;
    }

    TimelineClockSpan TimelineViewService::BuildTimelineClockSpan(int current_frame, int total_frames,
                                                                  float duration_seconds, float fallback_fps) {
        const float fps = ComputePlaybackFps(total_frames, duration_seconds, fallback_fps);
        const float current_time_sec = static_cast<float>(current_frame) / fps;
        return TimelineClockSpan {
            .current = ToClockTime(current_time_sec), .total = ToClockTime(duration_seconds), .fps = fps};
    }

    DroppedFrameMap TimelineViewService::BuildDroppedFrameMap(const VTX::ReplayTimeData& times, int total_frames,
                                                              float expected_fps) {
        DroppedFrameMap map;
        if (total_frames <= 0 || expected_fps <= 0.0f) {
            return map;
        }
        map.flagged.assign(static_cast<size_t>(total_frames), 0);
        map.gap_ms.assign(static_cast<size_t>(total_frames), 0.0f);
        map.missing.assign(static_cast<size_t>(total_frames), 0);

        const std::vector<uint64_t>& ticks =
            FirstNonZeroTick(times.created_utc) != 0 ? times.created_utc : times.game_time;
        if (FirstNonZeroTick(ticks) == 0) {
            return map;
        }

        const double expected_interval_ms = 1000.0 / static_cast<double>(expected_fps);
        const int last_index = std::min(total_frames, static_cast<int>(ticks.size())) - 1;

        int prev_stamped = -1;
        for (int frame = 0; frame <= last_index; ++frame) {
            if (ticks[static_cast<size_t>(frame)] == 0) {
                continue;
            }
            if (prev_stamped >= 0) {
                const int span_frames = frame - prev_stamped;
                const double actual_ms =
                    static_cast<double>(ticks[static_cast<size_t>(frame)] - ticks[static_cast<size_t>(prev_stamped)]) /
                    10'000.0;
                const double expected_ms = expected_interval_ms * span_frames;
                if (actual_ms > expected_ms * 1.5) {
                    const int missing =
                        std::max(1, static_cast<int>(actual_ms / expected_interval_ms + 0.5) - span_frames);
                    ++map.gap_count;
                    map.total_missing += missing;
                    for (int i = prev_stamped + 1; i <= frame; ++i) {
                        map.flagged[static_cast<size_t>(i)] = 1;
                        map.gap_ms[static_cast<size_t>(i)] = static_cast<float>(actual_ms);
                        map.missing[static_cast<size_t>(i)] = missing;
                    }
                }
            }
            prev_stamped = frame;
        }
        return map;
    }

    TimelineClockSpan TimelineViewService::BuildTimelineClockSpan(int current_frame, int total_frames,
                                                                  float duration_seconds,
                                                                  const VTX::ReplayTimeData& times,
                                                                  float fallback_fps) {
        const float fps = ComputePlaybackFps(total_frames, duration_seconds, fallback_fps);
        const float current_time_sec =
            FrameToElapsedSeconds(current_frame, times, total_frames, duration_seconds, fallback_fps);
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
