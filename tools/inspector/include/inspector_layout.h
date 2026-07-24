#pragma once
#include <memory>
#include <vector>
#include "gui/gui_layer.h"

class GuiScaleController;
class InspectorSession;
class RepairReplayWindow;

class InspectorLayout : public IGuiLayer {
public:
    InspectorLayout(const std::shared_ptr<InspectorSession>& session,
                    const std::shared_ptr<GuiScaleController>& scale_controller);
    ~InspectorLayout() override = default;
    void OnUpdate() override;
    void OnRender() override;

protected:
    std::shared_ptr<InspectorSession> session_;
    std::shared_ptr<GuiScaleController> scale_controller_;
    bool force_layout_reset_ = true;

    // Analysis windows: floating, not part of the default dock layout.
    // Stored as IGuiLayer to avoid name collision with imgui_internal's ImGuiWindow.
    std::vector<std::shared_ptr<IGuiLayer>> analysis_windows_;
    int analysis_window_counter_ = 0;

    // File > Repair Replay: floating, independent of a loaded replay (it repairs a
    // possibly-unopenable .vtx from its ".recovery" sidecar). At most one at a time.
    std::shared_ptr<RepairReplayWindow> repair_window_;
};
