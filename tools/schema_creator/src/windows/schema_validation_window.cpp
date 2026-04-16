#include "windows/schema_validation_window.h"

#include <cfloat>
#include <imgui.h>

#include "schema_creator_session.h"

namespace {

// Converts validation severity enum to table label.
const char* SeverityToString(VtxServices::ValidationSeverity severity) {
    switch (severity) {
        case VtxServices::ValidationSeverity::Info: return "Info";
        case VtxServices::ValidationSeverity::Warning: return "Warning";
        case VtxServices::ValidationSeverity::Error: return "Error";
        default: return "Unknown";
    }
}

// Returns severity color used for table highlight text.
ImVec4 SeverityColor(VtxServices::ValidationSeverity severity) {
    switch (severity) {
        case VtxServices::ValidationSeverity::Info: return ImVec4(0.60f, 0.83f, 1.0f, 1.0f);
        case VtxServices::ValidationSeverity::Warning: return ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
        case VtxServices::ValidationSeverity::Error: return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        default: return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    }
}

// Builds "Struct.Field" location label for issue table rows.
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

// Constructs schema validation window bound to schema creator session.
SchemaValidationWindow::SchemaValidationWindow(std::shared_ptr<SchemaCreatorSession> session)
    : ImGuiWindow("Schema Validation", session)
    , schema_session_(std::move(session)) {
}

// Renders window shell only while validation panel visibility is enabled.
void SchemaValidationWindow::OnRender() {
    if (!schema_session_->IsValidationWindowVisible()) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(540.0f, 420.0f), ImGuiCond_FirstUseEver);
    bool is_visible = schema_session_->IsValidationWindowVisible();
    if (ImGui::Begin("Schema Validation", &is_visible)) {
        DrawContent();
    }
    schema_session_->SetValidationWindowVisible(is_visible);
    ImGui::End();
}

// Renders validation summary and issue table for current schema document.
void SchemaValidationWindow::DrawContent() {
    schema_session_->RefreshReportsIfNeeded();
    const auto& report = schema_session_->GetValidationReport();

    int error_count = 0;
    int warning_count = 0;
    // Precompute counters so summary and table are always in sync.
    for (const auto& issue : report.issues) {
        if (issue.severity == VtxServices::ValidationSeverity::Error) {
            ++error_count;
        } else if (issue.severity == VtxServices::ValidationSeverity::Warning) {
            ++warning_count;
        }
    }

    ImGui::Text("Result: %s", report.is_valid ? "Valid (no blocking errors)" : "Invalid");
    ImGui::Text("Errors: %d  Warnings: %d  Total Issues: %zu", error_count, warning_count, report.issues.size());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (report.issues.empty()) {
        ImGui::TextDisabled("No validation issues.");
        return;
    }

    // Step 1: Build and render issue table rows.
    if (ImGui::BeginTable(
            "ValidationIssuesTable",
            3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 230.0f);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
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
