#pragma once
#include "gui/gui_types.h"
#include "gui/gui_window.h"

class InspectorSession;

class ReplayTimeDataWindow : public ImGuiWindow {
public:    
    ReplayTimeDataWindow(std::shared_ptr<InspectorSession> session);

protected:
    void DrawContent() override;

private:
    std::shared_ptr<InspectorSession> inspector_session_;
    int highlighted_game_timestamp_row_ = -1;
    bool pending_scroll_to_highlighted_game_row_ = false;
    bool pending_switch_to_game_timestamp_tab_ = false;
};
