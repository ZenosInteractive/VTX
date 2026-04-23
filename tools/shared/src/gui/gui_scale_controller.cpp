#include "gui/gui_scale_controller.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace {
    constexpr float kMinEffectiveScale = 0.75f;
    constexpr float kMaxEffectiveScale = 4.00f;
    constexpr float kMinScaleAdjustment = 0.75f;
    constexpr float kMaxScaleAdjustment = 2.00f;
    constexpr float kScaleEpsilon = 0.01f;
    constexpr char kSettingsFileName[] = "gui_settings.ini";
    constexpr char kScaleKey[] = "scale_adjustment";
    constexpr char kAutoValue[] = "auto";

    // Returns a stable LOCALAPPDATA-backed settings path for shared GUI preferences.
    std::filesystem::path BuildSettingsPath() {
#if defined(_WIN32)
        char* local_app_data = nullptr;
        size_t size = 0;
        if (_dupenv_s(&local_app_data, &size, "LOCALAPPDATA") == 0 && local_app_data != nullptr) {
            std::filesystem::path path = std::filesystem::path(local_app_data) / "VTX" / kSettingsFileName;
            free(local_app_data);
            return path;
        }
#endif
        return std::filesystem::current_path() / kSettingsFileName;
    }

    // Formats float values consistently for INI persistence.
    std::string FormatScaleValue(float value) {
        std::ostringstream stream;
        stream.setf(std::ios::fixed, std::ios::floatfield);
        stream.precision(2);
        stream << value;
        return stream.str();
    }

    // Chooses a single scale factor from GLFW's platform-provided content scale pair.
    float ResolveContentScale(float x_scale, float y_scale) {
        const float resolved = std::max(x_scale, y_scale);
        if (resolved <= 0.0f) {
            return 1.0f;
        }
        return resolved;
    }

} // namespace

// Seeds scale state from persisted per-tool preferences.
GuiScaleController::GuiScaleController(std::string tool_id)
    : tool_id_(std::move(tool_id))
    , settings_path_(BuildSettingsPath()) {
    LoadSettings();
}

// Binds the active GLFW window and subscribes to live content-scale changes.
void GuiScaleController::BindWindow(GLFWwindow* window) {
    window_ = window;
    if (!window_) {
        return;
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetWindowContentScaleCallback(window_, [](GLFWwindow* glfw_window, float x_scale, float y_scale) {
        auto* controller = static_cast<GuiScaleController*>(glfwGetWindowUserPointer(glfw_window));
        if (!controller) {
            return;
        }
        controller->OnWindowContentScaleChanged(x_scale, y_scale);
    });

    RefreshAutoScale();
}

// Captures the base ImGui style once and applies the current effective scale.
void GuiScaleController::InitializeImGui() {
    CaptureBaseStyle();
    RefreshAutoScale();
    ApplyScale();
}

// Polls content scale to catch monitor changes even when GLFW callbacks are delayed.
void GuiScaleController::Update() {
    RefreshAutoScale();
}

// Raises manual scale adjustment relative to the monitor-derived auto scale.
bool GuiScaleController::IncreaseAdjustment(float step) {
    return SetScaleAdjustment(GetScaleAdjustment() + step);
}

// Lowers manual scale adjustment relative to the monitor-derived auto scale.
bool GuiScaleController::DecreaseAdjustment(float step) {
    return SetScaleAdjustment(GetScaleAdjustment() - step);
}

// Applies a persisted manual adjustment multiplier on top of auto scaling.
bool GuiScaleController::SetScaleAdjustment(float adjustment) {
    const float clamped_adjustment = ClampAdjustment(adjustment);
    if (std::fabs(clamped_adjustment - 1.0f) <= kScaleEpsilon) {
        return ResetToAuto();
    }
    if (scale_adjustment_.has_value() && std::fabs(scale_adjustment_.value() - clamped_adjustment) <= kScaleEpsilon) {
        return false;
    }

    scale_adjustment_ = clamped_adjustment;
    SaveSettings();
    ApplyScale();
    return true;
}

// Removes manual adjustment so scaling tracks monitor DPI only.
bool GuiScaleController::ResetToAuto() {
    if (!scale_adjustment_.has_value()) {
        return false;
    }

    scale_adjustment_.reset();
    SaveSettings();
    ApplyScale();
    return true;
}

// Reports the user-selected adjustment multiplier or neutral auto behavior.
float GuiScaleController::GetScaleAdjustment() const {
    return scale_adjustment_.value_or(1.0f);
}

// Captures the pristine ImGui style so future rescaling always starts from the same baseline.
void GuiScaleController::CaptureBaseStyle() {
    if (has_base_style_) {
        return;
    }

    base_style_ = ImGui::GetStyle();
    has_base_style_ = true;
}

// Refreshes auto scale from the bound GLFW window's content scale.
void GuiScaleController::RefreshAutoScale() {
    if (!window_) {
        return;
    }

    float x_scale = 1.0f;
    float y_scale = 1.0f;
    glfwGetWindowContentScale(window_, &x_scale, &y_scale);
    OnWindowContentScaleChanged(x_scale, y_scale);
}

// Applies a new effective scale when the underlying window DPI changes.
void GuiScaleController::OnWindowContentScaleChanged(float x_scale, float y_scale) {
    const float resolved_scale = ClampEffectiveScale(ResolveContentScale(x_scale, y_scale));
    if (std::fabs(resolved_scale - auto_scale_) <= kScaleEpsilon) {
        return;
    }

    auto_scale_ = resolved_scale;
    ApplyScale();
}

// Rebuilds style sizing from the base style and updates global font scaling.
void GuiScaleController::ApplyScale() {
    if (!has_base_style_) {
        return;
    }

    const float next_scale = ClampEffectiveScale(auto_scale_ * GetScaleAdjustment());
    if (std::fabs(next_scale - effective_scale_) <= kScaleEpsilon) {
        return;
    }

    ImGuiStyle scaled_style = base_style_;
    scaled_style.ScaleAllSizes(next_scale);
    ImGui::GetStyle() = scaled_style;

    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = next_scale;
    effective_scale_ = next_scale;
}

// Loads a persisted tool-specific scale adjustment from the shared GUI INI file.
void GuiScaleController::LoadSettings() {
#if defined(_WIN32)
    if (!std::filesystem::exists(settings_path_)) {
        return;
    }

    char buffer[64] = {};
    GetPrivateProfileStringA(tool_id_.c_str(), kScaleKey, kAutoValue, buffer, static_cast<DWORD>(std::size(buffer)),
                             settings_path_.string().c_str());
    const std::string stored_value = buffer;
    if (stored_value.empty() || stored_value == kAutoValue) {
        return;
    }

    try {
        scale_adjustment_ = ClampAdjustment(std::stof(stored_value));
    } catch (...) {
        scale_adjustment_.reset();
    }
#endif
}

// Persists tool-specific scale adjustment so manual overrides survive app restarts.
void GuiScaleController::SaveSettings() const {
#if defined(_WIN32)
    std::error_code error;
    std::filesystem::create_directories(settings_path_.parent_path(), error);

    const std::string value =
        scale_adjustment_.has_value() ? FormatScaleValue(scale_adjustment_.value()) : std::string(kAutoValue);
    WritePrivateProfileStringA(tool_id_.c_str(), kScaleKey, value.c_str(), settings_path_.string().c_str());
#endif
}

// Clamps the final effective ImGui scale to a range that keeps layouts usable.
float GuiScaleController::ClampEffectiveScale(float scale) {
    return std::clamp(scale, kMinEffectiveScale, kMaxEffectiveScale);
}

// Clamps the user-selected adjustment multiplier independently from monitor DPI.
float GuiScaleController::ClampAdjustment(float adjustment) {
    return std::clamp(adjustment, kMinScaleAdjustment, kMaxScaleAdjustment);
}
