#pragma once

#include <memory>
#include <string>

#include "gui/gui_window.h"

class SchemaCreatorSession;

class SchemaBucketsMappingWindow : public ImGuiWindow {
public:
    explicit SchemaBucketsMappingWindow(std::shared_ptr<SchemaCreatorSession> session);
    ~SchemaBucketsMappingWindow() override = default;

    void OnRender() override;

protected:
    void DrawContent() override;

private:
    void ClampSelectionIndices();
    bool AddBucket(const std::string& bucket_name);
    bool RemoveSelectedBucket();
    bool AddBoneModel(const std::string& model_name);
    bool RemoveSelectedBoneModel();
    bool AddBoneToSelectedModel(const std::string& bone_name);
    bool RemoveSelectedBone();

    std::shared_ptr<SchemaCreatorSession> schema_session_;
    int selected_bucket_index_ = -1;
    int selected_model_index_ = -1;
    int selected_bone_index_ = -1;
    std::string pending_bucket_name_;
    std::string pending_model_name_;
    std::string pending_bone_name_;
};

