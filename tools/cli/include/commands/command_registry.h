#pragma once


#include "core/cli_concepts.h"
#include "core/cli_session.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace VtxCli {

    /// Levenshtein distance for command suggestion (case-insensitive).
    inline size_t CmdEditDistance(std::string_view a, std::string_view b) {
        const size_t m = a.size(), n = b.size();
        std::vector<size_t> prev(n + 1), curr(n + 1);
        for (size_t j = 0; j <= n; ++j)
            prev[j] = j;
        for (size_t i = 1; i <= m; ++i) {
            curr[0] = i;
            for (size_t j = 1; j <= n; ++j) {
                char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i - 1])));
                char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j - 1])));
                size_t cost = (ca == cb) ? 0 : 1;
                curr[j] = std::min({curr[j - 1] + 1, prev[j] + 1, prev[j - 1] + cost});
            }
            std::swap(prev, curr);
        }
        return prev[n];
    }

    inline std::string FindClosestCommand(const std::string& input, const std::vector<std::string>& candidates,
                                          size_t threshold = 3) {
        std::string best;
        size_t best_dist = threshold + 1;
        for (const auto& c : candidates) {
            size_t d = CmdEditDistance(input, c);
            if (d < best_dist) {
                best_dist = d;
                best = c;
            }
        }
        return best;
    }

    struct CommandMeta {
        std::string name;
        std::string description;
    };


    struct CommandContext {
        CliSession& session;
        bool exit_requested = false;
        const std::vector<CommandMeta>* help_entries = nullptr;
    };


    inline std::vector<std::string> Tokenize(const std::string& line) {
        std::vector<std::string> tokens;
        std::string current;
        bool in_quotes = false;

        for (char c : line) {
            if (in_quotes) {
                if (c == '"') {
                    in_quotes = false;
                } else {
                    current += c;
                }
            } else {
                if (c == '"') {
                    in_quotes = true;
                } else if (c == ' ' || c == '\t') {
                    if (!current.empty()) {
                        tokens.push_back(std::move(current));
                        current.clear();
                    }
                } else {
                    current += c;
                }
            }
        }
        if (!current.empty()) {
            tokens.push_back(std::move(current));
        }
        return tokens;
    }

    template <FormatWriter Fmt>
    class CommandRegistry {
    public:
        /// Register a command template.  Cmd must expose:
        ///   static constexpr std::string_view Name;
        ///   static constexpr std::string_view Help;
        ///   void Run(CommandContext&, std::span<const std::string>, Fmt&);
        template <template <typename> typename Cmd>
        void Register() {
            Cmd<Fmt> cmd;
            std::string name {Cmd<Fmt>::Name};
            help_entries_.push_back({name, std::string {Cmd<Fmt>::Help}});
            handlers_[name] = [cmd = std::move(cmd)](CommandContext& ctx,
                                                     std::span<const std::string> args) mutable -> std::string {
                Fmt w;
                cmd.Run(ctx, args, w);
                return w.Finalize("");
            };
        }

        /// Tokenize the line, look up the handler, execute, return response JSON.
        std::string Dispatch(CommandContext& ctx, const std::string& line) {
            auto tokens = Tokenize(line);
            if (tokens.empty())
                return {};

            const std::string& cmd_name = tokens[0];
            auto it = handlers_.find(cmd_name);

            if (it == handlers_.end()) {
                Fmt w;
                w.BeginObject()
                    .Key("status")
                    .WriteString("error")
                    .Key("command")
                    .WriteString(cmd_name)
                    .Key("error")
                    .WriteString("Unknown command. Type 'help' for available commands.");

                // Suggest closest command name
                std::vector<std::string> names;
                names.reserve(handlers_.size());
                for (const auto& [n, _] : handlers_)
                    names.push_back(n);
                std::string closest = FindClosestCommand(cmd_name, names);
                if (!closest.empty()) {
                    w.Key("hint").WriteString("Did you mean '" + closest + "'?");
                } else {
                    w.Key("hint").WriteNull();
                }

                w.EndObject();
                return w.Finalize("");
            }

            std::span<const std::string> args;
            if (tokens.size() > 1) {
                args = std::span<const std::string>(tokens.data() + 1, tokens.size() - 1);
            }
            return it->second(ctx, args);
        }

        const std::vector<CommandMeta>& GetHelpEntries() const { return help_entries_; }

    private:
        using ErasedFn = std::function<std::string(CommandContext&, std::span<const std::string>)>;
        std::unordered_map<std::string, ErasedFn> handlers_;
        std::vector<CommandMeta> help_entries_;
    };

} // namespace VtxCli
