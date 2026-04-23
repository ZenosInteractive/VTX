#include "windows/buckets_window.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "gui/gui_types.h"
#include "inspector_session.h"

namespace {

    std::string ToLowerCopy(const std::string& value) {
        std::string lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower;
    }

    bool ContainsCaseInsensitive(const std::string& text, const std::string& query_lower) {
        if (query_lower.empty()) {
            return true;
        }
        return ToLowerCopy(text).find(query_lower) != std::string::npos;
    }

    bool EntityMatchesFilter(const VtxServices::EntityListItem& entity, const std::string& query_lower) {
        if (query_lower.empty()) {
            return true;
        }

        if (ContainsCaseInsensitive(entity.entity_id, query_lower)) {
            return true;
        }

        if (ContainsCaseInsensitive(entity.entity_type_name, query_lower)) {
            return true;
        }

        return ContainsCaseInsensitive("type " + std::to_string(entity.entity_type_id), query_lower);
    }

} // namespace

BucketsWindow::BucketsWindow(std::shared_ptr<InspectorSession> session)
    : ImGuiWindow(VtxGuiNames::BucketsWindow, session)
    , inspector_session_(std::move(session)) {}

void BucketsWindow::DrawContent() {
    auto* reader = inspector_session_->GetReader();
    if (!inspector_session_->HasLoadedReplay() || !reader) {
        ImGui::TextDisabled("Load a replay to inspect entities.");
        return;
    }

    auto& state = inspector_session_->MutableEntityInspectorState();
    const auto screen = VtxServices::EntityInspectorViewService::BuildScreen(
        reader, inspector_session_->HasLoadedReplay(), inspector_session_->IsScrubbingTimeline(),
        inspector_session_->GetCurrentFrame(), state);
    state = screen.state;

    if (screen.view_model.is_loading) {
        const ImVec4 color = screen.view_model.status_tone == VtxServices::EntityStatusTone::Warning
                                 ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                 : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(color, "%s", screen.view_model.status_message.c_str());
        if (!screen.view_model.stale_frame_message.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", screen.view_model.stale_frame_message.c_str());
        }
        ImGui::BeginDisabled();
    }

    if (screen.view_model.is_loading) {
        ImGui::Separator();
    }

    if (screen.view_model.has_frame) {
        ImGui::BeginChild("EntityTreePanel", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        DrawEntityListPanel(screen.view_model);
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("%s", screen.view_model.empty_properties_message.c_str());
    }

    if (screen.view_model.is_loading) {
        ImGui::EndDisabled();
    }
}

void BucketsWindow::DrawEntityListPanel(const VtxServices::EntityInspectorViewModel& view_model) {
    // Draw entity buckets with a shared schema-name toggle and text filter.
    if (view_model.buckets.empty()) {
        ImGui::TextDisabled("No buckets in this frame.");
        return;
    }

    bool& show_schema_names = inspector_session_->MutableShowSchemaNames();
    ImGui::Checkbox("Show Schema Names", &show_schema_names);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##EntityFilter", "Filter by UniqueID or Type...", entity_filter_,
                             IM_ARRAYSIZE(entity_filter_));
    ImGui::Separator();

    const std::string filter_query = ToLowerCopy(entity_filter_);
    bool has_visible_entities = false;
    for (const auto& bucket_view : view_model.buckets) {
        std::vector<size_t> visible_indices;
        visible_indices.reserve(bucket_view.entities.size());
        for (size_t i = 0; i < bucket_view.entities.size(); ++i) {
            if (EntityMatchesFilter(bucket_view.entities[i], filter_query)) {
                visible_indices.push_back(i);
            }
        }

        if (visible_indices.empty()) {
            continue;
        }
        has_visible_entities = true;

        if (ImGui::TreeNodeEx(bucket_view.id.c_str(), ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen, "%s",
                              bucket_view.label.c_str())) {
            for (const size_t i : visible_indices) {
                const auto& entity = bucket_view.entities[i];
                ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                if (entity.is_selected) {
                    node_flags |= ImGuiTreeNodeFlags_Selected;
                }

                const std::string node_id = bucket_view.id + "_entity_" + std::to_string(i);
                const std::string type_label = show_schema_names && !entity.entity_type_name.empty()
                                                   ? entity.entity_type_name
                                                   : "Type " + std::to_string(entity.entity_type_id);
                const std::string display_label =
                    "Entity " + std::to_string(i) + " (UniqueID: " + entity.entity_id + ") - " + type_label;

                ImGui::TreeNodeEx(node_id.c_str(), node_flags, "%s", display_label.c_str());
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    auto& state = inspector_session_->MutableEntityInspectorState();
                    state = VtxServices::EntityInspectorViewService::SelectEntity(state, bucket_view.bucket_index,
                                                                                  entity.entity_id);
                    ImGui::SetWindowFocus(VtxGuiNames::EntityDetailsWindow);
                }
            }
            ImGui::TreePop();
        }
    }

    if (!has_visible_entities) {
        ImGui::TextDisabled("No entities match the current filter.");
    }
}
