#pragma once
#include <memory>
#include <vector>

#include "gui/gui_layer.h"

struct GLFWwindow;
class GuiScaleController;
class GuiManager {
public:
    GuiManager(GLFWwindow* window, const std::shared_ptr<GuiScaleController>& scale_controller);
    ~GuiManager();

    GuiManager(const GuiManager&) = delete;
    GuiManager& operator=(const GuiManager&) = delete;

    void AddLayer(std::shared_ptr<IGuiLayer> layer);

    void BeginFrame();
    void Update();
    void Render();
    void EndFrame();

private:
    GLFWwindow* window_;
    std::shared_ptr<GuiScaleController> scale_controller_;
    std::vector<std::shared_ptr<IGuiLayer>> gui_layers_;
};
