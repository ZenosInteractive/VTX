#include "windows/timeline_window.h"

#include <algorithm>
#include <imgui.h>
#include <vector>

#include "gui/gui_types.h"
#include "inspector_session.h"

namespace {

struct TimelineHoverInfo {
    bool has_hover = false;
    int frame_index = -1;
    float frame_time_seconds = 0.0f;
    VtxServices::ClockTime frame_time_clock;
};

struct TimelineFrameBarView {
    int frame_index = 0;
    float x_start = 0.0f;
    float x_end = 0.0f;
    float y_top = 0.0f;
    float y_bottom = 0.0f;
    bool is_current = false;
    bool is_hovered = false;
};

struct TimelineStripViewModel {
    float total_content_width = 0.0f;
    float item_full_width = 0.0f;
    std::vector<TimelineFrameBarView> bars;
    TimelineHoverInfo hover_info;
    bool request_scroll = false;
    float desired_scroll_x = 0.0f;
};

// Clamps a requested frame into valid timeline range.
int HandleGoToFrame(int requested_frame, int total_frames) {
    return VtxServices::TimelineViewService::ClampFrame(requested_frame, total_frames);
}

// Computes per-bar width including bar+spacing budget.
float ComputeItemFullWidth(const VtxServices::TimelineBarState& timeline_bar_state) {
    return VtxServices::TimelineViewService::ComputeItemFullWidth(timeline_bar_state);
}

// Computes scroll offset that places the selected frame near the center.
float ComputeCenteredScroll(
    const VtxServices::TimelineBarState& timeline_bar_state,
    int frame_index,
    float view_width) {
    return VtxServices::TimelineViewService::ComputeCenteredScroll(
        frame_index,
        ComputeItemFullWidth(timeline_bar_state),
        view_width);
}

// Builds hover payload (frame index + derived time labels) for tooltip rendering.
TimelineHoverInfo BuildHoverInfo(int hovered_frame, int total_frames, float duration_seconds) {
    TimelineHoverInfo info;
    if (hovered_frame < 0) {
        return info;
    }

    const float fps = VtxServices::TimelineViewService::ComputePlaybackFps(total_frames, duration_seconds);
    const float frame_time_sec = static_cast<float>(hovered_frame) / fps;
    info.has_hover = true;
    info.frame_index = hovered_frame;
    info.frame_time_seconds = frame_time_sec;
    info.frame_time_clock = VtxServices::TimelineViewService::ToClockTime(frame_time_sec);
    return info;
}

// Builds strip bars, hover state, and optional scroll requests for current frame.
TimelineStripViewModel BuildStripViewModel(
    VtxServices::TimelineBarState& timeline_bar_state,
    int total_frames,
    int current_frame,
    float duration_seconds,
    float scroll_x,
    float view_width,
    float origin_x,
    float origin_y,
    float timeline_height,
    bool ctrl_down,
    float wheel,
    bool is_window_hovered,
    float mouse_x,
    float mouse_y) {
    TimelineStripViewModel view_model;
    float working_scroll_x = scroll_x;

    // Zoom is anchored around mouse position to avoid losing context.
    if (is_window_hovered && ctrl_down && wheel != 0.0f) {
        const float mouse_x_in_child = mouse_x - origin_x;
        view_model.request_scroll = true;
        view_model.desired_scroll_x = VtxServices::TimelineViewService::ComputeZoomedScroll(
            wheel,
            mouse_x_in_child,
            scroll_x,
            timeline_bar_state);
        working_scroll_x = view_model.desired_scroll_x;
    } else if (current_frame != timeline_bar_state.last_tracked_frame) {
        // When frame changes externally, keep the active frame near center.
        view_model.request_scroll = true;
        view_model.desired_scroll_x = ComputeCenteredScroll(timeline_bar_state, current_frame, view_width);
        working_scroll_x = view_model.desired_scroll_x;
    }

    const auto range = VtxServices::TimelineViewService::ComputeVisibleRange(
        working_scroll_x,
        view_width,
        total_frames,
        timeline_bar_state);
    view_model.total_content_width = range.total_content_width;
    view_model.item_full_width = range.item_full_width;
    view_model.bars.reserve(std::max(0, range.last_visible_idx - range.first_visible_idx));

    const int hovered_frame_idx = VtxServices::TimelineViewService::ResolveHoveredFrame(
        mouse_x,
        mouse_y,
        origin_x,
        origin_y,
        timeline_height,
        range);

    for (int i = range.first_visible_idx; i < range.last_visible_idx; ++i) {
        const float bar_height = 0.8f * (timeline_height - 5.0f);
        const float x_start = origin_x + (i * range.item_full_width);
        const float x_end = x_start + timeline_bar_state.bar_width;
        const float y_bottom = origin_y + timeline_height;
        const float y_top = y_bottom - bar_height;

        view_model.bars.push_back(TimelineFrameBarView{
            .frame_index = i,
            .x_start = x_start,
            .x_end = x_end,
            .y_top = y_top,
            .y_bottom = y_bottom,
            .is_current = i == current_frame,
            .is_hovered = i == hovered_frame_idx,
        });
    }

    view_model.hover_info = BuildHoverInfo(hovered_frame_idx, total_frames, duration_seconds);
    return view_model;
}

} // namespace

// Constructs the timeline window bound to Inspector session state.
TimelineWindow::TimelineWindow(const std::shared_ptr<InspectorSession>& session)
    : ImGuiWindow(VtxGuiNames::TimelineWindow, session)
    , inspector_session_(session) {
    flags_ |= ImGuiWindowFlags_NoScrollbar;
}

// Renders timeline summary panel, slider, and zoomable strip.
void TimelineWindow::DrawContent() {
    if (!inspector_session_->HasLoadedReplay()) {
        ImGui::TextDisabled("No VTX replay loaded. Timeline unavailable.");
        return;
    }

    const int total_frames = inspector_session_->GetFooter().total_frames;
    const float duration = inspector_session_->GetFooter().duration_seconds;
    DrawTimeAndFrameInfo(total_frames, duration);
    ImGui::Spacing();
    DrawTimelineSlider(total_frames, duration);
    DrawFrameStripTimeline(inspector_session_->GetTotalFrames());
}

// Renders textual time/frame summary and direct "go to frame" input.
void TimelineWindow::DrawTimeAndFrameInfo(int total_frames, float duration) {
    (void)duration;
    if (ImGui::BeginChild("InfoPanel", ImVec2(0, 45), true)) {
        const auto time_span = VtxServices::TimelineViewService::BuildTimelineClockSpan(
            inspector_session_->GetCurrentFrame(),
            total_frames,
            inspector_session_->GetFooter().duration_seconds);

        ImGui::AlignTextToFramePadding();
        ImGui::Text(
            "Time: %02d:%02d / %02d:%02d",
            time_span.current.minutes,
            time_span.current.seconds,
            time_span.total.minutes,
            time_span.total.seconds);

        ImGui::SameLine(ImGui::GetWindowWidth() - 320.0f);
        ImGui::Text("Frame: %d / %d", inspector_session_->GetCurrentFrame(), total_frames);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);

        int goto_frame = inspector_session_->GetCurrentFrame();
        if (ImGui::InputInt("Go to", &goto_frame, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
            inspector_session_->SetCurrentFrame(HandleGoToFrame(goto_frame, total_frames));
        }
    }
    ImGui::EndChild();
}

// Renders main timeline slider and updates scrubbing state.
void TimelineWindow::DrawTimelineSlider(int total_frames, float duration_seconds) {
    (void)duration_seconds;
    ImGui::SetNextItemWidth(-1.0f);

    int current_frame = inspector_session_->GetCurrentFrame();
    if (ImGui::SliderInt("##MainTimeline", &current_frame, 0, total_frames - 1, "")) {
        inspector_session_->SetCurrentFrame(HandleGoToFrame(current_frame, total_frames));
    }
    inspector_session_->SetScrubbingTimeline(ImGui::IsItemActive());
}

// Renders zoomable frame bars and hover/click seek interactions.
void TimelineWindow::DrawFrameStripTimeline(int total_frames) {
    const float avail_width = ImGui::GetContentRegionAvail().x;
    const float timeline_height = 50.0f;
    ImGui::TextDisabled("Zoom: %.1fx (Ctrl+Wheel to zoom)", timeline_bar_state_.bar_width / 2.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool child_visible = ImGui::BeginChild(
        "FrameTimelineScroll",
        ImVec2(avail_width, timeline_height + ImGui::GetStyle().ScrollbarSize),
        true,
        ImGuiWindowFlags_HorizontalScrollbar);

    if (child_visible) {
        // Step 1: Build strip model from current viewport + input state.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const auto mouse_pos = ImGui::GetMousePos();
        const auto strip_vm = BuildStripViewModel(
            timeline_bar_state_,
            total_frames,
            inspector_session_->GetCurrentFrame(),
            inspector_session_->GetFooter().duration_seconds,
            ImGui::GetScrollX(),
            ImGui::GetWindowWidth(),
            p.x,
            p.y,
            timeline_height,
            ImGui::GetIO().KeyCtrl,
            ImGui::GetIO().MouseWheel,
            ImGui::IsWindowHovered(),
            mouse_pos.x,
            mouse_pos.y);
        if (strip_vm.request_scroll) {
            ImGui::SetScrollX(strip_vm.desired_scroll_x);
            timeline_bar_state_.last_tracked_frame = inspector_session_->GetCurrentFrame();
        }
        // Step 2: Draw strip background and visible bars.
        ImDrawList* child_draw_list = ImGui::GetWindowDrawList();

        ImGui::Dummy(ImVec2(strip_vm.total_content_width, timeline_height));
        child_draw_list->AddRectFilled(
            p,
            ImVec2(p.x + strip_vm.total_content_width, p.y + timeline_height),
            IM_COL32(30, 30, 30, 255));

        for (const auto& bar : strip_vm.bars) {
            ImU32 bar_color = IM_COL32(128, 128, 128, 255);
            if (bar.is_current) {
                child_draw_list->AddRectFilled(
                    ImVec2(bar.x_start - 1.0f, p.y),
                    ImVec2(bar.x_end + 1.0f, bar.y_bottom),
                    IM_COL32(255, 255, 255, 100));
                bar_color = IM_COL32(100, 100, 255, 255);
            }

            child_draw_list->AddRectFilled(ImVec2(bar.x_start, bar.y_top), ImVec2(bar.x_end, bar.y_bottom), bar_color);
        }

        // Step 3: Render hover tooltip and click-to-seek.
        if (strip_vm.hover_info.has_hover) {
            const auto& hover_info = strip_vm.hover_info;
            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Frame: %d", hover_info.frame_index);
            ImGui::Text(
                "Time: %02d:%02d (%.2fs)",
                hover_info.frame_time_clock.minutes,
                hover_info.frame_time_clock.seconds,
                hover_info.frame_time_seconds);
            ImGui::Separator();
            ImGui::TextDisabled("(Click to seek)");
            ImGui::EndTooltip();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Tooltip hover and click share the same resolved frame index.
                inspector_session_->SetCurrentFrame(HandleGoToFrame(hover_info.frame_index, total_frames));
                timeline_bar_state_.last_tracked_frame = inspector_session_->GetCurrentFrame();
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}
