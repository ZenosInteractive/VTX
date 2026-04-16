#pragma once

#include <memory>

#include "gui/gui_window.h"

class SchemaCreatorSession;

class SchemaEvolutionWindow : public ImGuiWindow {
public:
    explicit SchemaEvolutionWindow(std::shared_ptr<SchemaCreatorSession> session);
    ~SchemaEvolutionWindow() override = default;

    void OnRender() override;

protected:
    void DrawContent() override;

private:
    std::shared_ptr<SchemaCreatorSession> schema_session_;
};
