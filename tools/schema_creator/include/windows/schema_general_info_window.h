#pragma once

#include <memory>
#include <string>

#include "gui/gui_window.h"

class SchemaCreatorSession;

class SchemaGeneralInfoWindow : public ImGuiWindow {
public:
    explicit SchemaGeneralInfoWindow(std::shared_ptr<SchemaCreatorSession> session);
    ~SchemaGeneralInfoWindow() override = default;

    void OnRender() override;

protected:
    void DrawContent() override;

private:
    std::shared_ptr<SchemaCreatorSession> schema_session_;
};

