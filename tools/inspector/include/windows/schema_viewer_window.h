#pragma once
#include "gui/gui_window.h"
#include "vtx/common/readers/schema_reader/game_schema_types.h"

class InspectorSession;

class SchemaViewerWindow : public ImGuiWindow {
public:
    SchemaViewerWindow(std::shared_ptr<InspectorSession> session);

protected:
    void DrawContent() override;

private:
    void DrawTypeIcon(VTX::FieldType type_id, VTX::FieldContainerType container_type, float size = 0.0f) const;
    std::shared_ptr<InspectorSession> inspector_session_;
};
