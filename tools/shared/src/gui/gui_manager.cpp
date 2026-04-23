#include "gui/gui_manager.h"
#include <GLFW/glfw3.h>

#include "gui/gui_scale_controller.h"

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// Initializes ImGui runtime and backend bindings for the active GLFW window.
GuiManager::GuiManager(GLFWwindow* window, const std::shared_ptr<GuiScaleController>& scale_controller)
    : window_(window)
    , scale_controller_(scale_controller) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Enable the same global capabilities for every tool host.
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    if (scale_controller_) {
        scale_controller_->InitializeImGui();
    }

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 130");
}

// Shuts down ImGui backends and destroys the global ImGui context.
GuiManager::~GuiManager() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// Registers a render/update layer into the frame lifecycle.
void GuiManager::AddLayer(std::shared_ptr<IGuiLayer> layer) {
    gui_layers_.push_back(layer);
}

// Starts a new ImGui frame and resets backend frame state.
void GuiManager::BeginFrame() {
    if (scale_controller_) {
        scale_controller_->Update();
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

// Runs per-frame update logic for each registered layer.
void GuiManager::Update() {
    for (auto& layer : gui_layers_) {
        layer->OnUpdate();
    }
}

// Runs per-frame render logic for each registered layer.
void GuiManager::Render() {
    for (auto& layer : gui_layers_) {
        layer->OnRender();
    }
}

// Finalizes frame rendering, including optional multi-viewport windows.
void GuiManager::EndFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // Multi-viewport rendering may switch contexts internally; restore ours.
        GLFWwindow* backup_current_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        glfwMakeContextCurrent(backup_current_context);
    }
}
