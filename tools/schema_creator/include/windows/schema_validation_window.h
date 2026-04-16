#pragma once

#include <memory>

#include "gui/gui_window.h"

class SchemaCreatorSession;

class SchemaValidationWindow : public ImGuiWindow {
public:
    explicit SchemaValidationWindow(std::shared_ptr<SchemaCreatorSession> session);
    ~SchemaValidationWindow() override = default;

    void OnRender() override;

protected:
    void DrawContent() override;

private:
    std::shared_ptr<SchemaCreatorSession> schema_session_;
};
