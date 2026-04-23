#include "schema_creator_layout.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <vector>

#include "gui/gui_scale_controller.h"
#include "gui/gui_types.h"
#include "gui/portable-file-dialogs.h"
#include "schema_creator_session.h"
#include "services/schema_creator_service.h"

namespace {

    struct DockWindowPlacement {
        std::string window_name;
        std::string region;
    };

    constexpr float kStatusTtlSeconds = 3.0f;

    // Builds save dialog suggestion path for current generation mode.
    std::string BuildSaveSuggestionPath(const SchemaCreatorSession& session, bool apply_next_generation_rules) {
        if (apply_next_generation_rules) {
            return VtxServices::SchemaCreatorService::BuildSuggestedNextGenerationPath(session.GetActivePath());
        }
        if (session.HasActivePath()) {
            return session.GetActivePath();
        }
        return "schema.json";
    }

    // Executes save/save-as flow based on generation mode and active path state.
    bool SaveSchemaFromMenu(SchemaCreatorSession& session, bool force_save_as) {
        const bool next_generation_mode = session.IsNextGenerationMode();
        const bool apply_next_generation_rules = next_generation_mode && session.HasBaseline();

        if (!force_save_as && !next_generation_mode && session.HasActivePath()) {
            // Fast path: plain save can reuse current file without opening dialog.
            return session.SaveSchema(false);
        }

        const std::string dialog_title =
            force_save_as ? (next_generation_mode ? "Save next generation schema as..." : "Save schema as...")
                          : (next_generation_mode ? "Save next generation schema" : "Save schema");

        auto save_dialog = pfd::save_file(dialog_title, BuildSaveSuggestionPath(session, apply_next_generation_rules),
                                          {"JSON Files (.json)", "*.json", "All Files", "*"});

        if (save_dialog.result().empty()) {
            return false;
        }
        return session.SaveSchemaToPath(save_dialog.result(), apply_next_generation_rules);
    }

    // Builds canonical dock layout for Schema Creator workspace.
    void BuildDockLayout(ImGuiID dockspace_id, const ImVec2& workspace_size) {
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, workspace_size);

        ImGuiID dock_main_id = dockspace_id;
        ImGuiID dock_bottom_id =
            ImGui::DockBuilderSplitNode(dock_main_id, ImGuiDir_Down, 0.20f, nullptr, &dock_main_id);
        ImGuiID dock_top_left_id = dock_main_id;
        ImGuiID dock_top_right_id =
            ImGui::DockBuilderSplitNode(dock_top_left_id, ImGuiDir_Right, 0.30f, nullptr, &dock_top_left_id);

        const std::vector<DockWindowPlacement> placements = {
            DockWindowPlacement {"Property Mappings", "top_left"},
            DockWindowPlacement {"General Info", "top_left"},
            DockWindowPlacement {"Buckets & Bone Mapping", "top_left"},
            DockWindowPlacement {"Schema Validation", "top_right"},
            DockWindowPlacement {"Schema Evolution", "top_right"},
            DockWindowPlacement {VtxGuiNames::LogWindow, "bottom"},
        };

        for (const auto& placement : placements) {
            ImGuiID target = dock_top_left_id;
            if (placement.region == "bottom") {
                target = dock_bottom_id;
            } else if (placement.region == "top_right") {
                target = dock_top_right_id;
            }
            ImGui::DockBuilderDockWindow(placement.window_name.c_str(), target);
        }

        ImGui::DockBuilderFinish(dockspace_id);
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

// Constructs top-level Schema Creator layout shell.
SchemaCreatorLayout::SchemaCreatorLayout(const std::shared_ptr<SchemaCreatorSession>& session,
                                         const std::shared_ptr<GuiScaleController>& scale_controller)
    : session_(session)
    , scale_controller_(scale_controller) {}

// Updates transient status banner TTL and clears expired messages.
void SchemaCreatorLayout::OnUpdate() {
    if (status_ttl_seconds_ <= 0.0f) {
        return;
    }
    status_ttl_seconds_ -= ImGui::GetIO().DeltaTime;
    if (status_ttl_seconds_ <= 0.0f) {
        status_ttl_seconds_ = 0.0f;
        status_message_.clear();
        status_is_error_ = false;
    }
}

// Renders dockspace host, menu commands, and status banner.
void SchemaCreatorLayout::OnRender() {
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

    ImGui::Begin("Schema Creator DockSpace Base", nullptr, window_flags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("SchemaCreatorDockSpace");
    // Step 1: Build or rebuild dock layout.
    if (force_layout_reset_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        force_layout_reset_ = false;
        BuildDockLayout(dockspace_id, viewport->WorkSize);
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // Step 2: Render file/window menus.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Schema")) {
                session_->NewSchema();
                SetStatus("New schema document created.");
            }

            if (ImGui::MenuItem("Open Schema...")) {
                auto chooser =
                    pfd::open_file("Open schema JSON", ".", {"JSON Files (.json)", "*.json", "All Files", "*"});
                if (!chooser.result().empty()) {
                    const bool loaded = session_->LoadSchemaFromPath(chooser.result()[0]);
                    SetStatus(loaded ? "Schema loaded." : "Failed to load schema.", !loaded);
                }
            }

            if (ImGui::BeginMenu("Open Recent")) {
                const auto recent_files = session_->GetRecentFilesSnapshot();
                if (recent_files.empty()) {
                    ImGui::MenuItem("No recent files", nullptr, false, false);
                } else {
                    for (const auto& recent_path : recent_files) {
                        if (ImGui::MenuItem(recent_path.c_str())) {
                            const bool loaded = session_->LoadSchemaFromPath(recent_path);
                            SetStatus(loaded ? "Schema loaded: " + recent_path
                                             : "Failed to load schema: " + recent_path,
                                      !loaded);
                        }
                    }
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Validate")) {
                session_->ValidateNow();
                SetStatus(session_->GetValidationReport().is_valid ? "Validation completed (no blocking errors)."
                                                                   : "Validation completed (errors found).",
                          !session_->GetValidationReport().is_valid);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Save")) {
                const bool saved = SaveSchemaFromMenu(*session_, false);
                SetStatus(saved ? "Schema saved." : "Schema was not saved.", !saved);
            }

            if (ImGui::MenuItem("Save As...")) {
                const bool saved = SaveSchemaFromMenu(*session_, true);
                SetStatus(saved ? "Schema saved." : "Schema was not saved.", !saved);
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                SetStatus("Use the window close button to exit.");
            }
            ImGui::EndMenu();
        }

        DrawViewMenu(scale_controller_);

        if (ImGui::BeginMenu("Windows")) {
            bool show_mappings = session_->IsMappingsWindowVisible();
            if (ImGui::MenuItem("Property Mappings", nullptr, show_mappings)) {
                session_->ToggleMappingsWindowVisible();
            }

            bool show_general_info = session_->IsGeneralInfoWindowVisible();
            if (ImGui::MenuItem("General Info", nullptr, show_general_info)) {
                session_->ToggleGeneralInfoWindowVisible();
            }

            bool show_buckets_mapping = session_->IsBucketsMappingWindowVisible();
            if (ImGui::MenuItem("Buckets & Bone Mapping", nullptr, show_buckets_mapping)) {
                session_->ToggleBucketsMappingWindowVisible();
            }

            bool show_validation = session_->IsValidationWindowVisible();
            if (ImGui::MenuItem("Schema Validation", nullptr, show_validation)) {
                session_->ToggleValidationWindowVisible();
            }

            bool show_evolution = session_->IsEvolutionWindowVisible();
            if (ImGui::MenuItem("Schema Evolution", nullptr, show_evolution)) {
                session_->ToggleEvolutionWindowVisible();
            }

            ImGui::MenuItem(VtxGuiNames::LogWindow, nullptr, true, false);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                force_layout_reset_ = true;
                SetStatus("Layout reset queued.");
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    // Step 3: Render transient status line.
    if (!status_message_.empty()) {
        const ImVec4 color = status_is_error_ ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(0.55f, 0.95f, 0.65f, 1.0f);
        ImGui::TextColored(color, "%s", status_message_.c_str());
    }

    ImGui::End();
}

// Sets transient status text used by layout-level feedback strip.
void SchemaCreatorLayout::SetStatus(const std::string& message, bool is_error) {
    status_message_ = message;
    status_is_error_ = is_error;
    status_ttl_seconds_ = kStatusTtlSeconds;
}
