#pragma once

#include <string>
#include <memory>

#include "gui/gui_window.h"
#include "services/entity_inspector_view_service.h"

class InspectorSession;
namespace VTX {
    struct PropertyContainer;
}

class BucketsWindow : public ImGuiWindow {
public:
    BucketsWindow(std::shared_ptr<InspectorSession> session);
    
protected:
    void DrawContent() override;

private:
    void DrawEntityListPanel(const VtxServices::EntityInspectorViewModel& view_model);

    std::shared_ptr<InspectorSession> inspector_session_;
    char entity_filter_[256] = {};
};
