#pragma once

#include <future>
#include <string>

#include "gui/gui_layer.h"
#include "vtx/writer/core/vtx_replay_recovery.h"

// Floating window that repairs a crashed .vtx from its ".recovery" sidecar via
// VTX::RepairReplayFile. The user selects the .vtx (and, if it lives elsewhere, the
// .vtx.recovery sidecar); the repair runs on a worker thread so the UI stays
// responsive, and the window shows live progress plus a success/failure result.
//
// Implements IGuiLayer directly (rather than the shared ImGuiWindow base) so this
// header never pulls in gui/gui_window.h -- whose `class ImGuiWindow` would clash with
// Dear ImGui's `struct ImGuiWindow` in translation units that include imgui_internal.h
// (e.g. inspector_layout.cpp).
class RepairReplayWindow : public IGuiLayer {
public:
    RepairReplayWindow() = default;
    ~RepairReplayWindow() override = default;

    void OnUpdate() override {}
    void OnRender() override;

    void SetOpen(bool open) { is_open_ = open; }
    bool IsOpen() const { return is_open_; }

private:
    enum class Phase { Idle, Running, Done };

    struct Outcome {
        VTX::RepairResult result;
        std::string note; // extra context (e.g. sidecar staging)
    };

    void DrawContent();
    void StartRepair();
    void PickVtx();
    void PickRecovery();
    static Outcome RunRepair(std::string vtx_path, std::string recovery_path);

    std::string vtx_path_;
    std::string recovery_path_;

    Phase phase_ = Phase::Idle;
    std::future<Outcome> future_;
    Outcome outcome_;
    int spinner_tick_ = 0;
    bool is_open_ = true;
};
