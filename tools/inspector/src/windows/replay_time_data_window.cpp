#include "windows/replay_time_data_window.h"

#include <algorithm>
#include <imgui.h>
#include <string>
#include <utility>

#include "gui/gui_types.h"
#include "inspector_session.h"
#include "services/footer_summary_service.h"

namespace {

    enum class TimeDataTabKind : int {
        GameTimestamps = 0,
        UtcTimestamps = 1,
        Gaps = 2,
        Segments = 3,
    };

    struct TimeDataTabInteraction {
        bool should_seek = false;
        int seek_frame = 0;
        bool should_switch_to_game_timestamp_tab = false;
        bool consumed_scroll_request = false;
        bool tab_opened = false;
    };

    // Clamps requested frame index into currently loaded replay frame bounds.
    int ClampFrameIndex(int frame_index, int total_frames) {
        if (total_frames <= 0) {
            return 0;
        }
        return std::clamp(frame_index, 0, total_frames - 1);
    }

    // Resolves clicked row to target replay frame index for each time-data tab.
    int ResolveFrameForTabRow(TimeDataTabKind tab_kind, int row, const VTX::ReplayTimeData& time_data) {
        switch (tab_kind) {
        case TimeDataTabKind::GameTimestamps:
        case TimeDataTabKind::UtcTimestamps:
            return row;
        case TimeDataTabKind::Gaps:
            if (row >= 0 && row < static_cast<int>(time_data.gaps.size())) {
                return time_data.gaps[static_cast<size_t>(row)];
            }
            return row;
        case TimeDataTabKind::Segments:
            if (row >= 0 && row < static_cast<int>(time_data.segments.size())) {
                return time_data.segments[static_cast<size_t>(row)];
            }
            return row;
        }
        return row;
    }

    // Draws a styled bullet entry for the time-data guide panel.
    void DrawTimeDataGuideItem(const char* title, const char* description) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.72f, 0.96f, 1.0f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Indent();
        ImGui::PushTextWrapPos();
        ImGui::TextWrapped("%s", description);
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
        ImGui::Spacing();
    }

    // Draws a quick reference for the meaning and usage of each replay time stream.
    void DrawTimeDataGuide() {
        ImGui::SetNextItemOpen(false, ImGuiCond_FirstUseEver);
        if (ImGui::CollapsingHeader("Time Data Guide")) {
            if (ImGui::BeginChild("TimeDataGuideBody", ImVec2(0.0f, 220.0f), true)) {
                ImGui::Spacing();
                DrawTimeDataGuideItem(
                    "GameTime",
                    "The internal timer captured directly from the source. It can increase, decrease, or jump "
                    "forward/backward depending on the source, and it can be affected by engine time dilation. "
                    "Use this mainly for display purposes. When the source supports it, it can also help binary "
                    "search to locate the correct frame.");
                DrawTimeDataGuideItem(
                    "UTC Timestamp",
                    "The time each frame was captured. This is guaranteed to be monotonically increasing. If the "
                    "source start UTC is known, timestamps begin from that value; otherwise they begin when data "
                    "is written to VTX. Use this for internal playback timing by making values relative to the "
                    "first entry.");
                DrawTimeDataGuideItem(
                    "Gaps", "Frame numbers where a gap is detected in UTC timestamps. This can represent pauses, frame "
                            "loss, or similar capture interruptions.");
                DrawTimeDataGuideItem(
                    "Segments",
                    "Frame numbers where game-time direction changes, or where developers manually mark important "
                    "parts of the data. This is commonly used to indicate new rounds.");
            }
            ImGui::EndChild();
        }
    }

    // Renders one time-data tab as a clickable table and emits navigation actions.
    TimeDataTabInteraction DrawTimeDataTab(const VtxServices::ReplayTimeTabViewModel& tab, const char* table_id,
                                           TimeDataTabKind tab_kind, const VTX::ReplayTimeData& time_data,
                                           int total_frames, int highlighted_frame_row,
                                           bool should_scroll_to_highlighted_game_row, bool should_select_tab) {
        TimeDataTabInteraction interaction;
        const ImGuiTabItemFlags tab_flags = should_select_tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(tab.label.c_str(), nullptr, tab_flags)) {
            interaction.tab_opened = true;
            if (tab.values.empty()) {
                ImGui::TextDisabled("No entries.");
            } else if (ImGui::BeginTable(table_id, 2,
                                         ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                             ImGuiTableFlags_Resizable,
                                         ImVec2(0, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn(tab.value_column_label.c_str(), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                const bool render_all_rows_for_scroll =
                    should_scroll_to_highlighted_game_row && tab_kind == TimeDataTabKind::GameTimestamps;

                auto render_row = [&](int row) {
                    const bool is_highlighted_timeline_row =
                        (tab_kind == TimeDataTabKind::GameTimestamps || tab_kind == TimeDataTabKind::UtcTimestamps) &&
                        (row == highlighted_frame_row);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    const std::string selectable_id = "[" + std::to_string(row) + "]##TimeDataRow_" +
                                                      std::to_string(static_cast<int>(tab_kind)) + "_" +
                                                      std::to_string(row);
                    if (ImGui::Selectable(selectable_id.c_str(), is_highlighted_timeline_row,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        interaction.should_seek = true;
                        interaction.seek_frame =
                            ClampFrameIndex(ResolveFrameForTabRow(tab_kind, row, time_data), total_frames);
                        interaction.should_switch_to_game_timestamp_tab =
                            tab_kind == TimeDataTabKind::Gaps || tab_kind == TimeDataTabKind::Segments;
                    }

                    if ((tab_kind == TimeDataTabKind::GameTimestamps) && is_highlighted_timeline_row &&
                        should_scroll_to_highlighted_game_row) {
                        ImGui::SetScrollHereY(0.25f);
                        interaction.consumed_scroll_request = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", tab.values[row].c_str());
                };

                if (render_all_rows_for_scroll) {
                    for (int row = 0; row < static_cast<int>(tab.values.size()); ++row) {
                        render_row(row);
                    }
                } else {
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(tab.values.size()));
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                            render_row(row);
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        return interaction;
    }

} // namespace

ReplayTimeDataWindow::ReplayTimeDataWindow(std::shared_ptr<InspectorSession> session)
    : ImGuiWindow(VtxGuiNames::ReplayTimeDataWindow, session)
    , inspector_session_(std::move(session)) {}

void ReplayTimeDataWindow::DrawContent() {
    if (!inspector_session_->HasLoadedReplay()) {
        ImGui::TextDisabled("No VTX replay loaded.");
        return;
    }

    // Step 1: Build the view model with the current display format.
    TimeDisplayFormat current_format = inspector_session_->GetTimeDisplayFormat();

    auto view_model = VtxServices::FooterSummaryService::BuildReplayTimeViewModel(inspector_session_->GetFooter().times,
                                                                                  current_format);

    // Step 2: Draw summary and format controls.
    if (ImGui::BeginTable("TimeDataSummary", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Array", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Entries", ImGuiTableColumnFlags_WidthStretch);

        for (const auto& row : view_model.summary_rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", row.label.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", row.value.c_str());
        }
        ImGui::EndTable();
    }
    // Step 3: Explain each time stream before rendering the detailed tables.
    DrawTimeDataGuide();
    ImGui::Separator();

    // Step 4: Draw formatting controls.
    ImGui::Spacing();
    bool formatted = (current_format == TimeDisplayFormat::Formatted);
    if (ImGui::Checkbox("Formatted", &formatted)) {
        current_format = formatted ? TimeDisplayFormat::Formatted : TimeDisplayFormat::Ticks;
        inspector_session_->SetTimeDisplayFormat(current_format);
        view_model = VtxServices::FooterSummaryService::BuildReplayTimeViewModel(inspector_session_->GetFooter().times,
                                                                                 current_format);
    }
    ImGui::Separator();

    // Step 5: Draw per-stream value tables and handle row navigation interactions.
    const auto& time_data = inspector_session_->GetFooter().times;
    const int total_frames = inspector_session_->GetTotalFrames();
    const int current_frame = inspector_session_->GetCurrentFrame();
    const int max_game_timestamp_row = static_cast<int>(time_data.game_time.size()) - 1;
    if (max_game_timestamp_row < 0) {
        highlighted_game_timestamp_row_ = -1;
        pending_switch_to_game_timestamp_tab_ = false;
        pending_scroll_to_highlighted_game_row_ = false;
    } else {
        highlighted_game_timestamp_row_ = std::clamp(current_frame, 0, max_game_timestamp_row);
    }

    if (ImGui::BeginTabBar("ReplayTimeDataTabs")) {
        static constexpr const char* kTableIds[] = {"##GTTable", "##UTCTable", "##GapsTable", "##SegsTable"};
        for (size_t i = 0; i < view_model.tabs.size() && i < std::size(kTableIds); ++i) {
            const TimeDataTabKind tab_kind = static_cast<TimeDataTabKind>(i);
            const bool should_select_tab =
                pending_switch_to_game_timestamp_tab_ && (tab_kind == TimeDataTabKind::GameTimestamps);
            const bool should_scroll_to_highlighted_game_row =
                pending_scroll_to_highlighted_game_row_ && (tab_kind == TimeDataTabKind::GameTimestamps);

            const TimeDataTabInteraction interaction = DrawTimeDataTab(
                view_model.tabs[i], kTableIds[i], tab_kind, time_data, total_frames, highlighted_game_timestamp_row_,
                should_scroll_to_highlighted_game_row, should_select_tab);

            if (interaction.should_seek) {
                inspector_session_->SetCurrentFrame(interaction.seek_frame);
            }

            if (interaction.should_switch_to_game_timestamp_tab) {
                if (max_game_timestamp_row >= 0) {
                    highlighted_game_timestamp_row_ = std::clamp(interaction.seek_frame, 0, max_game_timestamp_row);
                    pending_switch_to_game_timestamp_tab_ = true;
                    pending_scroll_to_highlighted_game_row_ = true;
                }
            }

            if (should_select_tab && interaction.tab_opened) {
                pending_switch_to_game_timestamp_tab_ = false;
            }

            if (interaction.consumed_scroll_request) {
                pending_scroll_to_highlighted_game_row_ = false;
            }
        }
        ImGui::EndTabBar();
    }
}
