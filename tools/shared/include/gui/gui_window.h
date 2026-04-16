#pragma once
#include <memory>
#include <string>
#include <imgui.h>
#include "gui/gui_layer.h"
#include "session/vtx_session_base.h"

// Base class for other ImGui sub-windows (Timeline, DiffViwer,HexViewer, etc.)
class ImGuiWindow : public IGuiLayer {
protected:
    std::string title_;
    bool is_open_ = true;
    ImGuiWindowFlags flags_ = 0;

public:
    ImGuiWindow(const std::string& title, const std::shared_ptr<VtxSessionBase>& session, ImGuiWindowFlags flags = 0) ;
    virtual ~ImGuiWindow() = default;

    void OnUpdate() final;
    void OnRender();

    void Toggle();
    void SetOpen(bool open);
    bool IsOpen() const;

protected:
    //Child classes only need to override these two methods
    virtual void UpdateLogic() {} 
    virtual void DrawContent() = 0;
    
    //pointer to global session
    std::shared_ptr<VtxSessionBase> session_;
};
