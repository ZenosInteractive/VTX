#pragma once

#include "gui/gui_window.h"

class LogsWindow : public ImGuiWindow {
public:
    explicit LogsWindow(std::shared_ptr<VtxSessionBase> session);

protected:
    void DrawContent() override;

private:
    bool auto_scroll_ = true;
};
