#include "inspector_layout.h"
#include "gui/portable-file-dialogs.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <vector>

#include "gui/gui_scale_controller.h"
#include "gui/gui_types.h"
#include "inspector_session.h"
#include "windows/analysis_window_factory.h"

namespace {

    struct DockWindowPlacement {
        std::string window_name;
        std::string region;
    };

    struct DockLayoutSpec {
        float left_ratio = 0.25f;
        float left_top_ratio = 0.75f;
        float bottom_ratio = 0.25f;
        float top_right_details_ratio = 0.50f;
        std::vector<DockWindowPlacement> placements;
    };

    struct CommandResult {
        bool success = true;
        bool warning = false;
        std::string log_message;
    };

    // Returns the canonical dock placement spec for the Inspector workspace.
    DockLayoutSpec BuildDefaultDockLayout() {
        DockLayoutSpec spec;
        spec.placements = {
            DockWindowPlacement {VtxGuiNames::FilePropertiesWindow, "left_top"},
            DockWindowPlacement {VtxGuiNames::ReaderProfilerWindow, "left_bottom"},
            DockWindowPlacement {VtxGuiNames::BucketsWindow, "tabs"},
            DockWindowPlacement {VtxGuiNames::ChunkIndexWindow, "tabs"},
            DockWindowPlacement {VtxGuiNames::TimelineEventsWindow, "tabs"},
            DockWindowPlacement {VtxGuiNames::ReplayTimeDataWindow, "tabs"},
            DockWindowPlacement {VtxGuiNames::SchemaViewerWindow, "details"},
            DockWindowPlacement {VtxGuiNames::EntityDetailsWindow, "details"},
            DockWindowPlacement {VtxGuiNames::TimelineWindow, "bottom"},
            DockWindowPlacement {VtxGuiNames::LogWindow, "bottom"},
        };
        return spec;
    }

    // Validates file picker output before replay-open flow starts.
    CommandResult ValidateOpenReplaySelection(const std::string& selected_path) {
        CommandResult result;
        if (selected_path.empty()) {
            result.success = false;
            result.warning = true;
            result.log_message = "No replay selected.";
        }
        return result;
    }

    // Builds success/failure feedback for replay-open commands.
    CommandResult BuildOpenReplayResult(const std::string& selected_path, bool success) {
        CommandResult result;
        result.success = success;
        result.warning = !success;
        result.log_message = success ? "Replay opened: " + selected_path : "Failed to open replay: " + selected_path;
        return result;
    }

    // Builds feedback for replay-close commands.
    CommandResult BuildCloseReplayResult(bool had_loaded_replay) {
        CommandResult result;
        if (!had_loaded_replay) {
            result.success = false;
            result.warning = true;
            result.log_message = "No replay is currently loaded.";
            return result;
        }

        result.log_message = "Replay closed.";
        return result;
    }

    // Routes command feedback to warning/info GUI log channel.
    void LogCommandResult(InspectorSession& session, const CommandResult& result) {
        if (result.warning) {
            session.AddGuiWarningLog(result.log_message);
            return;
        }
        session.AddGuiInfoLog(result.log_message);
    }

    // Renders shared DPI scaling controls for the desktop tools.
    void DrawViewMenu(const std::shared_ptr<GuiScaleController>& scale_controller) {
        if (!scale_controller || !ImGui::BeginMenu("View")) {
            return;
        }

        ImGui::TextDisabled("Auto Scale: %.0f%%", scale_controller->GetAutoScale() * 100.0f);
        ImGui::TextDisabled("Effective Scale: %.0f%%", scale_controller->GetEffectiveScale() * 100.0f);
        if (scale_controller->HasOverride()) {
            ImGui::TextDisabled("Adjustment: %.0f%%", scale_controller->GetScaleAdjustment() * 100.0f);
        } else {
            ImGui::TextDisabled("Adjustment: Auto");
        }

        ImGui::Separator();

        const float adjustment = scale_controller->GetScaleAdjustment();
        if (ImGui::MenuItem("Scale Down", nullptr, false, adjustment > 0.76f)) {
            scale_controller->DecreaseAdjustment();
        }
        if (ImGui::MenuItem("Scale Up")) {
            scale_controller->IncreaseAdjustment();
        }

        float adjustment_slider = adjustment;
        ImGui::PushItemWidth(160.0f);
        if (ImGui::SliderFloat("Scale Adjustment", &adjustment_slider, 0.75f, 2.0f, "%.2fx")) {
            scale_controller->SetScaleAdjustment(adjustment_slider);
        }
        ImGui::PopItemWidth();

        if (ImGui::MenuItem("Reset to Auto", nullptr, false, scale_controller->HasOverride())) {
            scale_controller->ResetToAuto();
        }

        ImGui::EndMenu();
    }

} // namespace

// Constructs the top-level Inspector layout shell.
InspectorLayout::InspectorLayout(const std::shared_ptr<InspectorSession>& session,
                                 const std::shared_ptr<GuiScaleController>& scale_controller)
    : session_(session)
    , scale_controller_(scale_controller) {}

// Inspector layout has no per-frame update state today.
void InspectorLayout::OnUpdate() {}

// Renders dockspace host, menu bar, and file/window actions.
void InspectorLayout::OnRender() {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    window_flags |=
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("VTX DockSpace Base", nullptr, window_flags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("VTXDockSpace");
    const auto layout_spec = BuildDefaultDockLayout();


    // Step 1: Build or rebuild dockspace layout.
    if (force_layout_reset_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        force_layout_reset_ = false;
        // Recreate canonical Inspector docking layout from the placement table.
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        ImGuiID dock_main_id = dockspace_id;

        ImGuiID dock_id_left =
            ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Left, layout_spec.left_ratio, nullptr, &dock_main_id);
        ImGuiID dock_id_left_top =
            ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Up, layout_spec.left_top_ratio, nullptr, &dock_id_left);
        ImGuiID dock_id_left_bottom = dock_id_left;

        ImGuiID dock_id_bottom =
            ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, layout_spec.bottom_ratio, nullptr, &dock_main_id);
        ImGuiID dock_id_details = ImGui::DockBuilderSplitNode(
            dock_main_id, ImGuiDir_Right, layout_spec.top_right_details_ratio, nullptr, &dock_main_id);
        ImGuiID dock_id_tabs = dock_main_id;

        for (const auto& placement : layout_spec.placements) {
            ImGuiID target = dock_main_id;
            if (placement.region == "left_top") {
                target = dock_id_left_top;
            } else if (placement.region == "left_bottom") {
                target = dock_id_left_bottom;
            } else if (placement.region == "tabs") {
                target = dock_id_tabs;
            } else if (placement.region == "details") {
                target = dock_id_details;
            } else if (placement.region == "bottom") {
                target = dock_id_bottom;
            }
            ImGui::DockBuilderDockWindow(placement.window_name.c_str(), target);
        }

        ImGui::DockBuilderFinish(dockspace_id);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // Step 2: Render top menu actions.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open VTX Replay...")) {
                auto f = pfd::open_file("Select a VTX Replay", ".", {"VTX Files (.vtx)", "*.vtx", "All Files", "*"});

                const std::string selected_path = f.result().empty() ? std::string {} : f.result()[0];
                const auto selection = ValidateOpenReplaySelection(selected_path);
                const auto result = selection.success
                                        ? BuildOpenReplayResult(selected_path, session_->LoadReplay(selected_path))
                                        : selection;
                LogCommandResult(*session_, result);
            }

            if (ImGui::BeginMenu("Open Recent")) {
                const auto recent_files = session_->GetRecentFilesSnapshot();
                if (recent_files.empty()) {
                    ImGui::MenuItem("No recent files", nullptr, false, false);
                } else {
                    for (const auto& recent_path : recent_files) {
                        if (ImGui::MenuItem(recent_path.c_str())) {
                            const auto result = BuildOpenReplayResult(recent_path, session_->LoadReplay(recent_path));
                            LogCommandResult(*session_, result);
                        }
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Close Replay", nullptr, false, session_->HasLoadedReplay())) {
                const bool had_loaded_replay = session_->HasLoadedReplay();
                if (had_loaded_replay) {
                    session_->CloseReplay();
                }
                const auto result = BuildCloseReplayResult(had_loaded_replay);
                LogCommandResult(*session_, result);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Exit")) {
                if (GLFWwindow* current_window = glfwGetCurrentContext()) {
                    glfwSetWindowShouldClose(current_window, GLFW_TRUE);
                }
            }
            ImGui::EndMenu();
        }

        DrawViewMenu(scale_controller_);

        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem(VtxGuiNames::FilePropertiesWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::BucketsWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::ReaderProfilerWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::SchemaViewerWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::ReplayTimeDataWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::ChunkIndexWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::TimelineEventsWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::EntityDetailsWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::TimelineWindow, nullptr, true);
            ImGui::MenuItem(VtxGuiNames::LogWindow, nullptr, true);

            ImGui::Separator();

            if (ImGui::MenuItem("Reset Layout to Default")) {
                force_layout_reset_ = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Analysis", session_->HasLoadedReplay())) {
            if (ImGui::MenuItem("Entity LifeTime")) {
                analysis_windows_.push_back(
                    AnalysisWindowFactory::CreateEntityLifeTimeWindow(session_, ++analysis_window_counter_));
            }
            if (ImGui::MenuItem("Unique Properties")) {
                analysis_windows_.push_back(
                    AnalysisWindowFactory::CreateUniquePropertiesWindow(session_, ++analysis_window_counter_));
            }
            if (ImGui::MenuItem("Track Property")) {
                analysis_windows_.push_back(
                    AnalysisWindowFactory::CreateTrackPropertyWindow(session_, ++analysis_window_counter_));
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    ImGui::End();

    // Render analysis windows (floating, outside dockspace).
    // OnRender() on each window internally skips if the window is closed.
    for (auto& window : analysis_windows_) {
        window->OnRender();
    }

    // Prune closed analysis windows
    std::erase_if(analysis_windows_, [](const std::shared_ptr<IGuiLayer>& layer) {
        return !AnalysisWindowFactory::IsAnalysisWindowOpen(layer);
    });
}
