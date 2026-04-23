#pragma once

#include <memory>
#include "gui/gui_layer.h"

class InspectorSession;

// Factory functions to create analysis windows without exposing their headers
// (avoids ImGuiWindow name collision with imgui_internal.h in inspector_layout.cpp).

namespace AnalysisWindowFactory {

    std::shared_ptr<IGuiLayer> CreateEntityLifeTimeWindow(std::shared_ptr<InspectorSession> session, int instance_id);

    std::shared_ptr<IGuiLayer> CreateUniquePropertiesWindow(std::shared_ptr<InspectorSession> session, int instance_id);

    std::shared_ptr<IGuiLayer> CreateTrackPropertyWindow(std::shared_ptr<InspectorSession> session, int instance_id);

    // Returns true if the analysis window layer is still open.
    bool IsAnalysisWindowOpen(const std::shared_ptr<IGuiLayer>& layer);

} // namespace AnalysisWindowFactory
