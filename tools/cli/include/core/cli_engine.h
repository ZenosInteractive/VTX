#pragma once
#include "core/cli_concepts.h"
#include "core/cli_session.h"
#include "commands/command_registry.h"

#include "commands/analysis_commands.h"
#include "commands/file_commands.h"
#include "commands/info_commands.h"
#include "commands/inspect_commands.h"
#include "commands/navigation_commands.h"
#include "commands/session_commands.h"

namespace VtxCli {

inline constexpr const char* kCliVersion = "0.1.0";

template<FormatWriter Fmt, Transport IO>
class CliEngine {
public:
    template<typename... Args>
    explicit CliEngine(Args&&... transport_args)
        : transport_(std::forward<Args>(transport_args)...)
    {
        RegisterCommands();
    }

    void Run()
    {
        CommandContext ctx{session_};
        ctx.help_entries = &registry_.GetHelpEntries();

        bool verbose = false;
        if constexpr (requires { transport_.IsVerbose(); }) {
            verbose = transport_.IsVerbose();
        }

        if (verbose) {
            transport_.WriteLog("------------------------------------------");
            transport_.WriteLog("  VTX CLI v" + std::string(kCliVersion) + " - Headless Inspector");
            transport_.WriteLog("  Type 'help' for available commands");
            transport_.WriteLog("------------------------------------------");
        }

        {
            Fmt w;
            w.BeginObject()
             .Key("status").WriteString("ready")
             .Key("tool").WriteString("vtx_cli")
             .Key("version").WriteString(kCliVersion)
             .Key("commands");
            w.BeginArray();
            for (const auto& entry : registry_.GetHelpEntries()) {
                w.WriteString(entry.name);
            }
            w.EndArray()
             .EndObject();
            transport_.WriteLine(w.Finalize(""));
        }

        while (!ctx.exit_requested && transport_.IsOpen()) {
            auto line = transport_.ReadLine();
            if (!line) break;
            if (line->empty()) continue;

            auto response = registry_.Dispatch(ctx, *line);
            if (!response.empty()) {
                transport_.WriteLine(response);
            }
        }
    }

private:
    void RegisterCommands()
    {
        // File
        registry_.template Register<OpenCommand>();
        registry_.template Register<CloseCommand>();

        // Session
        registry_.template Register<HelpCommand>();
        registry_.template Register<ExitCommand>();
        registry_.template Register<QuitCommand>();

        // Info
        registry_.template Register<InfoCommand>();
        registry_.template Register<HeaderCommand>();
        registry_.template Register<FooterCommand>();
        registry_.template Register<SchemaCommand>();
        registry_.template Register<ChunksCommand>();
        registry_.template Register<EventsCommand>();

        // Navigation
        registry_.template Register<FrameCommand>();

        // Inspect
        registry_.template Register<BucketsCommand>();
        registry_.template Register<EntitiesCommand>();
        registry_.template Register<EntityCommand>();
        registry_.template Register<PropertyCommand>();
        registry_.template Register<TypesCommand>();

        // Analysis
        registry_.template Register<DiffCommand>();
        registry_.template Register<TrackCommand>();
        registry_.template Register<SearchCommand>();
    }

    CliSession  session_;
    CommandRegistry<Fmt> registry_;
    IO transport_;

public:
    /// Print the list of interactive commands to an output stream.
    /// Can be called without a running engine (static-like via temp instance).
    static void PrintCommandHelp(std::ostream& out)
    {
        CommandRegistry<Fmt> reg;
        // Register all commands into a temporary registry
        reg.template Register<OpenCommand>();
        reg.template Register<CloseCommand>();
        reg.template Register<HelpCommand>();
        reg.template Register<ExitCommand>();
        reg.template Register<QuitCommand>();
        reg.template Register<InfoCommand>();
        reg.template Register<HeaderCommand>();
        reg.template Register<FooterCommand>();
        reg.template Register<SchemaCommand>();
        reg.template Register<ChunksCommand>();
        reg.template Register<EventsCommand>();
        reg.template Register<FrameCommand>();
        reg.template Register<BucketsCommand>();
        reg.template Register<EntitiesCommand>();
        reg.template Register<EntityCommand>();
        reg.template Register<PropertyCommand>();
        reg.template Register<TypesCommand>();
        reg.template Register<DiffCommand>();
        reg.template Register<TrackCommand>();
        reg.template Register<SearchCommand>();

        for (const auto& entry : reg.GetHelpEntries()) {
            out << "  " << entry.description << "\n";
        }
    }
};

} // namespace VtxCli
