#pragma once

#include <memory>
#include <string>

#include "gui/gui_layer.h"

class GuiScaleController;
class SchemaCreatorSession;

class SchemaCreatorLayout : public IGuiLayer {
public:
    SchemaCreatorLayout(const std::shared_ptr<SchemaCreatorSession>& session, const std::shared_ptr<GuiScaleController>& scale_controller);
    ~SchemaCreatorLayout() override = default;

    void OnUpdate() override;
    void OnRender() override;

private:
    void SetStatus(const std::string& message, bool is_error = false);

    std::shared_ptr<SchemaCreatorSession> session_;
    std::shared_ptr<GuiScaleController> scale_controller_;
    bool force_layout_reset_ = true;
    std::string status_message_;
    float status_ttl_seconds_ = 0.0f;
    bool status_is_error_ = false;
};
