#include "windows/schema_general_info_window.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "schema_creator_session.h"

namespace {

    // Renders editable std::string via fixed temporary buffer for ImGui input APIs.
    bool InputTextString(const char* label, std::string& value, size_t min_buffer_size = 256,
                         ImGuiInputTextFlags flags = 0) {
        const size_t buffer_size = std::max(min_buffer_size, value.size() + 64);
        std::vector<char> buffer(buffer_size, '\0');
        std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());

        if (!ImGui::InputText(label, buffer.data(), buffer.size(), flags)) {
            return false;
        }
        value = buffer.data();
        return true;
    }

} // namespace

// Constructs schema general-info window bound to schema creator session.
SchemaGeneralInfoWindow::SchemaGeneralInfoWindow(std::shared_ptr<SchemaCreatorSession> session)
    : ImGuiWindow("General Info", session)
    , schema_session_(std::move(session)) {}

// Renders window shell only while general-info panel visibility is enabled.
void SchemaGeneralInfoWindow::OnRender() {
    if (!schema_session_->IsGeneralInfoWindowVisible()) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(520.0f, 420.0f), ImGuiCond_FirstUseEver);
    bool is_visible = schema_session_->IsGeneralInfoWindowVisible();
    if (ImGui::Begin("General Info", &is_visible)) {
        DrawContent();
    }
    schema_session_->SetGeneralInfoWindowVisible(is_visible);
    ImGui::End();
}

// Renders high-level schema metadata and aggregate counts.
void SchemaGeneralInfoWindow::DrawContent() {
    schema_session_->RefreshReportsIfNeeded();
    auto& document = schema_session_->MutableDocument();

    // Step 1: Show session/document state summary.
    const std::string path_text = schema_session_->HasActivePath() ? schema_session_->GetActivePath() : "<unsaved>";
    ImGui::Text("Path: %s", path_text.c_str());
    ImGui::Text("Baseline: %s", schema_session_->HasBaseline() ? "Available" : "Not loaded yet");
    ImGui::Text("Unsaved Changes: %s", schema_session_->HasUnsavedChanges() ? "Yes" : "No");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Step 2: Edit root-level schema metadata.
    if (InputTextString("version", document.version)) {
        schema_session_->MarkDirty();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Step 3: Compute and render whole-schema aggregates.
    size_t total_fields = 0;
    for (const auto& schema_struct : document.structs) {
        total_fields += schema_struct.fields.size();
    }

    size_t total_bones = 0;
    for (const auto& mapping : document.bone_mapping) {
        total_bones += mapping.bones.size();
    }

    ImGui::TextColored(ImVec4(0.62f, 0.86f, 0.95f, 1.0f), "Schema Totals");
    ImGui::Text("Structs: %d", static_cast<int>(document.structs.size()));
    ImGui::Text("Fields: %d", static_cast<int>(total_fields));
    ImGui::Text("Buckets: %d", static_cast<int>(document.buckets.size()));
    ImGui::Text("Bone Models: %d", static_cast<int>(document.bone_mapping.size()));
    ImGui::Text("Bone Entries: %d", static_cast<int>(total_bones));
}
