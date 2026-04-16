#include "windows/entity_details_window.h"

#include <algorithm>
#include <imgui.h>
#include <utility>

#include "gui/gui_types.h"
#include "inspector_session.h"
#include "vtx/common/vtx_optimized_bone.h"

EntityDetailsWindow::EntityDetailsWindow(std::shared_ptr<InspectorSession> session)
    : ImGuiWindow(VtxGuiNames::EntityDetailsWindow, session)
    , inspector_session_(std::move(session)) {
}

void EntityDetailsWindow::DrawContent() {
    auto* reader = inspector_session_->GetReader();
    if (!inspector_session_->HasLoadedReplay() || !reader) {
        ImGui::TextDisabled("Load a replay to inspect entity details.");
        return;
    }

    auto& state = inspector_session_->MutableEntityInspectorState();
    const auto screen = VtxServices::EntityInspectorViewService::BuildScreen(
        reader,
        inspector_session_->HasLoadedReplay(),
        inspector_session_->IsScrubbingTimeline(),
        inspector_session_->GetCurrentFrame(),
        state);
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

    VTX::PropertyAddressCache schema_cache;
    const VTX::PropertyAddressCache* global_cache = nullptr;
    if (reader) {
        schema_cache = reader->GetPropertyAddressCache();
        global_cache = &schema_cache;
    }

    if (screen.view_model.has_frame) {
        ImGui::BeginChild("EntityDetailsPanel", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        DrawPropertiesPanel(screen.view_model, global_cache);
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("%s", screen.view_model.empty_properties_message.c_str());
    }

    if (screen.view_model.is_loading) {
        ImGui::EndDisabled();
    }
}

void EntityDetailsWindow::DrawPropertiesPanel(
    const VtxServices::EntityInspectorViewModel& view_model,
    const VTX::PropertyAddressCache* global_cache) {
    // Render the selected entity details with shared schema/type name display preferences.
    const auto& state = inspector_session_->GetEntityInspectorState();
    bool& show_schema_names = inspector_session_->MutableShowSchemaNames();
    if (!view_model.empty_properties_message.empty() && view_model.properties.roots.empty()) {
        const bool is_missing_entity = !state.selected_entity_id.empty() &&
            view_model.empty_properties_message.find("no longer exists") != std::string::npos;
        if (is_missing_entity) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", view_model.empty_properties_message.c_str());
        } else {
            ImGui::TextDisabled("%s", view_model.empty_properties_message.c_str());
        }
        return;
    }

    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", view_model.header.entity_label.c_str());
    if (show_schema_names && !view_model.header.type_name.empty()) {
        const std::string type_name_label = "Type: " + view_model.header.type_name;
        if (ImGui::Selectable(type_name_label.c_str())) {
            inspector_session_->RequestSchemaHighlight(view_model.header.type_name, "");
            ImGui::SetWindowFocus(VtxGuiNames::SchemaViewerWindow);
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Schema Name: %s", view_model.header.type_name.c_str());
            ImGui::Separator();
            ImGui::TextDisabled("Left-Click to highlight in Schema Viewer");
            ImGui::EndTooltip();
        }
    } else {
        ImGui::TextDisabled("%s", view_model.header.type_label.c_str());
    }
    if (view_model.header.show_hash) {
        ImGui::TextDisabled("%s", view_model.header.hash_label.c_str());
    }
    ImGui::Checkbox("Show Schema Names", &show_schema_names);
    ImGui::Separator();
    ImGui::Spacing();

    for (const auto& node : view_model.properties.roots) {
        RenderPropertyNode(node, global_cache, show_schema_names);
    }

    if (show_schema_names && !view_model.header.missing_field_names.empty()) {
        const float section_start_x = ImGui::GetCursorStartPos().x;
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetCursorPosX(section_start_x);
        ImGui::TextColored(
            ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
            "Missing Fields (%zu):",
            view_model.header.missing_field_names.size());
        for (const auto& missing_field_name : view_model.header.missing_field_names) {
            ImGui::SetCursorPosX(section_start_x);
            ImGui::BulletText("%s", missing_field_name.c_str());
        }
    }
    
    ImGui::Separator();
    ImGui::Spacing();

}

void EntityDetailsWindow::RenderPropertyNode(
    const VtxServices::EntityPropertyNode& node,
    const VTX::PropertyAddressCache* global_cache,
    bool show_schema_names) {
    
    if (node.is_property) {
        std::string display_label = node.label;
        if (show_schema_names) {
            if (!node.schema_label.empty()) {
                display_label = node.schema_label;
            } else {
                const std::string schema_name =
                    VtxServices::EntityInspectorViewService::ResolveSchemaName(global_cache, node);
                if (!schema_name.empty()) {
                    display_label = schema_name;
                }
            }
        }

        const std::string display_text = display_label + ": " + node.value;
        if (ImGui::Selectable(display_text.c_str())) {
            const auto effect = VtxServices::EntityInspectorViewService::BuildPropertyActivateEffect(node, global_cache);
            if (effect.request_schema_focus) {
                inspector_session_->RequestSchemaHighlight(effect.schema_struct_name, effect.schema_property_name);
                if (effect.focus_schema_window) {
                    ImGui::SetWindowFocus(VtxGuiNames::SchemaViewerWindow);
                }
            }
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ImGui::SetClipboardText(node.raw_value.c_str());
        }

        if (ImGui::IsItemHovered()) {
            const std::string schema_name =
                VtxServices::EntityInspectorViewService::ResolveSchemaName(global_cache, node);
            ImGui::BeginTooltip();
            if (!schema_name.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Schema Name: %s", schema_name.c_str());
                ImGui::Separator();
            }
            ImGui::TextDisabled("Left-Click to highlight in Schema Viewer");
            ImGui::TextDisabled("Right-Click to copy raw value");
            if (node.raw_value != node.value) {
                ImGui::Text("Formatted: %s", node.value.c_str());
            }
            ImGui::Text("Raw: %s", node.raw_value.c_str());
            ImGui::EndTooltip();
        }
        return;
    }

    std::string tree_label = node.label;
    if (show_schema_names && !node.schema_label.empty()) {
        tree_label = node.schema_label;
    }

    ImGuiTreeNodeFlags flags = node.children.empty() ? ImGuiTreeNodeFlags_Leaf : 0;
    if (node.open_by_default) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    const bool open = ImGui::TreeNodeEx(node.id.c_str(), flags, "%s", tree_label.c_str());

    if (ImGui::IsItemHovered()) {
        const auto effect = VtxServices::EntityInspectorViewService::BuildPropertyContextEffect(node, global_cache);
        if (effect.request_schema_focus) {
            ImGui::BeginTooltip();
            if (!effect.schema_property_name.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Schema Name: %s", effect.schema_property_name.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Struct: %s", effect.schema_struct_name.c_str());
            }
            ImGui::Separator();
            ImGui::TextDisabled("Right-Click to find in Schema Viewer");
            ImGui::EndTooltip();
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        const auto effect = VtxServices::EntityInspectorViewService::BuildPropertyContextEffect(node, global_cache);
        if (effect.request_schema_focus) {
            inspector_session_->RequestSchemaHighlight(effect.schema_struct_name, effect.schema_property_name);
            ImGui::SetWindowFocus(VtxGuiNames::SchemaViewerWindow);
        }
    }

    if (!node.children.empty() && open) {
        const float nested_indent = ImGui::GetTreeNodeToLabelSpacing() * 0.5f;

        //Draw uint8 array as an hex viewer
        if (node.schema_type == VTX::FieldType::Int8 && node.schema_container_type == VTX::FieldContainerType::Array) {
            ImGui::Indent(nested_indent);
            DrawHexViewerForNode(node);
            ImGui::Unindent(nested_indent);
        } 
        else {
           //Draw array elements as before, by child
            const bool should_extra_indent = std::all_of(
                node.children.begin(),
                node.children.end(),
                [](const VtxServices::EntityPropertyNode& child) { return child.is_property; });
            
            if (should_extra_indent) ImGui::Indent(nested_indent);
            for (const auto& child : node.children) {
                RenderPropertyNode(child, global_cache, show_schema_names);
            }
            if (should_extra_indent) ImGui::Unindent(nested_indent);
        }
    }

    if (open) {
        ImGui::TreePop();
    }
}


void EntityDetailsWindow::DrawHexViewerForNode(const VtxServices::EntityPropertyNode& node) {
    // 1. Rebuild the bytes
    std::vector<uint8_t> raw_bytes;
    raw_bytes.reserve(node.children.size());
    for (const auto& child : node.children) {
        try {
            raw_bytes.push_back(static_cast<uint8_t>(std::stoul(child.raw_value)));
        } catch (...) {
            raw_bytes.push_back(0);
        }
    }

    if (raw_bytes.empty()) return;

    // 2. Extended state system: Save [Data Type] and [Display Base] per node
    static std::unordered_map<std::string, std::pair<int, int>> format_selections;
    auto& selection = format_selections[node.id];
    int& data_type = selection.first;
    int& display_base = selection.second;

    const char* type_names[] = { "8-bit Integer", "16-bit Integer", "32-bit Integer", "64-bit Integer", "32-bit Float", "64-bit Double" };
    const char* base_names[] = { "Hexadecimal", "Unsigned Decimal", "Signed Decimal" };

    // 3. User Interface (Two combos on the same line)
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo(("Type##" + node.id).c_str(), &data_type, type_names, IM_ARRAYSIZE(type_names));
    
    // Only show base selector if NOT Float or Double
    bool is_float = (data_type == 4 || data_type == 5);
    if (!is_float) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo(("Base##" + node.id).c_str(), &display_base, base_names, IM_ARRAYSIZE(base_names));
    }
    ImGui::Spacing();

    // 4. Size and visual width logic to keep columns aligned
    size_t bytes_per_item = 1;
    int item_width = 3; // Character width of each printed block (to align ASCII)

    if (data_type == 0) { bytes_per_item = 1; item_width = (display_base == 0) ? 3 : (display_base == 1) ? 4 : 5; }
    else if (data_type == 1) { bytes_per_item = 2; item_width = (display_base == 0) ? 5 : (display_base == 1) ? 6 : 7; }
    else if (data_type == 2) { bytes_per_item = 4; item_width = (display_base == 0) ? 9 : (display_base == 1) ? 11 : 12; }
    else if (data_type == 3) { bytes_per_item = 8; item_width = (display_base == 0) ? 17 : 21; } // Signed/Unsigned 64-bit occupy max ~20 + space
    else if (data_type == 4) { bytes_per_item = 4; item_width = 11; }
    else if (data_type == 5) { bytes_per_item = 8; item_width = 15; }

    // 5. Display Panel
    ImGui::BeginChild((node.id + "_HexDump").c_str(), ImVec2(0, 200), true, ImGuiWindowFlags_HorizontalScrollbar);
    
    const size_t bytes_per_line = 16; 
    
    for (size_t i = 0; i < raw_bytes.size(); i += bytes_per_line) {
        char line_buffer[512]; // Increased buffer size for printing large 64-bit decimals
        char* ptr = line_buffer;
        
        // Offset Column
        ptr += snprintf(ptr, sizeof(line_buffer) - (ptr - line_buffer), "%04zX: ", i);
        
        // Formatted Data Column
        for (size_t j = 0; j < bytes_per_line; j += bytes_per_item) {
            
            if (i + j + bytes_per_item <= raw_bytes.size()) {
                const uint8_t* data_ptr = &raw_bytes[i + j];

                switch (data_type) {
                    case 0: { // 8-bit
                        uint8_t v = *data_ptr;
                        if (display_base == 0) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%02X ", v);
                        else if (display_base == 1) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%3u ", v);
                        else ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%4d ", (int8_t)v);
                        break;
                    }
                    case 1: { // 16-bit
                        uint16_t v; std::memcpy(&v, data_ptr, sizeof(v));
                        if (display_base == 0) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%04X ", v);
                        else if (display_base == 1) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%5u ", v);
                        else ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%6d ", (int16_t)v);
                        break;
                    }
                    case 2: { // 32-bit
                        uint32_t v; std::memcpy(&v, data_ptr, sizeof(v));
                        if (display_base == 0) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%08X ", v);
                        else if (display_base == 1) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%10u ", v);
                        else ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%11d ", (int32_t)v);
                        break;
                    }
                    case 3: { // 64-bit
                        uint64_t v; std::memcpy(&v, data_ptr, sizeof(v));
                        if (display_base == 0) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%016llX ", (unsigned long long)v);
                        else if (display_base == 1) ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%20llu ", (unsigned long long)v);
                        else ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%20lld ", (long long)v);
                        break;
                    }
                    case 4: { // 32-bit Float
                        float v; std::memcpy(&v, data_ptr, sizeof(v));
                        ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%10.4f ", v);
                        break;
                    }
                    case 5: { // 64-bit Double
                        double v; std::memcpy(&v, data_ptr, sizeof(v));
                        ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%14.6f ", v);
                        break;
                    }
                }
            } else {
                // Dynamic Padding: Print exact spaces if the line is cut at the end of the buffer
                ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%*s", item_width, ""); 
            }
        }
        
        ptr += snprintf(ptr, 512 - (ptr - line_buffer), "  |  ");
        
        // ASCII Column (raw text)
        for (size_t j = 0; j < bytes_per_line; ++j) {
            if (i + j < raw_bytes.size()) {
                unsigned char c = raw_bytes[i + j];
                ptr += snprintf(ptr, 512 - (ptr - line_buffer), "%c", (c >= 32 && c < 128) ? c : '.');
            }
        }
        
        ImGui::TextUnformatted(line_buffer);
    }
    
    ImGui::EndChild();
}
