#pragma once
#include "core/cli_concepts.h"
#include "commands/command_registry.h"
#include "vtx/common/vtx_property_cache.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace VtxCli {

    template<FormatWriter Fmt>
    Fmt& ResponseOk(Fmt& writer, std::string_view command_name)
    {
        writer.BeginObject()
            .Key("status").WriteString("ok")
            .Key("command").WriteString(std::string(command_name))
            .Key("data");
        return writer.BeginObject();
    }

    template<FormatWriter Fmt>
    void EndResponse(Fmt& w)
    {
        w.EndObject()
         .EndObject();
    }

    template<FormatWriter Fmt>
    Fmt& ResponseError(Fmt& writer, std::string_view command_name, const std::string& message)
    {
        writer.BeginObject()
            .Key("status").WriteString("error")
            .Key("command").WriteString(std::string(command_name))
            .Key("error").WriteString(message)
            .Key("hint").WriteNull()
            .EndObject();
        return writer;
    }

    /// Error response with a hint (e.g. "did you mean X?")
    template<FormatWriter Fmt>
    Fmt& ResponseErrorHint(Fmt& writer, std::string_view command_name,
                           const std::string& message, const std::string& hint)
    {
        writer.BeginObject()
            .Key("status").WriteString("error")
            .Key("command").WriteString(std::string(command_name))
            .Key("error").WriteString(message)
            .Key("hint").WriteString(hint)
            .EndObject();
        return writer;
    }

    //returns true if a file is loaded, false after writing error.
    template<FormatWriter Fmt>
    bool RequireLoaded(const CommandContext& context, Fmt& writer, std::string_view command_name)
    {
        if (context.session.IsLoaded()) return true;
        ResponseError(writer, command_name, "No file loaded");
        return false;
    }



    /// Case-insensitive Levenshtein distance (for short strings).
    inline size_t EditDistance(std::string_view a, std::string_view b) {
        const size_t m = a.size(), n = b.size();
        std::vector<size_t> prev(n + 1), curr(n + 1);
        for (size_t j = 0; j <= n; ++j) prev[j] = j;
        for (size_t i = 1; i <= m; ++i) {
            curr[0] = i;
            for (size_t j = 1; j <= n; ++j) {
                char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i-1])));
                char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j-1])));
                size_t cost = (ca == cb) ? 0 : 1;
                curr[j] = std::min({curr[j-1] + 1, prev[j] + 1, prev[j-1] + cost});
            }
            std::swap(prev, curr);
        }
        return prev[n];
    }

    /// Find the closest match from a set of candidates.
    /// Returns empty string if no match is close enough (threshold = max edit distance).
    inline std::string FindClosestMatch(const std::string& input,
                                        const std::vector<std::string>& candidates,
                                        size_t threshold = 3)
    {
        std::string best;
        size_t best_dist = threshold + 1;
        for (const auto& c : candidates) {
            size_t d = EditDistance(input, c);
            if (d < best_dist) {
                best_dist = d;
                best = c;
            }
        }
        return best;
    }

    /// Find closest property name for a given entity type.
    inline std::string SuggestProperty(const VTX::PropertyAddressCache& cache,
                                       int32_t entity_type_id,
                                       const std::string& input)
    {
        auto it = cache.structs.find(entity_type_id);
        if (it == cache.structs.end()) return {};
        std::vector<std::string> names;
        names.reserve(it->second.properties.size());
        for (const auto& [name, _] : it->second.properties) {
            names.push_back(name);
        }
        return FindClosestMatch(input, names);
    }

    /// Find closest type name.
    inline std::string SuggestTypeName(const VTX::PropertyAddressCache& cache,
                                       const std::string& input)
    {
        std::vector<std::string> names;
        names.reserve(cache.name_to_id.size());
        for (const auto& [name, _] : cache.name_to_id) {
            names.push_back(name);
        }
        return FindClosestMatch(input, names);
    }

} // namespace VtxCli
