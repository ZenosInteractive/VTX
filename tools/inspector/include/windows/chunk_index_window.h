#pragma once
#include "gui/gui_window.h"

class InspectorSession;

class ChunkIndexWindow : public ImGuiWindow {
public:
    ChunkIndexWindow(std::shared_ptr<InspectorSession> session);

protected:
    void DrawContent() override;

private:
    std::shared_ptr<InspectorSession> inspector_session_;
};
