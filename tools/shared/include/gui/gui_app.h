#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "gui/gui_manager.h"

class GuiScaleController;

class GuiApplication {
public:
    // Receives the UTF-8 paths of files dragged onto the main window. Invoked on the
    // UI thread from glfwPollEvents(), i.e. between frames.
    using DropHandler = std::function<void(const std::vector<std::string>& paths)>;

    GuiApplication(std::string tool_id, const std::string& title, int width, int height);
    ~GuiApplication();
    void CreateMainLayout();
    void Run();
    void AddLayer(std::shared_ptr<IGuiLayer> layer);
    void SetDropHandler(DropHandler handler);
    bool ShouldClose() const;
    const std::shared_ptr<GuiScaleController>& GetScaleController() const { return scale_controller_; }

private:
    bool InitWindow(const std::string& title, int width, int height);
    void Shutdown();

    // GLFW callbacks carry no user data and a window has a single user pointer: the
    // application owns it and dispatches window events from these trampolines.
    static void OnGlfwContentScale(GLFWwindow* window, float x_scale, float y_scale);
    static void OnGlfwDrop(GLFWwindow* window, int count, const char** paths);

    GLFWwindow* window_ = nullptr;
    bool is_running_ = false;
    std::unique_ptr<GuiManager> gui_manager_;
    std::shared_ptr<GuiScaleController> scale_controller_;
    DropHandler drop_handler_;
};

using VtxToolApplication = GuiApplication;
