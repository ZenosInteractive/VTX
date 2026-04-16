#pragma once

#include <memory>
#include <string>

#include "services/schema_creator_service.h"
#include "session/vtx_session_base.h"

class SchemaCreatorSession : public VtxSessionBase {
public:
    SchemaCreatorSession();
    ~SchemaCreatorSession() override = default;

    void NewSchema();
    bool LoadSchemaFromPath(const std::string& path);
    bool SaveSchemaToPath(const std::string& path, bool save_as_next_generation);
    bool SaveSchema(bool save_as_next_generation);

    VtxServices::SchemaDocument& MutableDocument();
    const VtxServices::SchemaDocument& GetDocument() const { return document_; }

    const std::string& GetActivePath() const { return active_path_; }
    bool HasActivePath() const { return !active_path_.empty(); }

    bool HasUnsavedChanges() const { return has_unsaved_changes_; }
    bool HasBaseline() const { return has_baseline_; }
    const VtxServices::SchemaDocument& GetBaselineDocument() const { return baseline_document_; }

    void MarkDirty();
    void RefreshReportsIfNeeded();
    void ValidateNow();

    const VtxServices::ValidationReport& GetValidationReport() const { return validation_report_; }
    const VtxServices::EvolutionReport& GetEvolutionReport() const { return evolution_report_; }

    bool IsEditorWindowVisible() const { return show_editor_window_; }
    void SetEditorWindowVisible(bool visible) { show_editor_window_ = visible; }
    void ToggleEditorWindowVisible() { show_editor_window_ = !show_editor_window_; }

    bool IsGeneralInfoWindowVisible() const { return show_general_info_window_; }
    void SetGeneralInfoWindowVisible(bool visible) { show_general_info_window_ = visible; }
    void ToggleGeneralInfoWindowVisible() { show_general_info_window_ = !show_general_info_window_; }

    bool IsMappingsWindowVisible() const { return show_mappings_window_; }
    void SetMappingsWindowVisible(bool visible) { show_mappings_window_ = visible; }
    void ToggleMappingsWindowVisible() { show_mappings_window_ = !show_mappings_window_; }

    bool IsBucketsMappingWindowVisible() const { return show_buckets_mapping_window_; }
    void SetBucketsMappingWindowVisible(bool visible) { show_buckets_mapping_window_ = visible; }
    void ToggleBucketsMappingWindowVisible() { show_buckets_mapping_window_ = !show_buckets_mapping_window_; }

    bool IsValidationWindowVisible() const { return show_validation_window_; }
    void SetValidationWindowVisible(bool visible) { show_validation_window_ = visible; }
    void ToggleValidationWindowVisible() { show_validation_window_ = !show_validation_window_; }

    bool IsEvolutionWindowVisible() const { return show_evolution_window_; }
    void SetEvolutionWindowVisible(bool visible) { show_evolution_window_ = visible; }
    void ToggleEvolutionWindowVisible() { show_evolution_window_ = !show_evolution_window_; }

    bool IsNextGenerationMode() const { return next_generation_mode_; }
    void SetNextGenerationMode(bool enabled) { next_generation_mode_ = enabled; }

private:
    void CaptureBaselineFromCurrent();

    VtxServices::SchemaDocument document_;
    VtxServices::SchemaDocument baseline_document_;
    VtxServices::ValidationReport validation_report_;
    VtxServices::EvolutionReport evolution_report_;

    std::string active_path_;
    bool has_unsaved_changes_ = false;
    bool has_baseline_ = false;
    bool validation_dirty_ = true;
    bool evolution_dirty_ = true;
    bool show_editor_window_ = true;
    bool show_general_info_window_ = true;
    bool show_mappings_window_ = true;
    bool show_buckets_mapping_window_ = true;
    bool show_validation_window_ = true;
    bool show_evolution_window_ = true;
    bool next_generation_mode_ = false;
};
