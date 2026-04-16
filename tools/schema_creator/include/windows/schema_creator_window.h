#pragma once

#include <memory>
#include <string>

#include "gui/gui_window.h"

class SchemaCreatorSession;

class SchemaCreatorWindow : public ImGuiWindow {
public:
    explicit SchemaCreatorWindow(std::shared_ptr<SchemaCreatorSession> session);
    ~SchemaCreatorWindow() override = default;

    void OnRender() override;

protected:
    void DrawContent() override;

private:
    void DrawToolbar();
    void DrawStructListPane();
    void DrawStructEditorPane();

    bool AddStruct(const std::string& struct_name);
    bool DuplicateSelectedStruct();
    bool RemoveSelectedStruct();
    bool AddFieldToSelectedStruct();
    void ClampSelectionToDocument();

    std::shared_ptr<SchemaCreatorSession> schema_session_;
    int selected_struct_index_ = -1;
    int selected_field_index_ = -1;
    std::string pending_new_struct_name_;
};
