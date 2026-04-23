#pragma once
#include "commands/command_registry.h"
#include "commands/command_helpers.h"
#include <span>
#include <string>

namespace VtxCli {

    // exit / quit
    template <FormatWriter Fmt>
    struct ExitCommand {
        static constexpr std::string_view Name = "exit";
        static constexpr std::string_view Help = "exit - End the session";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            context.exit_requested = true;
            ResponseOk(writer, Name);
            EndResponse(writer);
        }
    };

    template <FormatWriter Fmt>
    struct QuitCommand {
        static constexpr std::string_view Name = "quit";
        static constexpr std::string_view Help = "quit - End the session (alias for exit)";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            context.exit_requested = true;
            ResponseOk(writer, Name);
            EndResponse(writer);
        }
    };

    // help
    template <FormatWriter Fmt>
    struct HelpCommand {
        static constexpr std::string_view Name = "help";
        static constexpr std::string_view Help = "help - List all available commands";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            ResponseOk(writer, Name);
            writer.Key("commands");
            writer.BeginArray();
            if (context.help_entries) {
                for (const auto& entry : *context.help_entries) {
                    writer.BeginObject()
                        .Key("name")
                        .WriteString(entry.name)
                        .Key("description")
                        .WriteString(entry.description)
                        .EndObject();
                }
            }
            writer.EndArray();
            EndResponse(writer);
        }
    };
} // namespace VtxCli
