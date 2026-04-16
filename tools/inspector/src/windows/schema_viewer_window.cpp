#include "windows/schema_viewer_window.h"

#include <imgui.h>
#include <string>
#include <utility>

#include "gui/gui_types.h"
#include "gui/portable-file-dialogs.h"
#include "inspector_session.h"
#include "services/field_type_utils.h"
#include "services/schema_view_service.h"

namespace {

ImVec4 ResolveFieldTypeColor(VTX::FieldType type_id) {
    switch (type_id) {
        case VTX::FieldType::Bool: return ImVec4(0.86f, 0.32f, 0.32f, 1.0f);
        case VTX::FieldType::Int8: return ImVec4(0.23f, 0.68f, 0.37f, 1.0f);
        case VTX::FieldType::Int32: return ImVec4(0.16f, 0.62f, 0.56f, 1.0f);
        case VTX::FieldType::Int64: return ImVec4(0.24f, 0.54f, 0.86f, 1.0f);
        case VTX::FieldType::Float: return ImVec4(0.72f, 0.74f, 0.22f, 1.0f);
        case VTX::FieldType::Double: return ImVec4(0.82f, 0.60f, 0.20f, 1.0f);
        case VTX::FieldType::String: return ImVec4(0.72f, 0.45f, 0.86f, 1.0f);
        case VTX::FieldType::Vector: return ImVec4(0.19f, 0.74f, 0.78f, 1.0f);
        case VTX::FieldType::Quat: return ImVec4(0.38f, 0.64f, 0.93f, 1.0f);
        case VTX::FieldType::Transform: return ImVec4(0.94f, 0.44f, 0.22f, 1.0f);
        case VTX::FieldType::FloatRange: return ImVec4(0.58f, 0.78f, 0.30f, 1.0f);
        case VTX::FieldType::Struct: return ImVec4(0.20f, 0.33f, 0.64f, 1.0f);
        case VTX::FieldType::Enum: return ImVec4(0.60f, 0.52f, 0.86f, 1.0f);
        default: return ImVec4(0.62f, 0.62f, 0.68f, 1.0f);
    }
}

bool ShouldOpenHighlightedStruct(const VtxServices::SchemaHighlightState& highlight, const std::string& struct_name) {
    return VtxServices::SchemaViewService::IsHighlightActive(highlight) &&
        highlight.do_scroll &&
        highlight.struct_name == struct_name;
}

bool ConsumeScrollRequest(VtxServices::SchemaHighlightState& highlight) {
    if (!highlight.do_scroll) {
        return false;
    }

    highlight.do_scroll = false;
    return true;
}

float ComputeHighlightAlpha(const VtxServices::SchemaHighlightState& highlight) {
    if (!VtxServices::SchemaViewService::IsHighlightActive(highlight)) {
        return 0.0f;
    }

    return highlight.time_remaining / 2.0f;
}

bool DrawStructLinkButton(const std::string& label, const std::string& id_suffix, const ImVec4& text_color) {
    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.18f, 0.24f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.31f, 0.45f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.42f, 0.64f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
    const bool clicked = ImGui::Button((label + id_suffix).c_str());
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return clicked;
}

} // namespace

SchemaViewerWindow::SchemaViewerWindow(std::shared_ptr<InspectorSession> session)
    : ImGuiWindow(VtxGuiNames::SchemaViewerWindow, session)
    , inspector_session_(std::move(session)) {
}

void SchemaViewerWindow::DrawTypeIcon(VTX::FieldType type_id, VTX::FieldContainerType container_type, float size) const {
    const ImVec4 base = ResolveFieldTypeColor(type_id);
    const float icon = size > 0.0f ? size : std::max(11.0f, ImGui::GetTextLineHeight() - 2.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool is_array = container_type == VTX::FieldContainerType::Array;
    const bool is_map = container_type == VTX::FieldContainerType::Map;

    const ImVec4 array_tint = ImVec4(
        std::min(base.x + 0.14f, 1.0f),
        std::min(base.y + 0.14f, 1.0f),
        std::min(base.z + 0.08f, 1.0f),
        1.0f);

    if (is_array) {
        draw->AddRectFilled(ImVec2(p.x + 2.5f, p.y + 2.5f), ImVec2(p.x + 2.5f + icon, p.y + 2.5f + icon), ImGui::ColorConvertFloat4ToU32(base), 3.0f);
        draw->AddRect(ImVec2(p.x + 2.5f, p.y + 2.5f), ImVec2(p.x + 2.5f + icon, p.y + 2.5f + icon), IM_COL32(16, 18, 24, 220), 3.0f, 0, 1.0f);

        draw->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + icon, p.y + icon), ImGui::ColorConvertFloat4ToU32(array_tint), 3.0f);
        draw->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + icon, p.y + icon), IM_COL32(16, 18, 24, 230), 3.0f, 0, 1.0f);
    } else if (is_map) {
        draw->AddCircleFilled(ImVec2(p.x + icon * 0.5f, p.y + icon * 0.5f), icon * 0.48f, ImGui::ColorConvertFloat4ToU32(base), 18);
        draw->AddCircle(ImVec2(p.x + icon * 0.5f, p.y + icon * 0.5f), icon * 0.48f, IM_COL32(16, 18, 24, 230), 18, 1.0f);
    } else {
        draw->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + icon, p.y + icon), ImGui::ColorConvertFloat4ToU32(base), 3.0f);
        draw->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + icon, p.y + icon), IM_COL32(16, 18, 24, 230), 3.0f, 0, 1.0f);
    }

    ImGui::Dummy(ImVec2(icon + 4.0f, icon));
    if (ImGui::IsItemHovered()) {
        if (is_array) {
            ImGui::SetTooltip("Array");
        } else if (is_map) {
            ImGui::SetTooltip("Map");
        }
    }
}

void SchemaViewerWindow::DrawContent() {
    auto* reader = inspector_session_->GetReader();
    if (!inspector_session_->HasLoadedReplay() || !reader) {
        ImGui::TextDisabled("Load a replay to view its schema.");
        return;
    }

    VTX::ContextualSchema schema = reader->GetContextualSchema();
    const auto& parsed_cache = reader->GetPropertyAddressCache();

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Identifier: %s", schema.data_identifier.c_str());
    ImGui::Text("Version: %d (%s)", schema.data_version, schema.data_version_string.c_str());
    ImGui::TextDisabled("Property Mapping: %zu bytes", schema.property_mapping.size());

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f);
    if (ImGui::Button("Export JSON", ImVec2(90.0f, 0.0f))) {
        auto dest = pfd::save_file("Export Schema JSON", "schema.json",
            { "JSON Files (.json)", "*.json", "All Files", "*" });
        if (!dest.result().empty()) {
            const bool success = VtxServices::SchemaViewService::ExportJsonToFile(dest.result(), schema.property_mapping);
            if (success) {
                inspector_session_->AddGuiInfoLog("Schema JSON exported successfully.");
            } else {
                inspector_session_->AddGuiErrorLog("Failed to export schema JSON.");
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Total Structs Mapped: %zu", parsed_cache.structs.size());
    ImGui::Spacing();
    
    auto& highlight = inspector_session_->MutableSchemaHighlight();
    VtxServices::SchemaViewService::TickHighlight(highlight, ImGui::GetIO().DeltaTime);
    const bool is_highlight_active = VtxServices::SchemaViewService::IsHighlightActive(highlight);

    for (const auto& [struct_id, struct_cache] : parsed_cache.structs) {
        const std::string& struct_name = struct_cache.name;
        
        bool struct_is_highlighted = is_highlight_active && (highlight.struct_name == struct_name);
        if (highlight.do_scroll) {
            ImGui::SetNextItemOpen(struct_name == highlight.struct_name, ImGuiCond_Always);
        }

        if (struct_is_highlighted) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.25f, 1.0f));
        }
        const bool open = ImGui::TreeNode(struct_name.c_str(), "%s (%zu properties)", struct_name.c_str(), struct_cache.properties.size());
        if (struct_is_highlighted) {
            ImGui::PopStyleColor();
        }
        if (struct_is_highlighted && highlight.property_name.empty() && ConsumeScrollRequest(highlight)) {
            ImGui::SetScrollHereY(0.2f);
        }

        if (open) {
            ImGui::PushID(struct_id);
            if (ImGui::BeginTable("StructPropsTable", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Property Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Array Index", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableHeadersRow();

                const auto ordered_properties = struct_cache.GetPropertiesInOrder();
                for (const auto& property_view : ordered_properties) {
                    const std::string_view prop_name = property_view.name;
                    const auto& address = *property_view.address;
                    bool is_target_prop = struct_is_highlighted && (highlight.property_name == prop_name);
                    
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    
                    if (is_target_prop) {
                        float alpha = ComputeHighlightAlpha(highlight); 
                        int alpha_int = static_cast<int>(255.0f * alpha); 
                        
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(220, 160, 0, alpha_int));
                        
                        if (ConsumeScrollRequest(highlight)) {
                            ImGui::SetScrollHereY(0.5f); 
                        }
                    }

                    const VTX::FieldContainerType container_type =
                        VtxServices::SchemaViewService::ResolveContainerType(address);
                    const std::string display_type =
                        VtxServices::SchemaViewService::ResolveDisplayType(address);

                    DrawTypeIcon(address.type_id, container_type);
                    ImGui::SameLine(0.0f, 4.0f);
                    const ImVec4 color = ResolveFieldTypeColor(address.type_id);
                    const bool is_known_struct = !address.child_type_name.empty() &&
                        parsed_cache.name_to_id.find(address.child_type_name) != parsed_cache.name_to_id.end();
                    if (is_known_struct) {
                        if (DrawStructLinkButton(display_type, "##SchemaType_" + struct_name + "_" + std::string(prop_name), color)) {
                            VtxServices::SchemaViewService::RequestHighlight(highlight, address.child_type_name, "");
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Open struct: %s", address.child_type_name.c_str());
                        }
                    } else {
                        ImGui::TextColored(color, "%s", display_type.c_str());
                    }

                    ImGui::TableNextColumn();
                    if (is_target_prop) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                    }
                    ImGui::TextUnformatted(prop_name.data(), prop_name.data() + prop_name.size());
                    if (is_target_prop) {
                        ImGui::PopStyleColor();
                    }

                    ImGui::TableNextColumn(); 
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[%d]", address.index);
                }
                
                ImGui::EndTable();
            }
            ImGui::PopID();
            ImGui::TreePop();
        }
    }
}
