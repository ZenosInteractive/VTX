#pragma once
#include <memory>
#include <string>
#include "gui/gui_manager.h"

class GuiScaleController;

class GuiApplication {
public:
    GuiApplication(std::string tool_id, const std::string& title, int width, int height);
    ~GuiApplication();
    void CreateMainLayout();
    void Run();
    void AddLayer(std::shared_ptr<IGuiLayer> layer);
    bool ShouldClose() const;
    const std::shared_ptr<GuiScaleController>& GetScaleController() const { return scale_controller_; }

private:
    bool InitWindow(const std::string& title, int width, int height);
    void Shutdown();

    GLFWwindow* window_ = nullptr;
    bool is_running_ = false;
    std::unique_ptr<GuiManager> gui_manager_;
    std::shared_ptr<GuiScaleController> scale_controller_;
};

using VtxToolApplication = GuiApplication;
