#pragma once
#include "gui/gui_window.h"

class InspectorSession;

class FileInfoWindow : public ImGuiWindow {
public:
    FileInfoWindow(std::shared_ptr<InspectorSession> session);

protected:
    void DrawContent() override;

private:
    std::shared_ptr<InspectorSession> inspector_session_;
};
