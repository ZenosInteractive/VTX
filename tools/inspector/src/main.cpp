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
#include "win_file_association.h"

#include <exception>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>

#include <cwchar>
#endif

namespace {

#if defined(_WIN32)
    // The replay the shell hands over -- "Open with...", a .vtx file association
    // (double-click) or a plain command line -- is the first argument after the program
    // name. Converted to UTF-8 so it matches the paths the file dialogs and GLFW drops
    // produce.
    std::string StartupReplayPath() {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) {
            return {};
        }

        std::string path;
        // First argument that is not a --flag (so "--register" et al. never look like a
        // replay path). The shell passes the dropped/opened file here via the registered
        // "%1" association -- see win_file_association.h.
        for (int i = 1; i < argc; ++i) {
            if (!argv[i] || argv[i][0] == L'\0' || wcsncmp(argv[i], L"--", 2) == 0) {
                continue;
            }
            const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (size > 1) {
                path.resize(static_cast<size_t>(size - 1));
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, path.data(), size, nullptr, nullptr);
            }
            break;
        }
        LocalFree(argv);
        return path;
    }

    // Handles --register / --unregister (set up or remove the .vtx file association)
    // before any UI. Returns true if a flag was handled and the app should exit; sets
    // exit_ok to whether it succeeded. Feedback goes through a message box because the
    // GUI subsystem has no console.
    bool HandleAssociationCli(bool& exit_ok) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) {
            return false;
        }

        bool handled = false;
        exit_ok = true;
        for (int i = 1; i < argc && !handled; ++i) {
            const bool do_register = wcscmp(argv[i], L"--register") == 0;
            const bool do_unregister = wcscmp(argv[i], L"--unregister") == 0;
            if (!do_register && !do_unregister) {
                continue;
            }
            const VtxInspector::AssociationResult result =
                do_register ? VtxInspector::RegisterVtxAssociation() : VtxInspector::UnregisterVtxAssociation();
            MessageBoxA(nullptr, result.message.c_str(), "VTX Inspector",
                        MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR));
            exit_ok = result.ok;
            handled = true;
        }
        LocalFree(argv);
        return handled;
    }
#else
    std::string StartupReplayPath(int argc, char** argv) {
        return (argc >= 2 && argv[1]) ? std::string(argv[1]) : std::string {};
    }
#endif

    // Boots Inspector app, registers all windows/layers, and runs UI loop.
    int RunInspector(const std::string& startup_path) {
        GuiApplication app("vtx_inspector", "VTX Inspector v0.1.0", 1600, 900);
        auto session = std::make_shared<InspectorSession>();

        // Self-associate .vtx on first launch of this build so double-click and
        // "Open with" work without the user running --register. Silent and idempotent;
        // respects an --unregister opt-out and never steals a default set to another app.
        if (const auto assoc = VtxInspector::EnsureVtxAssociationOnStartup(); assoc.changed) {
            session->AddGuiInfoLog(assoc.message);
        }

        auto layout = std::make_shared<InspectorLayout>(session, app.GetScaleController());
        app.AddLayer(layout);
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

        // Files dragged onto the window open like File > Open (first one wins).
        app.SetDropHandler([layout](const std::vector<std::string>& paths) { layout->RequestOpenReplay(paths); });

        // A replay handed over by the shell opens on the first frame.
        if (!startup_path.empty()) {
            layout->RequestOpenReplay({startup_path});
        }

        app.Run();
        return 0;
    }

} // namespace

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // --register / --unregister set up the file association, then exit.
    bool exit_ok = true;
    if (HandleAssociationCli(exit_ok)) {
        return exit_ok ? 0 : 1;
    }
    try {
        return RunInspector(StartupReplayPath());
    } catch (const std::exception&) {
        return -1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return RunInspector(StartupReplayPath(argc, argv));
    } catch (const std::exception&) {
        return -1;
    }
}
#endif
