#include "schema_creator_session.h"

SchemaCreatorSession::SchemaCreatorSession() {
    NewSchema();
    AddGuiInfoLog("Schema Creator session initialized.");
}

void SchemaCreatorSession::NewSchema() {
    document_ = VtxServices::SchemaCreatorService::CreateEmptyDocument();
    baseline_document_ = VtxServices::SchemaCreatorService::CreateEmptyDocument();
    validation_report_ = {};
    evolution_report_ = {};
    active_path_.clear();
    current_file_path_.clear();
    is_file_loaded_ = true;
    has_unsaved_changes_ = false;
    has_baseline_ = false;
    validation_dirty_ = true;
    evolution_dirty_ = true;
    RefreshReportsIfNeeded();
    AddGuiInfoLog("Started a new schema document.");
}

bool SchemaCreatorSession::LoadSchemaFromPath(const std::string& path) {
    VtxServices::SchemaDocument loaded_document;
    std::string error;
    if (!VtxServices::SchemaCreatorService::LoadFromFile(path, loaded_document, error)) {
        AddGuiErrorLog("Failed to load schema: " + error);
        return false;
    }

    document_ = std::move(loaded_document);
    active_path_ = path;
    current_file_path_ = path;
    is_file_loaded_ = true;
    RecordRecentFile(path);
    has_unsaved_changes_ = false;
    CaptureBaselineFromCurrent();
    validation_dirty_ = true;
    evolution_dirty_ = true;
    RefreshReportsIfNeeded();
    AddGuiInfoLog("Schema loaded: " + path);
    return true;
}

bool SchemaCreatorSession::SaveSchemaToPath(const std::string& path, bool save_as_next_generation) {
    VtxServices::SchemaDocument document_to_save = document_;

    if (save_as_next_generation && has_baseline_) {
        evolution_report_ = VtxServices::SchemaCreatorService::BuildEvolutionReport(baseline_document_, document_);
        evolution_dirty_ = false;

        if (evolution_report_.has_breaking_changes) {
            AddGuiErrorLog("Schema evolution check failed: breaking changes detected.");
            return false;
        }

        VtxServices::SchemaCreatorService::ApplyNextGenerationVersion(document_to_save, baseline_document_);
    }

    std::string error;
    if (!VtxServices::SchemaCreatorService::SaveToFile(document_to_save, path, error)) {
        AddGuiErrorLog("Failed to save schema: " + error);
        return false;
    }

    document_ = std::move(document_to_save);
    active_path_ = path;
    current_file_path_ = path;
    has_unsaved_changes_ = false;
    is_file_loaded_ = true;
    CaptureBaselineFromCurrent();
    validation_dirty_ = true;
    evolution_dirty_ = true;
    RefreshReportsIfNeeded();
    AddGuiInfoLog("Schema saved: " + path);
    return true;
}

bool SchemaCreatorSession::SaveSchema(bool save_as_next_generation) {
    if (active_path_.empty()) {
        AddGuiWarningLog("Cannot save without a target path.");
        return false;
    }
    return SaveSchemaToPath(active_path_, save_as_next_generation);
}

VtxServices::SchemaDocument& SchemaCreatorSession::MutableDocument() {
    return document_;
}

void SchemaCreatorSession::MarkDirty() {
    has_unsaved_changes_ = true;
    validation_dirty_ = true;
    evolution_dirty_ = true;
}

void SchemaCreatorSession::RefreshReportsIfNeeded() {
    if (validation_dirty_) {
        validation_report_ = VtxServices::SchemaCreatorService::ValidateSchema(document_);
        validation_dirty_ = false;
    }

    if (evolution_dirty_) {
        if (has_baseline_) {
            evolution_report_ = VtxServices::SchemaCreatorService::BuildEvolutionReport(baseline_document_, document_);
        } else {
            evolution_report_ = {};
        }
        evolution_dirty_ = false;
    }
}

void SchemaCreatorSession::ValidateNow() {
    validation_dirty_ = true;
    evolution_dirty_ = true;
    RefreshReportsIfNeeded();
    if (validation_report_.is_valid) {
        AddGuiInfoLog("Schema validation passed.");
    } else {
        AddGuiWarningLog("Schema validation reported errors.");
    }
}

void SchemaCreatorSession::CaptureBaselineFromCurrent() {
    baseline_document_ = document_;
    has_baseline_ = true;
}
