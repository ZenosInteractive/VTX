#pragma once

#include <future>
#include <memory>
#include <string>

#include "gui/gui_layer.h"
#include "services/replay_cut_service.h"

class InspectorSession;

// Floating window (File > Cut Replay...) that writes a new .vtx containing a
// sub-range of the loaded replay. The range can be given as elapsed time,
// frames, or absolute UTC; it is snapped to whole chunks (chunks are copied
// verbatim, only the footer is rebuilt). The copy runs on a worker thread.
//
// Implements IGuiLayer directly (rather than the shared ImGuiWindow base) so this
// header never pulls in gui/gui_window.h -- whose `class ImGuiWindow` would clash
// with Dear ImGui's `struct ImGuiWindow` in translation units that include
// imgui_internal.h (e.g. inspector_layout.cpp).
class CutReplayWindow : public IGuiLayer {
public:
    explicit CutReplayWindow(std::shared_ptr<InspectorSession> session);
    ~CutReplayWindow() override = default;

    void OnUpdate() override {}
    void OnRender() override;

    void SetOpen(bool open) { is_open_ = open; }
    bool IsOpen() const { return is_open_; }

private:
    enum class Phase { Idle, Running, Done };

    struct Outcome {
        bool ok = false;
        std::string error;
        std::string dest_path;
    };

    void DrawContent();
    void ResetRangeToFullReplay();
    // Resolves the current inputs to an inclusive frame range; false when the
    // inputs cannot be parsed (error_out explains why).
    bool ResolveRequestedFrames(int& start_frame, int& end_frame, std::string& error_out) const;
    void StartCut(const VtxServices::ReplayCutPlan& plan, const std::string& dest_path);
    static Outcome RunCut(std::string source_path, VTX::FileFooter footer, VtxServices::ReplayCutPlan plan,
                          std::string dest_path);

    std::shared_ptr<InspectorSession> session_;

    int range_mode_ = 0; // 0 = time, 1 = frame, 2 = UTC
    double time_start_seconds_ = 0.0;
    double time_end_seconds_ = 0.0;
    int frame_start_ = 0;
    int frame_end_ = 0;
    char utc_start_[64] = {};
    char utc_end_[64] = {};
    bool range_initialized_ = false;

    Phase phase_ = Phase::Idle;
    std::future<Outcome> future_;
    Outcome outcome_;
    int spinner_tick_ = 0;
    bool is_open_ = true;
};
