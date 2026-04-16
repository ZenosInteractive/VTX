#pragma once
#include "commands/command_registry.h"
#include "commands/command_helpers.h"
#include "commands/inspect_commands.h"
#include "format/vtx_type_serializer.h"

#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace VtxCli {


    namespace detail {

        inline bool CompareNumeric(double a, const std::string& op, double b) {
            if (op == "==") return a == b;
            if (op == "!=") return a != b;
            if (op == ">")  return a > b;
            if (op == "<")  return a < b;
            if (op == ">=") return a >= b;
            if (op == "<=") return a <= b;
            return false;
        }

        inline bool MatchesScalar(
            const VTX::PropertyContainer& pc,
            const VTX::PropertyAddress& addr,
            const std::string& op,
            const std::string& target)
        {
            if (addr.container_type != VTX::FieldContainerType::None) return false;
            auto idx = static_cast<size_t>(addr.index);

            switch (addr.type_id) {
                case VTX::FieldType::Bool: {
                    if (idx >= pc.bool_properties.size()) return false;
                    bool val = static_cast<bool>(pc.bool_properties[idx]);
                    bool tgt = (target == "true" || target == "1");
                    if (op == "==") return val == tgt;
                    if (op == "!=") return val != tgt;
                    return false;
                }
                case VTX::FieldType::Int32:
                case VTX::FieldType::Enum: {
                    if (idx >= pc.int32_properties.size()) return false;
                    try { return CompareNumeric(pc.int32_properties[idx], op, std::stod(target)); }
                    catch (...) { return false; }
                }
                case VTX::FieldType::Int64: {
                    if (idx >= pc.int64_properties.size()) return false;
                    try { return CompareNumeric(static_cast<double>(pc.int64_properties[idx]), op, std::stod(target)); }
                    catch (...) { return false; }
                }
                case VTX::FieldType::Float: {
                    if (idx >= pc.float_properties.size()) return false;
                    try { return CompareNumeric(pc.float_properties[idx], op, std::stod(target)); }
                    catch (...) { return false; }
                }
                case VTX::FieldType::Double: {
                    if (idx >= pc.double_properties.size()) return false;
                    try { return CompareNumeric(pc.double_properties[idx], op, std::stod(target)); }
                    catch (...) { return false; }
                }
                case VTX::FieldType::String: {
                    if (idx >= pc.string_properties.size()) return false;
                    if (op == "==") return pc.string_properties[idx] == target;
                    if (op == "!=") return pc.string_properties[idx] != target;
                    return false;
                }
                default: return false;
            }
        }

        //Map EVTXContainerType (FieldType, FieldContainerType) for schema lookup ──

        inline std::pair<VTX::FieldType, VTX::FieldContainerType>
        MapContainerType(VtxDiff::EVTXContainerType ct) {
            using CT = VtxDiff::EVTXContainerType;
            using FT = VTX::FieldType;
            using FCT = VTX::FieldContainerType;
            switch (ct) {
                case CT::BoolProperties:       return {FT::Bool,       FCT::None};
                case CT::Int32Properties:      return {FT::Int32,      FCT::None};
                case CT::Int64Properties:      return {FT::Int64,      FCT::None};
                case CT::FloatProperties:      return {FT::Float,      FCT::None};
                case CT::DoubleProperties:     return {FT::Double,     FCT::None};
                case CT::StringProperties:     return {FT::String,     FCT::None};
                case CT::TransformProperties:  return {FT::Transform,  FCT::None};
                case CT::VectorProperties:     return {FT::Vector,     FCT::None};
                case CT::QuatProperties:       return {FT::Quat,       FCT::None};
                case CT::RangeProperties:      return {FT::FloatRange, FCT::None};
                case CT::AnyStructProperties:  return {FT::Struct,     FCT::None};
                case CT::BoolArrays:           return {FT::Bool,       FCT::Array};
                case CT::Int32Arrays:          return {FT::Int32,      FCT::Array};
                case CT::Int64Arrays:          return {FT::Int64,      FCT::Array};
                case CT::FloatArrays:          return {FT::Float,      FCT::Array};
                case CT::DoubleArrays:         return {FT::Double,     FCT::Array};
                case CT::StringArrays:         return {FT::String,     FCT::Array};
                case CT::TransformArrays:      return {FT::Transform,  FCT::Array};
                case CT::VectorArrays:         return {FT::Vector,     FCT::Array};
                case CT::QuatArrays:           return {FT::Quat,       FCT::Array};
                case CT::RangeArrays:          return {FT::FloatRange, FCT::Array};
                case CT::AnyStructArrays:      return {FT::Struct,     FCT::Array};
                case CT::MapProperties:        return {FT::Struct,     FCT::Map};
                case CT::MapArrays:            return {FT::Struct,     FCT::Map};
                default:                       return {FT::None,       FCT::None};
            }
        }

        inline std::string ResolvePropertyName(
            const VTX::StructSchemaCache& schema,
            VtxDiff::EVTXContainerType ct, int32_t index)
        {
            auto [ft, fct] = MapContainerType(ct);
            uint64_t key = VTX::MakePropertyLookupKey(index, ft, fct);
            auto it = schema.names_by_lookup_key.find(key);
            if (it != schema.names_by_lookup_key.end()) return it->second;
            return VtxDiff::TypeToFieldName(ct) + "[" + std::to_string(index) + "]";
        }

    } // namespace detail

    
    template<FormatWriter Fmt>
    struct DiffCommand {
        static constexpr std::string_view Name = "diff";
        static constexpr std::string_view Help =
            "diff <frame_a> <frame_b> [--epsilon <val>] - Compare entities between two frames";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w)
        {
            if (!RequireLoaded(ctx, w, Name)) return;

            if (args.size() < 2) {
                ResponseError(w, Name, "Usage: diff <frame_a> <frame_b> [--epsilon <value>]");
                return;
            }

            int32_t frame_a, frame_b;
            try {
                frame_a = std::stoi(std::string(args[0]));
                frame_b = std::stoi(std::string(args[1]));
            } catch (...) {
                ResponseError(w, Name, "Invalid frame numbers");
                return;
            }

            VtxDiff::DiffOptions opts;
            opts.compare_floats_with_epsilon = true;
            opts.float_epsilon = 1e-5f;

            // Parse optional --epsilon
            for (size_t i = 2; i + 1 < args.size(); ++i) {
                if (args[i] == "--epsilon") {
                    try { opts.float_epsilon = std::stof(std::string(args[i + 1])); }
                    catch (...) {
                        ResponseError(w, Name, "Invalid epsilon value: " + std::string(args[i + 1]));
                        return;
                    }
                    break;
                }
            }

            int32_t total = ctx.session.GetTotalFrames();
            if (frame_a < 0 || frame_a >= total || frame_b < 0 || frame_b >= total) {
                ResponseError(w, Name, "Frame out of range [0, " + std::to_string(total - 1) + "]");
                return;
            }

            auto patch = ctx.session.DiffFrames(frame_a, frame_b, opts);

            if (patch.operations.empty()) {
                std::string reason = (frame_a == frame_b)
                    ? "Same frame index; no diff possible"
                    : "Frames are identical";
                ResponseOk(w, Name)
                    .Key("frame_a").WriteInt(frame_a)
                    .Key("frame_b").WriteInt(frame_b)
                    .Key("total_ops").WriteInt(0)
                    .Key("reason").WriteString(reason);
                w.Key("changed"); w.BeginArray().EndArray();
                w.Key("added");   w.BeginArray().EndArray();
                w.Key("removed"); w.BeginArray().EndArray();
                EndResponse(w);
                return;
            }

            // Load frame B for entity type name resolution
            const auto* frame_data = ctx.session.GetFrameData(frame_b);
            const auto& cache = ctx.session.GetPropertyCache();

            std::unordered_map<std::string, int32_t> uid_to_type;
            if (frame_data) {
                for (const auto& bkt : frame_data->GetBuckets()) {
                    for (size_t i = 0; i < std::min(bkt.unique_ids.size(), bkt.entities.size()); ++i) {
                        uid_to_type[bkt.unique_ids[i]] = bkt.entities[i].entity_type_id;
                    }
                }
            }

            // Group operations by entity
            using CT = VtxDiff::EVTXContainerType;
            constexpr auto AnyStructArrays = static_cast<int32_t>(CT::AnyStructArrays);

            std::vector<std::string> added_ids;
            std::vector<std::string> removed_ids;
            // actor_id set of changed property names
            std::unordered_map<std::string, std::set<std::string>> changed_map;

            for (const auto& op : patch.operations) {
                const auto& path = op.Path;

                // Entity-level add/remove (path: [AnyStructArrays, actor_hash])
                if (path.count == 2 && path.indices[0] == AnyStructArrays) {
                    if (op.Operation == VtxDiff::DiffOperation::Add) {
                        added_ids.push_back(op.ActorId);
                    } else if (op.Operation == VtxDiff::DiffOperation::Remove) {
                        removed_ids.push_back(op.ActorId);
                    }
                    continue;
                }

                // Property-level change (path depth > 2)
                if (path.count > 2 && path.indices[0] == AnyStructArrays && !op.ActorId.empty()) {
                    auto container_type = static_cast<CT>(path.indices[2]);
                    int32_t prop_index = (path.count > 3) ? path.indices[3] : -1;

                    // Resolve to property name via schema
                    auto type_it = uid_to_type.find(op.ActorId);
                    if (type_it != uid_to_type.end() && prop_index >= 0) {
                        auto sc_it = cache.structs.find(type_it->second);
                        if (sc_it != cache.structs.end()) {
                            changed_map[op.ActorId].insert(
                                detail::ResolvePropertyName(sc_it->second, container_type, prop_index));
                            continue;
                        }
                    }
                    // Fallback: use container type name
                    changed_map[op.ActorId].insert(
                        VtxDiff::TypeToFieldName(container_type) + "[" + std::to_string(prop_index) + "]");
                }
            }

            // Write output
            ResponseOk(w, Name)
                .Key("frame_a").WriteInt(frame_a)
                .Key("frame_b").WriteInt(frame_b)
                .Key("total_ops").WriteInt(static_cast<int32_t>(patch.operations.size()));

            // Helper: resolve type_name from uid, always writes the key
            auto writeTypeName = [&](const std::string& uid) {
                auto type_it = uid_to_type.find(uid);
                if (type_it != uid_to_type.end()) {
                    auto sc_it = cache.structs.find(type_it->second);
                    if (sc_it != cache.structs.end()) {
                        w.Key("type_name").WriteString(sc_it->second.name);
                        return;
                    }
                }
                w.Key("type_name").WriteNull();
            };

            w.Key("changed");
            w.BeginArray();
            for (const auto& [uid, props] : changed_map) {
                w.BeginObject()
                    .Key("unique_id").WriteString(uid)
                    .Key("operation").WriteString("modify");
                writeTypeName(uid);
                w.Key("property_count").WriteInt(static_cast<int32_t>(props.size()));
                w.Key("properties");
                w.BeginArray();
                for (const auto& prop : props) w.WriteString(prop);
                w.EndArray();
                w.EndObject();
            }
            w.EndArray();

            w.Key("added");
            w.BeginArray();
            for (const auto& uid : added_ids) {
                w.BeginObject()
                    .Key("unique_id").WriteString(uid)
                    .Key("operation").WriteString("add");
                writeTypeName(uid);
                w.EndObject();
            }
            w.EndArray();

            w.Key("removed");
            w.BeginArray();
            for (const auto& uid : removed_ids) {
                w.BeginObject()
                    .Key("unique_id").WriteString(uid)
                    .Key("operation").WriteString("remove");
                writeTypeName(uid);
                w.EndObject();
            }
            w.EndArray();

            EndResponse(w);
        }
    };
    
    template<FormatWriter Fmt>
    struct TrackCommand {
        static constexpr std::string_view Name = "track";
        static constexpr std::string_view Help =
            "track <bucket> <entity> <prop> <start> <end> [step] | --index <n> | --id <uid>";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w)
        {
            if (!RequireLoaded(ctx, w, Name)) return;

            if (args.size() < 5) {
                ResponseError(w, Name,
                    "Usage: track <bucket> <entity> <property> <start> <end> [step]\n"
                    "       track <bucket> --index <n> <property> <start> <end> [step]\n"
                    "       track <bucket> --id <uid> <property> <start> <end> [step]");
                return;
            }

            const std::string bucket_arg(args[0]);

            auto [sel, consumed] = detail::ParseEntitySelector(args, 1);
            if (consumed == 0) {
                ResponseError(w, Name, "Missing entity selector");
                return;
            }

            size_t next = 1 + consumed;
            if (next + 2 >= args.size()) {
                ResponseError(w, Name, "Missing property name or frame range");
                return;
            }

            const std::string prop_name(args[next]);

            int32_t start_frame, end_frame, step = 1;
            try {
                start_frame = std::stoi(std::string(args[next + 1]));
                end_frame   = std::stoi(std::string(args[next + 2]));
                if (next + 3 < args.size()) step = std::stoi(std::string(args[next + 3]));
            } catch (...) {
                ResponseError(w, Name, "Invalid frame range or step");
                return;
            }

            if (step <= 0) { ResponseError(w, Name, "Step must be > 0"); return; }
            if (start_frame > end_frame) { ResponseError(w, Name, "start must be <= end"); return; }

            int32_t total = ctx.session.GetTotalFrames();
            if (start_frame < 0 || start_frame >= total || end_frame < 0 || end_frame >= total) {
                ResponseError(w, Name, "Frame out of range [0, " + std::to_string(total - 1) + "]");
                return;
            }

            // Validate bucket/entity/property on start_frame before streaming
            const auto* first_frame = ctx.session.GetFrameData(start_frame);
            if (!first_frame) {
                ResponseError(w, Name, "Failed to load frame " + std::to_string(start_frame));
                return;
            }
            auto first_ref = detail::ResolveBucket(ctx, *first_frame, bucket_arg);
            if (!first_ref.bucket) {
                ResponseError(w, Name, "Bucket not found: " + bucket_arg);
                return;
            }
            int first_eidx = detail::ResolveEntity(*first_ref.bucket, sel);
            if (first_eidx < 0) {
                ResponseError(w, Name, "Entity not found: " + sel.value);
                return;
            }

            const auto& cache = ctx.session.GetPropertyCache();
            const auto& first_entity = first_ref.bucket->entities[first_eidx];
            auto sc_it = cache.structs.find(first_entity.entity_type_id);
            if (sc_it == cache.structs.end()) {
                ResponseError(w, Name, "No schema for entity type " + std::to_string(first_entity.entity_type_id));
                return;
            }
            if (!sc_it->second.properties.contains(prop_name)) {
                std::string suggestion = SuggestProperty(cache, first_entity.entity_type_id, prop_name);
                if (!suggestion.empty()) {
                    ResponseErrorHint(w, Name,
                        "Property not found: " + prop_name,
                        "Did you mean '" + suggestion + "'?");
                } else {
                    ResponseError(w, Name, "Property not found: " + prop_name);
                }
                return;
            }

            ResponseOk(w, Name)
                .Key("entity").WriteString(sel.value)
                .Key("property").WriteString(prop_name)
                .Key("start_frame").WriteInt(start_frame)
                .Key("end_frame").WriteInt(end_frame)
                .Key("step").WriteInt(step);

            w.Key("samples");
            w.BeginArray();

            for (int32_t f = start_frame; f <= end_frame; f += step) {
                const auto* frame = ctx.session.GetFrameData(f);
                if (!frame) continue;

                auto ref = detail::ResolveBucket(ctx, *frame, bucket_arg);
                if (!ref.bucket) continue;

                int eidx = detail::ResolveEntity(*ref.bucket, sel);
                if (eidx < 0) continue;

                const auto& entity = ref.bucket->entities[eidx];

                w.BeginObject()
                    .Key("frame").WriteInt(f);

                w.Key("value");
                if (!SerializeProperty(w, entity, prop_name, cache)) {
                    w.WriteNull();
                }

                w.EndObject();
            }

            w.EndArray();
            EndResponse(w);
        }
    };
    
    template<FormatWriter Fmt>
    struct SearchCommand {
        static constexpr std::string_view Name = "search";
        static constexpr std::string_view Help = "search <property> <op> <value> [bucket] - Find entities by property value";

        void Run(CommandContext& ctx, std::span<const std::string> args, Fmt& w)
        {
            if (!RequireLoaded(ctx, w, Name)) return;

            if (args.size() < 3) {
                ResponseError(w, Name, "Usage: search <property> <op> <value> [bucket]  (ops: == != > < >= <=)");
                return;
            }

            const std::string prop_name(args[0]);
            const std::string op(args[1]);
            const std::string target(args[2]);

            static const std::unordered_set<std::string> valid_ops = {"==","!=",">","<",">=","<="};
            if (!valid_ops.contains(op)) {
                ResponseError(w, Name, "Invalid operator: " + op + "  (use == != > < >= <=)");
                return;
            }

            const auto* frame = ctx.session.GetCurrentFrameData();
            if (!frame) {
                ResponseError(w, Name, "Failed to load frame " + std::to_string(ctx.session.GetCurrentFrame()));
                return;
            }

            const auto& cache = ctx.session.GetPropertyCache();
            int32_t only_bucket = -1;
            if (args.size() >= 4) {
                auto ref = detail::ResolveBucket(ctx, *frame, std::string(args[3]));
                if (!ref.bucket) {
                    ResponseError(w, Name, "Bucket not found: " + std::string(args[3]));
                    return;
                }
                only_bucket = ref.index;
            }

            // Pre-check: does this property exist in ANY schema type?
            bool property_exists_anywhere = false;
            for (const auto& [_, sc] : cache.structs) {
                if (sc.properties.contains(prop_name)) {
                    property_exists_anywhere = true;
                    break;
                }
            }
            if (!property_exists_anywhere) {
                // Find closest property name across all types
                std::string suggestion;
                size_t best_dist = 4;
                for (const auto& [_, sc] : cache.structs) {
                    for (const auto& [pname, __] : sc.properties) {
                        size_t d = EditDistance(prop_name, pname);
                        if (d < best_dist) { best_dist = d; suggestion = pname; }
                    }
                }
                if (!suggestion.empty()) {
                    ResponseErrorHint(w, Name,
                        "Property '" + prop_name + "' not found in any entity type",
                        "Did you mean '" + suggestion + "'?");
                } else {
                    ResponseError(w, Name,
                        "Property '" + prop_name + "' not found in any entity type");
                }
                return;
            }

            // Collect matches
            int32_t match_count = 0;
            struct Match {
                int32_t bi; int32_t ei;
                std::string uid; std::string type;
                const VTX::PropertyContainer* entity;
            };
            std::vector<Match> matches;

            const auto& buckets = frame->GetBuckets();
            for (size_t bi = 0; bi < buckets.size(); ++bi) {
                if (only_bucket >= 0 && static_cast<int32_t>(bi) != only_bucket) continue;
                const auto& bucket = buckets[bi];
                for (size_t ei = 0; ei < bucket.entities.size(); ++ei) {
                    const auto& entity = bucket.entities[ei];
                    auto sc_it = cache.structs.find(entity.entity_type_id);
                    if (sc_it == cache.structs.end()) continue;
                    auto pr_it = sc_it->second.properties.find(prop_name);
                    if (pr_it == sc_it->second.properties.end()) continue;
                    if (!detail::MatchesScalar(entity, pr_it->second, op, target)) continue;
                    matches.push_back({
                        static_cast<int32_t>(bi), static_cast<int32_t>(ei),
                        ei < bucket.unique_ids.size() ? bucket.unique_ids[ei] : std::string{},
                        sc_it->second.name, &entity});
                }
            }

            ResponseOk(w, Name)
                .Key("frame").WriteInt(ctx.session.GetCurrentFrame())
                .Key("property").WriteString(prop_name)
                .Key("operator").WriteString(op)
                .Key("value").WriteString(target)
                .Key("match_count").WriteInt(static_cast<int32_t>(matches.size()));

            w.Key("matches");
            w.BeginArray();
            for (const auto& m : matches) {
                w.BeginObject()
                    .Key("bucket_index").WriteInt(m.bi)
                    .Key("entity_index").WriteInt(m.ei)
                    .Key("unique_id").WriteString(m.uid)
                    .Key("type_name").WriteString(m.type);
                w.Key("current_value");
                SerializeProperty(w, *m.entity, prop_name, cache);
                w.EndObject();
            }
            w.EndArray();
            EndResponse(w);
        }
    };

} // namespace VtxCli
