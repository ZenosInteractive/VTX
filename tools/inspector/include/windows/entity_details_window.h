#pragma once

#include "gui/gui_window.h"
#include "services/entity_inspector_view_service.h"

class InspectorSession;

class EntityDetailsWindow : public ImGuiWindow {
public:
    explicit EntityDetailsWindow(std::shared_ptr<InspectorSession> session);

protected:
    void DrawContent() override;

private:
    void DrawPropertiesPanel(const VtxServices::EntityInspectorViewModel& view_model,
                             const VTX::PropertyAddressCache* global_cache);
    void RenderPropertyNode(const VtxServices::EntityPropertyNode& node, const VTX::PropertyAddressCache* global_cache,
                            bool show_schema_names);

    void DrawHexViewerForNode(const VtxServices::EntityPropertyNode& node);
    std::shared_ptr<InspectorSession> inspector_session_;
};
