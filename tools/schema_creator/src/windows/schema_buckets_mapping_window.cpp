#include "windows/schema_buckets_mapping_window.h"

#include <algorithm>
#include <cfloat>
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

// Constructs buckets/bone-mapping window bound to schema creator session.
SchemaBucketsMappingWindow::SchemaBucketsMappingWindow(std::shared_ptr<SchemaCreatorSession> session)
    : ImGuiWindow("Buckets & Bone Mapping", session)
    , schema_session_(std::move(session)) {}

// Renders window shell only while buckets/mapping panel visibility is enabled.
void SchemaBucketsMappingWindow::OnRender() {
    if (!schema_session_->IsBucketsMappingWindowVisible()) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 540.0f), ImGuiCond_FirstUseEver);
    bool is_visible = schema_session_->IsBucketsMappingWindowVisible();
    if (ImGui::Begin("Buckets & Bone Mapping", &is_visible)) {
        DrawContent();
    }
    schema_session_->SetBucketsMappingWindowVisible(is_visible);
    ImGui::End();
}

// Renders bucket management and bone-mapping management panels.
void SchemaBucketsMappingWindow::DrawContent() {
    schema_session_->RefreshReportsIfNeeded();
    auto& document = schema_session_->MutableDocument();
    ClampSelectionIndices();

    if (ImGui::BeginTable("BucketsMappingRoot", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Buckets", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("Bone Mapping", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        ImGui::TableNextRow();

        // Step 1: Buckets panel.
        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("BucketsPane", ImVec2(0.0f, 0.0f), true)) {
            ImGui::TextColored(ImVec4(0.62f, 0.86f, 0.95f, 1.0f), "Buckets");
            ImGui::TextDisabled("Total: %d", static_cast<int>(document.buckets.size()));
            ImGui::Spacing();

            InputTextString("New Bucket", pending_bucket_name_);
            const bool can_add_bucket = !pending_bucket_name_.empty();
            if (!can_add_bucket) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Add Bucket")) {
                if (AddBucket(pending_bucket_name_)) {
                    pending_bucket_name_.clear();
                }
            }
            if (!can_add_bucket) {
                ImGui::EndDisabled();
            }

            const bool has_selected_bucket =
                selected_bucket_index_ >= 0 && selected_bucket_index_ < static_cast<int>(document.buckets.size());
            ImGui::SameLine();
            if (!has_selected_bucket) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Remove Bucket")) {
                RemoveSelectedBucket();
            }
            if (!has_selected_bucket) {
                ImGui::EndDisabled();
            }

            if (has_selected_bucket) {
                if (InputTextString("Selected Bucket", document.buckets[selected_bucket_index_])) {
                    schema_session_->MarkDirty();
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::BeginChild("BucketList", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NavFlattened)) {
                for (int i = 0; i < static_cast<int>(document.buckets.size()); ++i) {
                    const bool is_selected = selected_bucket_index_ == i;
                    const std::string label = document.buckets[i].empty()
                                                  ? ("<unnamed bucket##" + std::to_string(i) + ">")
                                                  : (document.buckets[i] + "##" + std::to_string(i));
                    if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        selected_bucket_index_ = i;
                    }
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        // Step 2: Bone mapping panel.
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("BoneMappingPane", ImVec2(0.0f, 0.0f), true)) {
            ImGui::TextColored(ImVec4(0.62f, 0.86f, 0.95f, 1.0f), "Bone Mapping");
            ImGui::TextDisabled("Models: %d", static_cast<int>(document.bone_mapping.size()));
            ImGui::Spacing();

            InputTextString("New Model", pending_model_name_);
            const bool can_add_model = !pending_model_name_.empty();
            if (!can_add_model) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Add Model")) {
                if (AddBoneModel(pending_model_name_)) {
                    pending_model_name_.clear();
                }
            }
            if (!can_add_model) {
                ImGui::EndDisabled();
            }

            const bool has_selected_model =
                selected_model_index_ >= 0 && selected_model_index_ < static_cast<int>(document.bone_mapping.size());
            ImGui::SameLine();
            if (!has_selected_model) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Remove Model")) {
                RemoveSelectedBoneModel();
            }
            if (!has_selected_model) {
                ImGui::EndDisabled();
            }

            if (has_selected_model) {
                auto& selected_mapping = document.bone_mapping[selected_model_index_];
                if (InputTextString("Selected Model", selected_mapping.model_name)) {
                    schema_session_->MarkDirty();
                }

                ImGui::Spacing();
                InputTextString("New Bone", pending_bone_name_);
                const bool can_add_bone = !pending_bone_name_.empty();
                if (!can_add_bone) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Add Bone")) {
                    if (AddBoneToSelectedModel(pending_bone_name_)) {
                        pending_bone_name_.clear();
                    }
                }
                if (!can_add_bone) {
                    ImGui::EndDisabled();
                }

                const bool has_selected_bone =
                    selected_bone_index_ >= 0 && selected_bone_index_ < static_cast<int>(selected_mapping.bones.size());
                ImGui::SameLine();
                if (!has_selected_bone) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Remove Bone")) {
                    RemoveSelectedBone();
                }
                if (!has_selected_bone) {
                    ImGui::EndDisabled();
                }

                if (has_selected_bone) {
                    if (InputTextString("Selected Bone", selected_mapping.bones[selected_bone_index_])) {
                        schema_session_->MarkDirty();
                    }
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::BeginTable("BoneMappingTables", 2,
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y))) {
                ImGui::TableSetupColumn("Models", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("Bones", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ImGui::BeginChild("ModelList", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NavFlattened)) {
                    for (int i = 0; i < static_cast<int>(document.bone_mapping.size()); ++i) {
                        const auto& mapping = document.bone_mapping[i];
                        const bool is_selected = selected_model_index_ == i;
                        const std::string label = mapping.model_name.empty()
                                                      ? ("<unnamed model##" + std::to_string(i) + ">")
                                                      : (mapping.model_name + "##" + std::to_string(i));
                        if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                            selected_model_index_ = i;
                            selected_bone_index_ = document.bone_mapping[i].bones.empty() ? -1 : 0;
                        }
                    }
                }
                ImGui::EndChild();

                ImGui::TableSetColumnIndex(1);
                if (ImGui::BeginChild("BoneList", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NavFlattened)) {
                    if (has_selected_model) {
                        const auto& selected_mapping = document.bone_mapping[selected_model_index_];
                        for (int i = 0; i < static_cast<int>(selected_mapping.bones.size()); ++i) {
                            const bool is_selected = selected_bone_index_ == i;
                            const std::string label = selected_mapping.bones[i].empty()
                                                          ? ("<unnamed bone##" + std::to_string(i) + ">")
                                                          : (selected_mapping.bones[i] + "##" + std::to_string(i));
                            if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                                selected_bone_index_ = i;
                            }
                        }
                    } else {
                        ImGui::TextDisabled("Select a model to view bones.");
                    }
                }
                ImGui::EndChild();
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::EndTable();
    }
}

// Keeps local selected indices within valid bucket/model/bone bounds.
void SchemaBucketsMappingWindow::ClampSelectionIndices() {
    const auto& document = schema_session_->GetDocument();
    if (document.buckets.empty()) {
        selected_bucket_index_ = -1;
    } else {
        selected_bucket_index_ = std::clamp(selected_bucket_index_, 0, static_cast<int>(document.buckets.size()) - 1);
    }

    if (document.bone_mapping.empty()) {
        selected_model_index_ = -1;
        selected_bone_index_ = -1;
        return;
    }

    selected_model_index_ = std::clamp(selected_model_index_, 0, static_cast<int>(document.bone_mapping.size()) - 1);
    const auto& mapping = document.bone_mapping[selected_model_index_];
    if (mapping.bones.empty()) {
        selected_bone_index_ = -1;
        return;
    }
    selected_bone_index_ = std::clamp(selected_bone_index_, 0, static_cast<int>(mapping.bones.size()) - 1);
}

// Adds a bucket entry if the requested name is non-empty and unique.
bool SchemaBucketsMappingWindow::AddBucket(const std::string& bucket_name) {
    if (bucket_name.empty()) {
        schema_session_->AddGuiWarningLog("Cannot create an empty bucket name.");
        return false;
    }

    auto& document = schema_session_->MutableDocument();
    for (const auto& existing : document.buckets) {
        if (existing == bucket_name) {
            schema_session_->AddGuiWarningLog("Bucket already exists: " + bucket_name);
            return false;
        }
    }

    document.buckets.push_back(bucket_name);
    selected_bucket_index_ = static_cast<int>(document.buckets.size()) - 1;
    schema_session_->MarkDirty();
    return true;
}

// Removes currently selected bucket and repairs local selection index.
bool SchemaBucketsMappingWindow::RemoveSelectedBucket() {
    auto& document = schema_session_->MutableDocument();
    if (selected_bucket_index_ < 0 || selected_bucket_index_ >= static_cast<int>(document.buckets.size())) {
        schema_session_->AddGuiWarningLog("Select a bucket before removing.");
        return false;
    }

    const std::string removed_name = document.buckets[selected_bucket_index_];
    document.buckets.erase(document.buckets.begin() + selected_bucket_index_);
    selected_bucket_index_ = std::min(selected_bucket_index_, static_cast<int>(document.buckets.size()) - 1);
    schema_session_->MarkDirty();
    schema_session_->AddGuiWarningLog("Bucket removed: " + removed_name);
    return true;
}

// Adds a bone model entry if the requested model key is non-empty and unique.
bool SchemaBucketsMappingWindow::AddBoneModel(const std::string& model_name) {
    if (model_name.empty()) {
        schema_session_->AddGuiWarningLog("Cannot create an empty model name.");
        return false;
    }

    auto& document = schema_session_->MutableDocument();
    for (const auto& mapping : document.bone_mapping) {
        if (mapping.model_name == model_name) {
            schema_session_->AddGuiWarningLog("Bone model already exists: " + model_name);
            return false;
        }
    }

    VtxServices::SchemaBoneMappingDoc mapping;
    mapping.model_name = model_name;
    document.bone_mapping.push_back(std::move(mapping));
    selected_model_index_ = static_cast<int>(document.bone_mapping.size()) - 1;
    selected_bone_index_ = -1;
    schema_session_->MarkDirty();
    return true;
}

// Removes currently selected bone model and repairs local selection indices.
bool SchemaBucketsMappingWindow::RemoveSelectedBoneModel() {
    auto& document = schema_session_->MutableDocument();
    if (selected_model_index_ < 0 || selected_model_index_ >= static_cast<int>(document.bone_mapping.size())) {
        schema_session_->AddGuiWarningLog("Select a model before removing.");
        return false;
    }

    const std::string removed_name = document.bone_mapping[selected_model_index_].model_name;
    document.bone_mapping.erase(document.bone_mapping.begin() + selected_model_index_);
    selected_model_index_ = std::min(selected_model_index_, static_cast<int>(document.bone_mapping.size()) - 1);
    selected_bone_index_ = -1;
    schema_session_->MarkDirty();
    schema_session_->AddGuiWarningLog("Bone model removed: " + removed_name);
    return true;
}

// Adds a bone entry to the currently selected model.
bool SchemaBucketsMappingWindow::AddBoneToSelectedModel(const std::string& bone_name) {
    if (bone_name.empty()) {
        schema_session_->AddGuiWarningLog("Cannot add an empty bone name.");
        return false;
    }

    auto& document = schema_session_->MutableDocument();
    if (selected_model_index_ < 0 || selected_model_index_ >= static_cast<int>(document.bone_mapping.size())) {
        schema_session_->AddGuiWarningLog("Select a model before adding bones.");
        return false;
    }

    auto& mapping = document.bone_mapping[selected_model_index_];
    for (const auto& existing_bone : mapping.bones) {
        if (existing_bone == bone_name) {
            schema_session_->AddGuiWarningLog("Bone already exists in model: " + bone_name);
            return false;
        }
    }

    mapping.bones.push_back(bone_name);
    selected_bone_index_ = static_cast<int>(mapping.bones.size()) - 1;
    schema_session_->MarkDirty();
    return true;
}

// Removes currently selected bone from the currently selected model.
bool SchemaBucketsMappingWindow::RemoveSelectedBone() {
    auto& document = schema_session_->MutableDocument();
    if (selected_model_index_ < 0 || selected_model_index_ >= static_cast<int>(document.bone_mapping.size())) {
        schema_session_->AddGuiWarningLog("Select a model before removing bones.");
        return false;
    }

    auto& mapping = document.bone_mapping[selected_model_index_];
    if (selected_bone_index_ < 0 || selected_bone_index_ >= static_cast<int>(mapping.bones.size())) {
        schema_session_->AddGuiWarningLog("Select a bone before removing.");
        return false;
    }

    const std::string removed_name = mapping.bones[selected_bone_index_];
    mapping.bones.erase(mapping.bones.begin() + selected_bone_index_);
    selected_bone_index_ = std::min(selected_bone_index_, static_cast<int>(mapping.bones.size()) - 1);
    schema_session_->MarkDirty();
    schema_session_->AddGuiWarningLog("Bone removed: " + removed_name);
    return true;
}
