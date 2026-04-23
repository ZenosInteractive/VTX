#pragma once
#include "commands/command_registry.h"
#include "commands/command_helpers.h"
#include "format/vtx_type_serializer.h"
#include <algorithm>
#include <cctype>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace VtxCli {

    namespace detail {

        inline const char* FieldTypeName(VTX::FieldType t) {
            switch (t) {
            case VTX::FieldType::Bool:
                return "bool";
            case VTX::FieldType::Int8:
                return "int8";
            case VTX::FieldType::Int32:
                return "int32";
            case VTX::FieldType::Int64:
                return "int64";
            case VTX::FieldType::Float:
                return "float";
            case VTX::FieldType::Double:
                return "double";
            case VTX::FieldType::String:
                return "string";
            case VTX::FieldType::Vector:
                return "vector";
            case VTX::FieldType::Quat:
                return "quat";
            case VTX::FieldType::Transform:
                return "transform";
            case VTX::FieldType::FloatRange:
                return "float_range";
            case VTX::FieldType::Struct:
                return "struct";
            case VTX::FieldType::Enum:
                return "enum";
            default:
                return "unknown";
            }
        }

        inline const char* ContainerTypeName(VTX::FieldContainerType t) {
            switch (t) {
            case VTX::FieldContainerType::Array:
                return "array";
            case VTX::FieldContainerType::Map:
                return "map";
            default:
                return "scalar";
            }
        }

        struct BucketRef {
            const VTX::Bucket* bucket = nullptr;
            std::string name;
            int32_t index = -1;
        };

        inline std::string ToLowerCopy(const std::string& value) {
            std::string lowered = value;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lowered;
        }

        inline std::vector<std::string> ExtractBucketNamesFromPropertyMapping(const std::string& property_mapping) {
            std::vector<std::string> names;
            if (property_mapping.empty())
                return names;

            const std::string key = "\"buckets\"";
            size_t key_pos = property_mapping.find(key);
            if (key_pos == std::string::npos)
                return names;

            size_t open_bracket = property_mapping.find('[', key_pos + key.size());
            if (open_bracket == std::string::npos)
                return names;

            bool in_string = false;
            bool escaping = false;
            std::string current;
            for (size_t i = open_bracket + 1; i < property_mapping.size(); ++i) {
                char c = property_mapping[i];
                if (!in_string) {
                    if (c == ']')
                        break;
                    if (c == '"') {
                        in_string = true;
                        current.clear();
                    }
                    continue;
                }
                if (escaping) {
                    current.push_back(c);
                    escaping = false;
                    continue;
                }
                if (c == '\\') {
                    escaping = true;
                    continue;
                }
                if (c == '"') {
                    in_string = false;
                    if (!current.empty())
                        names.push_back(current);
                    continue;
                }
                current.push_back(c);
            }
            return names;
        }

        inline BucketRef ResolveBucket(const CommandContext& ctx, const VTX::Frame& frame, const std::string& arg) {
            const auto& buckets = frame.GetBuckets();
            const auto schema_bucket_names =
                ExtractBucketNamesFromPropertyMapping(ctx.session.GetContextualSchema().property_mapping);

            bool is_number =
                !arg.empty() && std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); });

            if (is_number) {
                auto idx = static_cast<size_t>(std::stoul(arg));
                if (idx < buckets.size()) {
                    for (const auto& [bname, bidx] : frame.bucket_map) {
                        if (bidx == idx)
                            return {&buckets[idx], bname, static_cast<int32_t>(idx)};
                    }
                    if (idx < schema_bucket_names.size()) {
                        return {&buckets[idx], schema_bucket_names[idx], static_cast<int32_t>(idx)};
                    }
                    return {&buckets[idx], std::to_string(idx), static_cast<int32_t>(idx)};
                }
                return {};
            }

            auto it = frame.bucket_map.find(arg);
            if (it != frame.bucket_map.end()) {
                return {&frame.GetBuckets()[it->second], arg, static_cast<int32_t>(it->second)};
            }

            const std::string lowered_arg = ToLowerCopy(arg);
            for (const auto& [bname, bidx] : frame.bucket_map) {
                if (ToLowerCopy(bname) == lowered_arg) {
                    return {&frame.GetBuckets()[bidx], bname, static_cast<int32_t>(bidx)};
                }
            }

            for (size_t i = 0; i < schema_bucket_names.size() && i < buckets.size(); ++i) {
                if (schema_bucket_names[i] == arg || ToLowerCopy(schema_bucket_names[i]) == lowered_arg) {
                    return {&buckets[i], schema_bucket_names[i], static_cast<int32_t>(i)};
                }
            }

            return {};
        }

        inline int ResolveEntityIndex(const VTX::Bucket& bucket, const std::string& arg) {
            bool is_number =
                !arg.empty() && std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); });

            if (is_number) {
                try {
                    int idx = std::stoi(arg);
                    if (idx >= 0 && static_cast<size_t>(idx) < bucket.entities.size())
                        return idx;
                } catch (...) {}
            }

            for (size_t i = 0; i < bucket.unique_ids.size(); ++i) {
                if (bucket.unique_ids[i] == arg)
                    return static_cast<int>(i);
            }
            return -1;
        }


        struct EntitySelector {
            enum class Mode { Positional, ByIndex, ById };
            Mode mode = Mode::Positional;
            std::string value;
        };

        inline std::pair<EntitySelector, size_t> ParseEntitySelector(std::span<const std::string> args,
                                                                     size_t start = 0) {
            if (start + 1 < args.size()) {
                if (args[start] == "--index") {
                    return {{EntitySelector::Mode::ByIndex, std::string(args[start + 1])}, 2};
                }
                if (args[start] == "--id") {
                    return {{EntitySelector::Mode::ById, std::string(args[start + 1])}, 2};
                }
            }
            if (start < args.size()) {
                return {{EntitySelector::Mode::Positional, std::string(args[start])}, 1};
            }
            return {{}, 0};
        }

        /// Resolve entity from selector. Returns index or -1.
        inline int ResolveEntity(const VTX::Bucket& bucket, const EntitySelector& sel) {
            switch (sel.mode) {
            case EntitySelector::Mode::ByIndex: {
                try {
                    int idx = std::stoi(sel.value);
                    if (idx >= 0 && static_cast<size_t>(idx) < bucket.entities.size())
                        return idx;
                } catch (...) {}
                return -1;
            }
            case EntitySelector::Mode::ById: {
                for (size_t i = 0; i < bucket.unique_ids.size(); ++i) {
                    if (bucket.unique_ids[i] == sel.value)
                        return static_cast<int>(i);
                }
                return -1;
            }
            default:
                return ResolveEntityIndex(bucket, sel.value);
            }
        }

    } // namespace detail


    template <FormatWriter Fmt>
    struct BucketsCommand {
        static constexpr std::string_view Name = "buckets";
        static constexpr std::string_view Help = "buckets - List buckets in current frame";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w) {
            if (!RequireLoaded(ctx, w, Name))
                return;

            const auto* frame = ctx.session.GetCurrentFrameData();
            if (!frame) {
                ResponseError(w, Name, "Failed to load frame " + std::to_string(ctx.session.GetCurrentFrame()));
                return;
            }

            const auto& buckets = frame->GetBuckets();

            ResponseOk(w, Name)
                .Key("frame")
                .WriteInt(ctx.session.GetCurrentFrame())
                .Key("count")
                .WriteInt(static_cast<int32_t>(buckets.size()));

            std::unordered_map<size_t, std::string> idx_to_name;
            for (const auto& [bname, bidx] : frame->bucket_map) {
                idx_to_name[bidx] = bname;
            }
            const auto schema_bucket_names =
                detail::ExtractBucketNamesFromPropertyMapping(ctx.session.GetContextualSchema().property_mapping);
            for (size_t i = 0; i < schema_bucket_names.size() && i < buckets.size(); ++i) {
                if (!idx_to_name.contains(i) && !schema_bucket_names[i].empty()) {
                    idx_to_name[i] = schema_bucket_names[i];
                }
            }

            w.Key("buckets");
            w.BeginArray();
            for (size_t i = 0; i < buckets.size(); ++i) {
                const auto& bucket = buckets[i];
                auto name_it = idx_to_name.find(i);

                w.BeginObject().Key("index").WriteInt(static_cast<int32_t>(i));

                if (name_it != idx_to_name.end()) {
                    w.Key("name").WriteString(name_it->second);
                } else {
                    w.Key("name").WriteNull();
                }

                w.Key("entity_count").WriteInt(static_cast<int32_t>(bucket.entities.size())).EndObject();
            }
            w.EndArray();
            EndResponse(w);
        }
    };


    template <FormatWriter Fmt>
    struct EntitiesCommand {
        static constexpr std::string_view Name = "entities";
        static constexpr std::string_view Help = "entities <bucket> - List entities (by bucket name or index)";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w) {
            if (!RequireLoaded(ctx, w, Name))
                return;

            if (args.empty()) {
                ResponseError(w, Name, "Usage: entities <bucket_name_or_index>");
                return;
            }

            const auto* frame = ctx.session.GetCurrentFrameData();
            if (!frame) {
                ResponseError(w, Name, "Failed to load frame " + std::to_string(ctx.session.GetCurrentFrame()));
                return;
            }

            auto ref = detail::ResolveBucket(ctx, *frame, std::string(args[0]));
            if (!ref.bucket) {
                ResponseError(w, Name, "Bucket not found: " + std::string(args[0]));
                return;
            }

            const auto& cache = ctx.session.GetPropertyCache();
            const auto& bucket = *ref.bucket;

            ResponseOk(w, Name)
                .Key("bucket_index")
                .WriteInt(ref.index)
                .Key("bucket_name")
                .WriteString(ref.name)
                .Key("count")
                .WriteInt(static_cast<int32_t>(bucket.entities.size()));

            w.Key("entities");
            w.BeginArray();
            for (size_t i = 0; i < bucket.entities.size(); ++i) {
                const auto& entity = bucket.entities[i];
                w.BeginObject()
                    .Key("index")
                    .WriteInt(static_cast<int32_t>(i))
                    .Key("unique_id")
                    .WriteString(i < bucket.unique_ids.size() ? bucket.unique_ids[i] : "")
                    .Key("entity_type_id")
                    .WriteInt(entity.entity_type_id);

                auto it = cache.structs.find(entity.entity_type_id);
                if (it != cache.structs.end()) {
                    w.Key("type_name").WriteString(it->second.name);
                } else {
                    w.Key("type_name").WriteNull();
                }

                w.EndObject();
            }
            w.EndArray();
            EndResponse(w);
        }
    };


    template <FormatWriter Fmt>
    struct EntityCommand {
        static constexpr std::string_view Name = "entity";
        static constexpr std::string_view Help =
            "entity <bucket> <id_or_index> | --index <n> | --id <uid> - Show all properties";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w) {
            if (!RequireLoaded(ctx, w, Name))
                return;

            if (args.size() < 2) {
                ResponseError(w, Name,
                              "Usage: entity <bucket> <entity_id_or_index>\n"
                              "       entity <bucket> --index <n>\n"
                              "       entity <bucket> --id <uid>");
                return;
            }

            const auto* frame = ctx.session.GetCurrentFrameData();
            if (!frame) {
                ResponseError(w, Name, "Failed to load frame " + std::to_string(ctx.session.GetCurrentFrame()));
                return;
            }

            auto ref = detail::ResolveBucket(ctx, *frame, std::string(args[0]));
            if (!ref.bucket) {
                ResponseError(w, Name, "Bucket not found: " + std::string(args[0]));
                return;
            }

            auto [sel, consumed] = detail::ParseEntitySelector(args, 1);
            if (consumed == 0) {
                ResponseError(w, Name, "Missing entity selector");
                return;
            }

            int idx = detail::ResolveEntity(*ref.bucket, sel);
            if (idx < 0) {
                ResponseError(w, Name, "Entity not found: " + sel.value);
                return;
            }

            const auto& entity = ref.bucket->entities[idx];
            const auto& cache = ctx.session.GetPropertyCache();

            ResponseOk(w, Name)
                .Key("bucket_index")
                .WriteInt(ref.index)
                .Key("bucket_name")
                .WriteString(ref.name)
                .Key("entity_index")
                .WriteInt(idx)
                .Key("unique_id")
                .WriteString(static_cast<size_t>(idx) < ref.bucket->unique_ids.size() ? ref.bucket->unique_ids[idx]
                                                                                      : "");

            w.Key("entity");
            Serialize(w, entity, cache);
            EndResponse(w);
        }
    };


    template <FormatWriter Fmt>
    struct PropertyCommand {
        static constexpr std::string_view Name = "property";
        static constexpr std::string_view Help =
            "property <bucket> <entity> <name> | --index <n> <name> | --id <uid> <name>";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w) {
            if (!RequireLoaded(ctx, w, Name))
                return;

            if (args.size() < 3) {
                ResponseError(w, Name,
                              "Usage: property <bucket> <entity> <property_name>\n"
                              "       property <bucket> --index <n> <property_name>\n"
                              "       property <bucket> --id <uid> <property_name>");
                return;
            }

            const auto* frame = ctx.session.GetCurrentFrameData();
            if (!frame) {
                ResponseError(w, Name, "Failed to load frame " + std::to_string(ctx.session.GetCurrentFrame()));
                return;
            }

            auto ref = detail::ResolveBucket(ctx, *frame, std::string(args[0]));
            if (!ref.bucket) {
                ResponseError(w, Name, "Bucket not found: " + std::string(args[0]));
                return;
            }

            auto [sel, consumed] = detail::ParseEntitySelector(args, 1);
            if (consumed == 0) {
                ResponseError(w, Name, "Missing entity selector");
                return;
            }

            size_t prop_arg_idx = 1 + consumed;
            if (prop_arg_idx >= args.size()) {
                ResponseError(w, Name, "Missing property name");
                return;
            }

            int idx = detail::ResolveEntity(*ref.bucket, sel);
            if (idx < 0) {
                ResponseError(w, Name, "Entity not found: " + sel.value);
                return;
            }

            const auto& entity = ref.bucket->entities[idx];
            const auto& cache = ctx.session.GetPropertyCache();
            std::string prop_name(args[prop_arg_idx]);

            auto struct_it = cache.structs.find(entity.entity_type_id);
            if (struct_it == cache.structs.end()) {
                ResponseError(w, Name, "No schema for entity type " + std::to_string(entity.entity_type_id));
                return;
            }

            auto prop_it = struct_it->second.properties.find(prop_name);
            if (prop_it == struct_it->second.properties.end()) {
                std::string suggestion = SuggestProperty(cache, entity.entity_type_id, prop_name);
                if (!suggestion.empty()) {
                    ResponseErrorHint(w, Name, "Property not found: " + prop_name,
                                      "Did you mean '" + suggestion + "'?");
                } else {
                    ResponseError(w, Name, "Property not found: " + prop_name);
                }
                return;
            }

            ResponseOk(w, Name)
                .Key("bucket_index")
                .WriteInt(ref.index)
                .Key("bucket_name")
                .WriteString(ref.name)
                .Key("entity_index")
                .WriteInt(idx)
                .Key("unique_id")
                .WriteString(static_cast<size_t>(idx) < ref.bucket->unique_ids.size() ? ref.bucket->unique_ids[idx]
                                                                                      : "")
                .Key("property")
                .WriteString(prop_name)
                .Key("type")
                .WriteString(detail::FieldTypeName(prop_it->second.type_id))
                .Key("container")
                .WriteString(detail::ContainerTypeName(prop_it->second.container_type));

            w.Key("value");
            SerializeProperty(w, entity, prop_name, cache);
            EndResponse(w);
        }
    };


    // ── types [name] — discover struct types and their properties ───────

    template <FormatWriter Fmt>
    struct TypesCommand {
        static constexpr std::string_view Name = "types";
        static constexpr std::string_view Help = "types [name] - List struct types or inspect a type's properties";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w) {
            if (!RequireLoaded(ctx, w, Name))
                return;

            const auto& cache = ctx.session.GetPropertyCache();

            if (!args.empty()) {
                std::string type_name(args[0]);
                auto name_it = cache.name_to_id.find(type_name);
                if (name_it == cache.name_to_id.end()) {
                    std::string suggestion = SuggestTypeName(cache, type_name);
                    if (!suggestion.empty()) {
                        ResponseErrorHint(w, Name, "Unknown type: " + type_name, "Did you mean '" + suggestion + "'?");
                    } else {
                        ResponseError(w, Name, "Unknown type: " + type_name);
                    }
                    return;
                }

                auto struct_it = cache.structs.find(name_it->second);
                if (struct_it == cache.structs.end()) {
                    ResponseError(w, Name, "Schema not found for: " + type_name);
                    return;
                }

                const auto& sc = struct_it->second;

                ResponseOk(w, Name)
                    .Key("type_name")
                    .WriteString(sc.name)
                    .Key("type_id")
                    .WriteInt(name_it->second)
                    .Key("property_count")
                    .WriteInt(static_cast<int32_t>(sc.properties.size()));

                w.Key("properties");
                w.BeginArray();
                for (const auto& prop_view : sc.GetPropertiesInOrder()) {
                    w.BeginObject().Key("name").WriteString(std::string(prop_view.name));
                    if (prop_view.address) {
                        w.Key("type")
                            .WriteString(detail::FieldTypeName(prop_view.address->type_id))
                            .Key("container")
                            .WriteString(detail::ContainerTypeName(prop_view.address->container_type));
                        if (!prop_view.address->child_type_name.empty()) {
                            w.Key("child_type").WriteString(prop_view.address->child_type_name);
                        }
                    }
                    w.EndObject();
                }
                w.EndArray();
                EndResponse(w);
                return;
            }

            ResponseOk(w, Name).Key("count").WriteInt(static_cast<int32_t>(cache.name_to_id.size()));

            w.Key("types");
            w.BeginArray();
            for (const auto& [name, id] : cache.name_to_id) {
                auto struct_it = cache.structs.find(id);
                w.BeginObject()
                    .Key("name")
                    .WriteString(name)
                    .Key("type_id")
                    .WriteInt(id)
                    .Key("property_count")
                    .WriteInt(struct_it != cache.structs.end()
                                  ? static_cast<int32_t>(struct_it->second.properties.size())
                                  : 0)
                    .EndObject();
            }
            w.EndArray();
            EndResponse(w);
        }
    };

} // namespace VtxCli
