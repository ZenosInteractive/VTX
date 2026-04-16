#include "gui/gui_app.h"
#include "logging/logs_window.h"
#include "schema_creator_layout.h"
#include "schema_creator_session.h"
#include "windows/schema_buckets_mapping_window.h"
#include "windows/schema_evolution_window.h"
#include "windows/schema_creator_window.h"
#include "windows/schema_general_info_window.h"
#include "windows/schema_validation_window.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// Boots Schema Creator app, registers editor/validation/evolution layers, and runs UI loop.
int RunSchemaCreator() {
    GuiApplication app("vtx_schema_creator", "Schema Creator v1.0", 1600, 950);
    auto session = std::make_shared<SchemaCreatorSession>();
    session->SetMappingsWindowVisible(true);
    session->SetGeneralInfoWindowVisible(true);
    session->SetBucketsMappingWindowVisible(true);

    app.AddLayer(std::make_shared<SchemaCreatorLayout>(session, app.GetScaleController()));
    app.AddLayer(std::make_shared<SchemaCreatorWindow>(session));
    app.AddLayer(std::make_shared<SchemaGeneralInfoWindow>(session));
    app.AddLayer(std::make_shared<SchemaBucketsMappingWindow>(session));
    app.AddLayer(std::make_shared<SchemaValidationWindow>(session));
    app.AddLayer(std::make_shared<SchemaEvolutionWindow>(session));
    app.AddLayer(std::make_shared<LogsWindow>(session));
    app.Run();
    return 0;
}

} // namespace

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#else
int main() {
#endif
    try {
        // Delegate startup flow so main stays minimal and readable.
        return RunSchemaCreator();
    } catch (const std::exception&) {
        return -1;
    }
}
