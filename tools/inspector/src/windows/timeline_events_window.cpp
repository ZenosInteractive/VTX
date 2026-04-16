#include "windows/timeline_events_window.h"

#include <imgui.h>
#include <utility>

#include "gui/gui_types.h"
#include "inspector_session.h"
#include "services/footer_summary_service.h"

TimelineEventsWindow::TimelineEventsWindow(std::shared_ptr<InspectorSession> session)
    : ImGuiWindow(VtxGuiNames::TimelineEventsWindow, session)
    , inspector_session_(std::move(session)) {
}

void TimelineEventsWindow::DrawContent() {
    if (!inspector_session_->HasLoadedReplay()) {
        ImGui::TextDisabled("No VTX replay loaded.");
        return;
    }

    const auto view_model = VtxServices::FooterSummaryService::BuildTimelineEventsTableViewModel(
        inspector_session_->GetFooter().events);

    if (view_model.rows.empty()) {
        ImGui::TextDisabled("%s", view_model.empty_message.c_str());
        return;
    }

    ImGui::TextDisabled("%s", view_model.count_label.c_str());
    ImGui::Spacing();

    if (ImGui::BeginTable(
            "EventsTable",
            static_cast<int>(view_model.columns.size()),
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const auto& column : view_model.columns) {
            ImGui::TableSetupColumn(
                column.label.c_str(),
                column.stretch ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed,
                column.width);
        }
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(view_model.rows.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                ImGui::TableNextRow();
                for (size_t col = 0; col < view_model.rows[row].cells.size(); ++col) {
                    ImGui::TableNextColumn();
                    const auto& cell = view_model.rows[row].cells[col];
                    if (cell.highlighted) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", cell.text.c_str());
                    } else if (cell.disabled) {
                        ImGui::TextDisabled("%s", cell.text.c_str());
                    } else {
                        ImGui::Text("%s", cell.text.c_str());
                    }
                }
            }
        }
        ImGui::EndTable();
    }
}
