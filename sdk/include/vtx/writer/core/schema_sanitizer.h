#pragma once
#include <functional>
#include <unordered_map>
#include <string>
#include <vtx/common/vtx_types.h>
#include <vtx/common/readers/schema_reader/game_schema_types.h>

namespace VTX {
    struct SanitizeContext {
        const std::string& struct_name;
        const SchemaStruct* schema;
        bool is_loading; //true if it comes from a external load, false if is before writing
    };

    using SanitizeFunc = std::function<void(PropertyContainer& container, const SanitizeContext& ctx)>;

    class SchemaSanitizerRegistry {
        std::unordered_map<std::string, SanitizeFunc> _processors;

    public:
        void Register(const std::string& structName, const SanitizeFunc& func) { _processors[structName] = func; }

        void TrySanitize(PropertyContainer& container, const SanitizeContext& ctx) const {
            auto it = _processors.find(ctx.struct_name);
            if (it != _processors.end()) {
                it->second(container, ctx);
            }
        }
    };
} // namespace VTX