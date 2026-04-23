#pragma once

#include "commands/command_registry.h"
#include "commands/command_helpers.h"

#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace VtxCli {

    template <FormatWriter Fmt>
    struct OpenCommand {
        static constexpr std::string_view Name = "open";
        static constexpr std::string_view Help = "open <path> [--json-only] - Open a VTX replay file";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (args.empty()) {
                ResponseError(writer, Name, "Usage: open <path> [--json-only]");
                return;
            }

            bool json_only = false;
            std::vector<std::string> path_tokens;
            path_tokens.reserve(args.size());
            for (const auto& token : args) {
                if (token == "--json-only") {
                    json_only = true;
                    continue;
                }
                path_tokens.push_back(token);
            }
            if (path_tokens.empty()) {
                ResponseError(writer, Name, "Usage: open <path> [--json-only]");
                return;
            }

            std::string path = path_tokens[0];
            for (size_t i = 1; i < path_tokens.size(); ++i) {
                path += ' ';
                path += path_tokens[i];
            }

            if (context.session.IsLoaded()) {
                context.session.Close();
            }

            std::streambuf* old_stdout = nullptr;
            if (json_only) {
                old_stdout = std::cout.rdbuf(std::cerr.rdbuf());
            }

            const bool open_ok = context.session.Open(path);

            if (old_stdout) {
                std::cout.rdbuf(old_stdout);
            }

            if (!open_ok) {
                ResponseError(writer, Name, context.session.GetLastError());
                return;
            }

            const auto& footer = context.session.GetFooter();

            ResponseOk(writer, Name)
                .Key("file")
                .WriteString(context.session.GetFilePath())
                .Key("format")
                .WriteString(context.session.GetFormat() == VTX::VtxFormat::FlatBuffers ? "flatbuffers" : "protobuf")
                .Key("total_frames")
                .WriteInt(context.session.GetTotalFrames())
                .Key("duration_seconds")
                .WriteFloat(footer.duration_seconds)
                .Key("file_size_mb")
                .WriteFloat(context.session.GetFileSizeMb());
            EndResponse(writer);
        }
    };


    template <FormatWriter Fmt>
    struct CloseCommand {
        static constexpr std::string_view Name = "close";
        static constexpr std::string_view Help = "close - Close the current replay file";

        void Run(CommandContext& context, std::span<const std::string> args, Fmt& writer) {
            if (!RequireLoaded(context, writer, Name))
                return;

            context.session.Close();
            ResponseOk(writer, Name);
            EndResponse(writer);
        }
    };

} // namespace VtxCli
