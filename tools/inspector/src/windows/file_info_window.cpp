#include "windows/file_info_window.h"

#include <imgui.h>
#include <utility>

#include "gui/gui_types.h"
#include "inspector_session.h"
#include "services/footer_summary_service.h"

namespace {

bool BeginPropertyTable(const char* id, float label_width = 160.0f) {
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, label_width);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }
    return false;
}

ImVec4 ResolveToneColor(VtxServices::FileInfoTone tone) {
    switch (tone) {
        case VtxServices::FileInfoTone::Highlight:
            return ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
        case VtxServices::FileInfoTone::Success:
            return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
        default:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }
}

} // namespace

FileInfoWindow::FileInfoWindow(std::shared_ptr<InspectorSession> session)
    : ImGuiWindow(VtxGuiNames::FilePropertiesWindow, session)
    , inspector_session_(std::move(session)) {
}

void FileInfoWindow::DrawContent() {
    if (!inspector_session_->HasLoadedReplay()) {
        ImGui::TextDisabled("No VTX replay loaded.");
        return;
    }

    const auto view_model = VtxServices::FooterSummaryService::BuildFileInfoViewModel(
        inspector_session_->current_file_path_,
        inspector_session_->GetFileSizeMb(),
        inspector_session_->GetFormat(),
        inspector_session_->GetHeader(),
        inspector_session_->GetFooter());

    ImGui::TextWrapped("File: %s", view_model.file_path.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    for (size_t i = 0; i < view_model.sections.size(); ++i) {
        const auto& section = view_model.sections[i];
        const ImGuiTreeNodeFlags header_flags = section.default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0;
        if (ImGui::CollapsingHeader(section.title.c_str(), header_flags)) {
            if (section.render_as_raw_text) {
                if (section.raw_text.empty()) {
                    ImGui::TextDisabled("No custom metadata in this file.");
                } else {
                    ImVec2 text_size = ImVec2(-FLT_MIN, 200.0f);
                    ImGui::InputTextMultiline(
                        "##CustomMeta",
                        const_cast<char*>(section.raw_text.c_str()),
                        section.raw_text.size(),
                        text_size,
                        ImGuiInputTextFlags_ReadOnly);
                }
            } else if (BeginPropertyTable((section.title + "Table").c_str())) {
                for (const auto& row : section.rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", row.label.c_str());
                    ImGui::TableNextColumn();
                    if (row.tone == VtxServices::FileInfoTone::Disabled) {
                        ImGui::TextDisabled("%s", row.value.c_str());
                    } else if (row.tone == VtxServices::FileInfoTone::Normal) {
                        ImGui::Text("%s", row.value.c_str());
                    } else {
                        ImGui::TextColored(ResolveToneColor(row.tone), "%s", row.value.c_str());
                    }
                }
                ImGui::EndTable();
            }
        }

        if (i + 1 < view_model.sections.size()) {
            ImGui::Spacing();
            if (section.title == "Custom Metadata") {
                ImGui::Separator();
                ImGui::Spacing();
            }
        }
    }
}
