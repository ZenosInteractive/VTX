#include "gui/gui_window.h"
#include <imgui.h>

// Constructs a reusable ImGui window shell bound to a shared session.
ImGuiWindow::ImGuiWindow(const std::string& title, const std::shared_ptr<VtxSessionBase>& session,
                         ImGuiWindowFlags flags)
    : title_(title)
    , session_(session)
    , flags_(flags) {}

// Updates window-specific state only while the window is visible.
void ImGuiWindow::OnUpdate() {
    if (is_open_) {
        UpdateLogic();
    }
}

// Renders the window body and guarantees Begin/End pairing.
void ImGuiWindow::OnRender() {
    if (!is_open_) {
        return;
    }

    // Keep Begin/End paired even when content is conditionally skipped.
    if (ImGui::Begin(title_.c_str(), &is_open_, flags_)) {
        DrawContent();
    }
    ImGui::End();
}

// Flips open/closed visibility state.
void ImGuiWindow::Toggle() {
    is_open_ = !is_open_;
}

// Sets visibility explicitly.
void ImGuiWindow::SetOpen(bool open) {
    is_open_ = open;
}

// Returns current visibility state.
bool ImGuiWindow::IsOpen() const {
    return is_open_;
}
