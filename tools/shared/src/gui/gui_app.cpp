#include "GLFW/glfw3.h"

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3native.h"
#endif

#include <GL/gl.h>
#include <stdexcept>

#include "gui/gui_app.h"
#include "gui/gui_scale_controller.h"

namespace {
    constexpr float kClearColor = 0.1f;

#if defined(_WIN32)
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);

    // Opts the process into per-monitor DPI awareness before GLFW creates any windows.
    void ApplyProcessDpiAwareness() {
        const HMODULE user32_module = GetModuleHandleW(L"user32.dll");
        if (user32_module != nullptr) {
            const auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(user32_module, "SetProcessDpiAwarenessContext"));
            if (set_context != nullptr && set_context(reinterpret_cast<HANDLE>(-4))) {
                return;
            }
        }

        const HMODULE shcore_module = LoadLibraryW(L"shcore.dll");
        if (shcore_module != nullptr) {
            const auto set_awareness =
                reinterpret_cast<SetProcessDpiAwarenessFn>(GetProcAddress(shcore_module, "SetProcessDpiAwareness"));
            if (set_awareness != nullptr && SUCCEEDED(set_awareness(2))) {
                FreeLibrary(shcore_module);
                return;
            }
            FreeLibrary(shcore_module);
        }

        SetProcessDPIAware();
    }

    void ApplyWindowIcon(GLFWwindow* window) {
        if (!window) {
            return;
        }

        const HMODULE module_handle = GetModuleHandleW(nullptr);
        if (!module_handle) {
            return;
        }

        const auto* resource_name = L"IDI_ICON1";
        HICON large_icon =
            static_cast<HICON>(LoadImageW(module_handle, resource_name, IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
        HICON small_icon =
            static_cast<HICON>(LoadImageW(module_handle, resource_name, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
        if (!large_icon && !small_icon) {
            return;
        }

        HWND hwnd = glfwGetWin32Window(window);
        if (!hwnd) {
            return;
        }

        if (large_icon) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
        }
        if (small_icon) {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
        }
    }
#endif

} // namespace

// Creates app window and initializes shared GUI manager.
GuiApplication::GuiApplication(std::string tool_id, const std::string& title, int width, int height)
    : scale_controller_(std::make_shared<GuiScaleController>(std::move(tool_id))) {
    if (!InitWindow(title, width, height)) {
        throw std::runtime_error("Error initializing GUI window");
    }
    gui_manager_ = std::make_unique<GuiManager>(window_, scale_controller_);
    is_running_ = true;
}

// Ensures GUI resources are released on app shutdown.
GuiApplication::~GuiApplication() {
    Shutdown();
}

// Legacy hook kept for compatibility with older monolithic startup flow.
void GuiApplication::CreateMainLayout() {
    // No-op in shared build. Each tool sets up its own layout via AddLayer().
}

// Initializes GLFW window and applies platform-specific app icon.
bool GuiApplication::InitWindow(const std::string& title, int width, int height) {
#if defined(_WIN32)
    ApplyProcessDpiAwareness();
#endif

    if (!glfwInit()) {
        return false;
    }

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        return false;
    }

#if defined(_WIN32)
    ApplyWindowIcon(window_);
#endif

    if (scale_controller_) {
        scale_controller_->BindWindow(window_);
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    return true;
}

// Runs main UI loop until external close request is received.
void GuiApplication::Run() {
    while (is_running_ && !ShouldClose()) {
        // Step 1: Process OS events and clear framebuffer.
        glfwPollEvents();
        glClearColor(kClearColor, kClearColor, kClearColor, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Step 2: Run full GUI frame lifecycle.
        gui_manager_->BeginFrame();
        gui_manager_->Update();
        gui_manager_->Render();
        gui_manager_->EndFrame();

        // Step 3: Present rendered frame.
        glfwSwapBuffers(window_);
    }
}

// Adds a GUI layer to the frame update/render pipeline.
void GuiApplication::AddLayer(std::shared_ptr<IGuiLayer> layer) {
    gui_manager_->AddLayer(layer);
}

// Reports whether app window has been closed or destroyed.
bool GuiApplication::ShouldClose() const {
    return window_ == nullptr || glfwWindowShouldClose(window_) != 0;
}

// Shuts down manager, destroys window, and terminates GLFW runtime.
void GuiApplication::Shutdown() {
    is_running_ = false;
    gui_manager_.reset();
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}
