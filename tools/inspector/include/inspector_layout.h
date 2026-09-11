#pragma once
#include <memory>
#include <string>
#include <vector>
#include "gui/gui_layer.h"

class CutReplayWindow;
class FilterEntitiesWindow;
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

    // Opens a replay from outside the File menu: the path the shell hands over at
    // startup ("Open with...", a .vtx file association) or files dragged onto the
    // window. Queued and consumed on the next OnUpdate() so the open runs inside the
    // UI frame through the same validation, logging and recent-files path as
    // File > Open. Only the first path is opened; any others are reported. When a
    // replay is already loaded, a modal asks whether to close it and open the new
    // file or keep the current one.
    void RequestOpenReplay(std::vector<std::string> paths);

protected:
    // Shared open path for the File menu, Open Recent, startup argument and drops.
    void OpenReplay(const std::string& path);

    // Modal asking whether a dropped file replaces the loaded replay.
    void DrawReplaceReplayPopup();

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

    // File > Cut Replay: floating, operates on the loaded replay. At most one.
    std::shared_ptr<CutReplayWindow> cut_window_;

    // File > Filter Entities: floating, operates on the loaded replay. At most one.
    std::shared_ptr<FilterEntitiesWindow> filter_window_;

    // Paths queued by RequestOpenReplay(), consumed by the next OnUpdate().
    std::vector<std::string> pending_open_paths_;

    // Dropped file waiting for the user's decision while a replay is loaded; empty when
    // nothing is pending. replace_popup_open_ records that the modal was shown, so a
    // dismissal without a choice (Escape) counts as "keep current".
    std::string replace_candidate_;
    bool replace_popup_open_ = false;
};
