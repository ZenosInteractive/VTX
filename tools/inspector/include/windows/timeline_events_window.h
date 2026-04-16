#pragma once
#include "gui/gui_window.h"

class InspectorSession;

class TimelineEventsWindow : public ImGuiWindow {
public:
    TimelineEventsWindow(std::shared_ptr<InspectorSession> session);

protected:
    void DrawContent() override;

private:
    std::shared_ptr<InspectorSession> inspector_session_;
};
