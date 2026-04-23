#include "windows/schema_creator_window.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "schema_creator_session.h"

namespace {

    // Converts selected type token to default meta text token.
    std::string BuildMetaTypeToken(const std::string& type_id) {
        if (type_id.empty()) {
            return "";
        }

        std::string out = type_id;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (out == "none") {
            return "";
        }
        return out;
    }

    // Renders editable string using temporary buffer sized for ImGui input widget.
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

    // Renders containerType combo and writes selection into provided field.
    bool DrawContainerCombo(const char* label, std::string& container_type) {
        static const char* kLabels[] = {"None", "Array", "Map"};
        static const char* kValues[] = {"None", "Array", "Map"};

        int current_index = 0;
        for (int i = 0; i < 3; ++i) {
            if (container_type == kValues[i]) {
                current_index = i;
                break;
            }
        }

        bool changed = false;
        if (ImGui::BeginCombo(label, kLabels[current_index])) {
            for (int i = 0; i < 3; ++i) {
                const bool is_selected = current_index == i;
                if (ImGui::Selectable(kLabels[i], is_selected)) {
                    container_type = kValues[i];
                    changed = true;
                }
                if (is_selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    // Renders typeId/keyId preset combo to speed up authoring.
    bool DrawTypePresetCombo(const char* label, std::string& type_id, bool include_none) {
        static const char* kTypePresets[] = {"Int8",   "Int32",  "Int64", "Float",     "Double",     "Bool",
                                             "String", "Vector", "Quat",  "Transform", "FloatRange", "Struct"};
        static const char* kKeyPresets[] = {"None", "Int8", "Int32", "Int64", "Float", "Double", "Bool", "String"};

        const char* preview = type_id.empty() ? "<unset>" : type_id.c_str();
        bool changed = false;
        if (ImGui::BeginCombo(label, preview)) {
            if (include_none) {
                for (const char* preset : kKeyPresets) {
                    const bool is_selected = type_id == preset;
                    if (ImGui::Selectable(preset, is_selected)) {
                        type_id = preset;
                        changed = true;
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            } else {
                for (const char* preset : kTypePresets) {
                    const bool is_selected = type_id == preset;
                    if (ImGui::Selectable(preset, is_selected)) {
                        type_id = preset;
                        changed = true;
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

} // namespace

// Constructs schema editor window bound to schema creator session.
SchemaCreatorWindow::SchemaCreatorWindow(std::shared_ptr<SchemaCreatorSession> session)
    : ImGuiWindow("Property Mappings", session)
    , schema_session_(std::move(session)) {}

// Renders window shell while editor visibility is enabled.
void SchemaCreatorWindow::OnRender() {
    if (!schema_session_->IsMappingsWindowVisible()) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1280.0f, 820.0f), ImGuiCond_FirstUseEver);
    bool is_visible = schema_session_->IsMappingsWindowVisible();
    if (ImGui::Begin("Property Mappings", &is_visible)) {
        DrawContent();
    }
    schema_session_->SetMappingsWindowVisible(is_visible);
    ImGui::End();
}

// Renders main split view: struct list + selected struct editor.
void SchemaCreatorWindow::DrawContent() {
    schema_session_->RefreshReportsIfNeeded();
    // Keep selection indices valid after external document edits.
    ClampSelectionToDocument();

    DrawToolbar();
    ImGui::Separator();
    ImGui::Spacing();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("SchemaCreatorMain", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoPadInnerX,
                          ImVec2(-FLT_MIN, available.y))) {
        ImGui::TableSetupColumn("Structs", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch, 0.70f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        DrawStructListPane();

        ImGui::TableSetColumnIndex(1);
        DrawStructEditorPane();

        ImGui::EndTable();
    }
}

// Renders top toolbar with document status and quick actions.
void SchemaCreatorWindow::DrawToolbar() {
    const std::string path_text = schema_session_->HasActivePath() ? schema_session_->GetActivePath() : "<unsaved>";

    ImGui::TextColored(ImVec4(0.55f, 0.82f, 1.0f, 1.0f), "Schema Document");
    ImGui::Text("Path: %s", path_text.c_str());
    if (schema_session_->HasUnsavedChanges()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.35f, 1.0f), "[Unsaved Changes]");
    }
    ImGui::SameLine();

    bool next_generation_mode = schema_session_->IsNextGenerationMode();
    if (ImGui::Checkbox("Next Generation (additive only)", &next_generation_mode)) {
        schema_session_->SetNextGenerationMode(next_generation_mode);
        schema_session_->AddGuiInfoLog(next_generation_mode ? "Mode changed: Next Generation."
                                                            : "Mode changed: Overwrite Current Generation.");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, save enforces schema evolution checks and blocks removals/signature breaks.");
    }

    ImGui::Spacing();

    if (ImGui::Button("Add Field")) {
        if (AddFieldToSelectedStruct()) {
            schema_session_->AddGuiInfoLog("Field added.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Validate Now")) {
        schema_session_->ValidateNow();
    }
}

// Renders struct list and struct-level create/duplicate/remove actions.
void SchemaCreatorWindow::DrawStructListPane() {
    auto& document = schema_session_->MutableDocument();
    if (ImGui::BeginChild("StructsPaneRoot", ImVec2(0.0f, 0.0f), true)) {
        ImGui::TextColored(ImVec4(0.62f, 0.86f, 0.95f, 1.0f), "Structs");
        ImGui::TextDisabled("Total: %d", static_cast<int>(document.structs.size()));
        ImGui::Spacing();

        InputTextString("New Struct Name", pending_new_struct_name_);
        const bool can_create_struct = !pending_new_struct_name_.empty();
        if (!can_create_struct) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Create Struct")) {
            if (AddStruct(pending_new_struct_name_)) {
                pending_new_struct_name_.clear();
            }
        }
        if (!can_create_struct) {
            ImGui::EndDisabled();
        }

        const bool has_selected_struct =
            selected_struct_index_ >= 0 && selected_struct_index_ < static_cast<int>(document.structs.size());
        ImGui::SameLine();
        if (!has_selected_struct) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Duplicate Struct")) {
            DuplicateSelectedStruct();
        }
        if (!has_selected_struct) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (!has_selected_struct) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Remove Struct")) {
            RemoveSelectedStruct();
        }
        if (!has_selected_struct) {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginChild("StructList", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NavFlattened)) {
            // Left pane owns current struct/field selection state.
            for (int i = 0; i < static_cast<int>(document.structs.size()); ++i) {
                const bool is_selected = selected_struct_index_ == i;
                const std::string label = document.structs[i].struct_name.empty()
                                              ? ("<unnamed struct##" + std::to_string(i) + ">")
                                              : document.structs[i].struct_name;
                if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_struct_index_ = i;
                    selected_field_index_ = document.structs[i].fields.empty() ? -1 : 0;
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

// Renders selected struct details and selected field editor panels.
void SchemaCreatorWindow::DrawStructEditorPane() {
    auto& document = schema_session_->MutableDocument();
    if (selected_struct_index_ < 0 || selected_struct_index_ >= static_cast<int>(document.structs.size())) {
        ImGui::TextDisabled("Select a struct to edit its details and fields.");
        return;
    }

    auto& schema_struct = document.structs[selected_struct_index_];
    if (ImGui::BeginChild("StructEditorPaneRoot", ImVec2(0.0f, 0.0f), true)) {
        ImGui::TextColored(ImVec4(0.62f, 0.86f, 0.95f, 1.0f), "Struct Details");
        if (InputTextString("Struct Name", schema_struct.struct_name)) {
            schema_session_->MarkDirty();
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.62f, 0.86f, 0.95f, 1.0f), "Fields");
        const bool has_selected_field =
            selected_field_index_ >= 0 && selected_field_index_ < static_cast<int>(schema_struct.fields.size());

        if (ImGui::Button("Add New Field")) {
            AddFieldToSelectedStruct();
        }
        ImGui::SameLine();
        if (!has_selected_field) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Duplicate Field")) {
            auto duplicated = schema_struct.fields[selected_field_index_];
            duplicated.name += "_Copy";
            schema_struct.fields.push_back(std::move(duplicated));
            selected_field_index_ = static_cast<int>(schema_struct.fields.size()) - 1;
            schema_session_->MarkDirty();
        }
        if (!has_selected_field) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (!has_selected_field) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Remove Field")) {
            schema_struct.fields.erase(schema_struct.fields.begin() + selected_field_index_);
            selected_field_index_ = std::min(selected_field_index_, static_cast<int>(schema_struct.fields.size()) - 1);
            schema_session_->MarkDirty();
        }
        if (!has_selected_field) {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Step 1: Render field list and field details side-by-side.
        if (ImGui::BeginTable("FieldEditorTable", 2,
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupColumn("Field List", ImGuiTableColumnFlags_WidthStretch, 0.33f);
            ImGui::TableSetupColumn("Field Details", ImGuiTableColumnFlags_WidthStretch, 0.67f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::BeginChild("FieldList", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NavFlattened)) {
                for (int i = 0; i < static_cast<int>(schema_struct.fields.size()); ++i) {
                    const auto& field = schema_struct.fields[i];
                    const std::string label = field.name.empty() ? ("<unnamed field##" + std::to_string(i) + ">")
                                                                 : (field.name + "##" + std::to_string(i));
                    if (ImGui::Selectable(label.c_str(), selected_field_index_ == i,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_field_index_ = i;
                    }
                }
            }
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            if (ImGui::BeginChild("FieldDetails", ImVec2(0.0f, 0.0f), true)) {
                if (selected_field_index_ < 0 ||
                    selected_field_index_ >= static_cast<int>(schema_struct.fields.size())) {
                    ImGui::TextDisabled("Select a field to edit.");
                } else {
                    // Edit the selected field in-place; dirty mark is applied once per frame.
                    auto& field = schema_struct.fields[selected_field_index_];
                    bool changed = false;

                    ImGui::PushID(selected_field_index_);
                    changed |= InputTextString("Name", field.name);

                    const bool type_changed = DrawTypePresetCombo("typeId", field.type_id, false);
                    if (type_changed) {
                        field.meta.type = BuildMetaTypeToken(field.type_id);
                        changed = true;
                    }

                    const bool key_type_changed = DrawTypePresetCombo("keyId", field.key_id, true);
                    if (key_type_changed) {
                        field.meta.key_type = BuildMetaTypeToken(field.key_id);
                        changed = true;
                    }

                    changed |= DrawContainerCombo("containerType", field.container_type);
                    changed |= InputTextString("structType", field.struct_type);

                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.65f, 0.90f, 0.65f, 1.0f), "Meta");
                    changed |= InputTextString("meta.type", field.meta.type);
                    changed |= InputTextString("meta.keyType", field.meta.key_type);
                    changed |= InputTextString("meta.category", field.meta.category);
                    changed |= InputTextString("displayName", field.meta.display_name);
                    changed |= InputTextString("tooltip", field.meta.tooltip);
                    changed |= InputTextString("defaultValue", field.meta.default_value);

                    int version = field.meta.version;
                    if (ImGui::InputInt("meta.version", &version)) {
                        field.meta.version = version;
                        changed = true;
                    }

                    int fixed_array_dim = field.meta.fixed_array_dim;
                    if (ImGui::InputInt("meta.fixedArrayDim", &fixed_array_dim)) {
                        field.meta.fixed_array_dim = fixed_array_dim;
                        changed = true;
                    }
                    ImGui::PopID();

                    if (changed) {
                        schema_session_->MarkDirty();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

// Adds a new struct if the requested name is non-empty and unique.
bool SchemaCreatorWindow::AddStruct(const std::string& struct_name) {
    if (struct_name.empty()) {
        schema_session_->AddGuiWarningLog("Cannot create an empty struct name.");
        return false;
    }

    auto& document = schema_session_->MutableDocument();
    for (const auto& existing_struct : document.structs) {
        if (existing_struct.struct_name == struct_name) {
            schema_session_->AddGuiWarningLog("Struct already exists: " + struct_name);
            return false;
        }
    }

    VtxServices::SchemaStructDoc new_struct;
    new_struct.struct_name = struct_name;
    document.structs.push_back(std::move(new_struct));
    selected_struct_index_ = static_cast<int>(document.structs.size()) - 1;
    selected_field_index_ = -1;
    schema_session_->MarkDirty();
    return true;
}

// Duplicates selected struct and assigns a collision-free copy name.
bool SchemaCreatorWindow::DuplicateSelectedStruct() {
    auto& document = schema_session_->MutableDocument();
    if (selected_struct_index_ < 0 || selected_struct_index_ >= static_cast<int>(document.structs.size())) {
        schema_session_->AddGuiWarningLog("Select a struct before duplicating.");
        return false;
    }

    const auto& source_struct = document.structs[selected_struct_index_];
    VtxServices::SchemaStructDoc duplicated = source_struct;

    std::string base_name = source_struct.struct_name.empty() ? "StructCopy" : (source_struct.struct_name + "_Copy");
    std::string unique_name = base_name;
    int suffix = 2;

    auto is_name_taken = [&document](const std::string& name) {
        for (const auto& schema_struct : document.structs) {
            if (schema_struct.struct_name == name) {
                return true;
            }
        }
        return false;
    };

    // Generate a collision-free name without mutating the source struct.
    while (is_name_taken(unique_name)) {
        unique_name = base_name + std::to_string(suffix++);
    }

    duplicated.struct_name = unique_name;
    document.structs.push_back(std::move(duplicated));
    selected_struct_index_ = static_cast<int>(document.structs.size()) - 1;
    selected_field_index_ = document.structs[selected_struct_index_].fields.empty() ? -1 : 0;
    schema_session_->MarkDirty();
    schema_session_->AddGuiInfoLog("Struct duplicated: " + unique_name);
    return true;
}

// Removes currently selected struct and repairs current selection indices.
bool SchemaCreatorWindow::RemoveSelectedStruct() {
    auto& document = schema_session_->MutableDocument();
    if (selected_struct_index_ < 0 || selected_struct_index_ >= static_cast<int>(document.structs.size())) {
        schema_session_->AddGuiWarningLog("Select a struct before removing.");
        return false;
    }

    const std::string removed_name = document.structs[selected_struct_index_].struct_name;
    document.structs.erase(document.structs.begin() + selected_struct_index_);
    selected_struct_index_ = std::min(selected_struct_index_, static_cast<int>(document.structs.size()) - 1);
    selected_field_index_ = -1;
    schema_session_->MarkDirty();
    schema_session_->AddGuiWarningLog("Struct removed: " + removed_name);
    return true;
}

// Adds a default-initialized field to the selected struct.
bool SchemaCreatorWindow::AddFieldToSelectedStruct() {
    auto& document = schema_session_->MutableDocument();
    if (selected_struct_index_ < 0 || selected_struct_index_ >= static_cast<int>(document.structs.size())) {
        schema_session_->AddGuiWarningLog("Select a struct before adding fields.");
        return false;
    }

    auto& schema_struct = document.structs[selected_struct_index_];
    VtxServices::SchemaFieldDoc new_field;
    new_field.name = "NewField";
    new_field.type_id = "Int32";
    new_field.key_id = "None";
    new_field.container_type = "None";
    new_field.meta.type = "int32";
    new_field.meta.key_type = "";
    new_field.meta.display_name = new_field.name;
    new_field.meta.version = 1;
    schema_struct.fields.push_back(std::move(new_field));
    selected_field_index_ = static_cast<int>(schema_struct.fields.size()) - 1;
    schema_session_->MarkDirty();
    return true;
}

// Clamps struct/field selection indices to current document bounds.
void SchemaCreatorWindow::ClampSelectionToDocument() {
    const auto& document = schema_session_->GetDocument();
    if (document.structs.empty()) {
        selected_struct_index_ = -1;
        selected_field_index_ = -1;
        return;
    }

    selected_struct_index_ = std::clamp(selected_struct_index_, 0, static_cast<int>(document.structs.size()) - 1);
    const auto& selected_struct = document.structs[selected_struct_index_];
    if (selected_struct.fields.empty()) {
        selected_field_index_ = -1;
        return;
    }
    selected_field_index_ = std::clamp(selected_field_index_, 0, static_cast<int>(selected_struct.fields.size()) - 1);
}
