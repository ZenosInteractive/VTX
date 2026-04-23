#include "gui/gui_app.h"
#include "inspector_layout.h"
#include "windows/buckets_window.h"
#include "windows/chunk_index_window.h"
#include "windows/entity_details_window.h"
#include "windows/file_info_window.h"
#include "windows/reader_inspector_window.h"
#include "windows/replay_time_data_window.h"
#include "windows/schema_viewer_window.h"
#include "windows/timeline_events_window.h"
#include "windows/timeline_window.h"
#include "logging/logs_window.h"
#include "inspector_session.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

    // Boots Inspector app, registers all windows/layers, and runs UI loop.
    int RunInspector() {
        GuiApplication app("vtx_inspector", "VTX Inspector v1.0", 1600, 900);
        auto session = std::make_shared<InspectorSession>();

        app.AddLayer(std::make_shared<InspectorLayout>(session, app.GetScaleController()));
        app.AddLayer(std::make_shared<TimelineWindow>(session));
        app.AddLayer(std::make_shared<BucketsWindow>(session));
        app.AddLayer(std::make_shared<ReaderInspectorWindow>(session));
        app.AddLayer(std::make_shared<EntityDetailsWindow>(session));
        app.AddLayer(std::make_shared<SchemaViewerWindow>(session));
        app.AddLayer(std::make_shared<ReplayTimeDataWindow>(session));
        app.AddLayer(std::make_shared<ChunkIndexWindow>(session));
        app.AddLayer(std::make_shared<TimelineEventsWindow>(session));
        app.AddLayer(std::make_shared<LogsWindow>(session));
        app.AddLayer(std::make_shared<FileInfoWindow>(session));

        app.Run();
        return 0;
    }

} // namespace

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
#else
int main()
#endif
{
    try {
        // Delegate startup flow so main stays minimal and readable.
        return RunInspector();
    } catch (const std::exception&) {
        return -1;
    }
}
