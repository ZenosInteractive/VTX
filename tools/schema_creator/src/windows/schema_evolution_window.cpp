#include "windows/schema_evolution_window.h"

#include <cfloat>
#include <imgui.h>

#include "schema_creator_session.h"

namespace {

    const char* SeverityToString(VtxServices::ValidationSeverity severity) {
        switch (severity) {
        case VtxServices::ValidationSeverity::Info:
            return "Info";
        case VtxServices::ValidationSeverity::Warning:
            return "Warning";
        case VtxServices::ValidationSeverity::Error:
            return "Error";
        default:
            return "Unknown";
        }
    }

    ImVec4 SeverityColor(VtxServices::ValidationSeverity severity) {
        switch (severity) {
        case VtxServices::ValidationSeverity::Info:
            return ImVec4(0.60f, 0.83f, 1.0f, 1.0f);
        case VtxServices::ValidationSeverity::Warning:
            return ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
        case VtxServices::ValidationSeverity::Error:
            return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        default:
            return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
        }
    }

    std::string BuildLocationLabel(const std::string& struct_name, const std::string& field_name) {
        if (struct_name.empty()) {
            return "-";
        }
        if (field_name.empty()) {
            return struct_name;
        }
        return struct_name + "." + field_name;
    }

} // namespace

SchemaEvolutionWindow::SchemaEvolutionWindow(std::shared_ptr<SchemaCreatorSession> session)
    : ImGuiWindow("Schema Evolution", session)
    , schema_session_(std::move(session)) {}

void SchemaEvolutionWindow::OnRender() {
    if (!schema_session_->IsEvolutionWindowVisible()) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(540.0f, 420.0f), ImGuiCond_FirstUseEver);
    bool is_visible = schema_session_->IsEvolutionWindowVisible();
    if (ImGui::Begin("Schema Evolution", &is_visible)) {
        DrawContent();
    }
    schema_session_->SetEvolutionWindowVisible(is_visible);
    ImGui::End();
}

void SchemaEvolutionWindow::DrawContent() {
    schema_session_->RefreshReportsIfNeeded();

    if (!schema_session_->HasBaseline()) {
        ImGui::TextDisabled("Load or save a baseline schema first to run evolution checks.");
        return;
    }

    const auto& report = schema_session_->GetEvolutionReport();
    ImGui::Text("Result: %s", report.has_breaking_changes
                                  ? "Breaking changes detected (not allowed in next generation mode)"
                                  : "No breaking changes");
    ImGui::Text("Entries: %zu", report.issues.size());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (report.issues.empty()) {
        ImGui::TextDisabled("No schema evolution differences.");
        return;
    }

    if (ImGui::BeginTable("EvolutionIssuesTable", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 230.0f);
        ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& issue : report.issues) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(SeverityColor(issue.severity), "%s", SeverityToString(issue.severity));

            ImGui::TableNextColumn();
            const std::string location = BuildLocationLabel(issue.struct_name, issue.field_name);
            ImGui::TextUnformatted(location.c_str());

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", issue.message.c_str());
        }

        ImGui::EndTable();
    }
}
