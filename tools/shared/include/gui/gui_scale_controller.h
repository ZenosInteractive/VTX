#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <imgui.h>

struct GLFWwindow;

// Owns DPI-aware ImGui scaling and persists per-tool override state.
class GuiScaleController {
public:
    explicit GuiScaleController(std::string tool_id);
    ~GuiScaleController() = default;

    GuiScaleController(const GuiScaleController&) = delete;
    GuiScaleController& operator=(const GuiScaleController&) = delete;

    void BindWindow(GLFWwindow* window);
    void InitializeImGui();
    void Update();

    bool IncreaseAdjustment(float step = 0.10f);
    bool DecreaseAdjustment(float step = 0.10f);
    bool SetScaleAdjustment(float adjustment);
    bool ResetToAuto();

    const std::string& GetToolId() const { return tool_id_; }
    float GetAutoScale() const { return auto_scale_; }
    float GetEffectiveScale() const { return effective_scale_; }
    bool HasOverride() const { return scale_adjustment_.has_value(); }
    float GetScaleAdjustment() const;

private:
    void CaptureBaseStyle();
    void RefreshAutoScale();
    void OnWindowContentScaleChanged(float x_scale, float y_scale);
    void ApplyScale();
    void LoadSettings();
    void SaveSettings() const;

    static float ClampEffectiveScale(float scale);
    static float ClampAdjustment(float adjustment);

    std::string tool_id_;
    std::filesystem::path settings_path_;
    GLFWwindow* window_ = nullptr;
    ImGuiStyle base_style_{};
    bool has_base_style_ = false;
    float auto_scale_ = 1.0f;
    float effective_scale_ = 1.0f;
    std::optional<float> scale_adjustment_;
};
