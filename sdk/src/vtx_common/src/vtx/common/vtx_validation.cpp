/**
 * @file vtx_validation.cpp
 * @brief Implementation of the independently callable validation passes.
 * @author Zenos Interactive
 */
#include "vtx/common/vtx_validation.h"

#include <array>
#include <string>
#include <unordered_set>

#include "vtx/common/readers/schema_reader/schema_enums.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/readers/schema_reader/schema_validator.h"

namespace VTX {

    namespace {

        VtxDiagnostic MakeDiagnostic(VtxErrorCode code, Severity severity, std::string message,
                                     const char* source_api) {
            VtxDiagnostic diagnostic;
            diagnostic.code = code;
            diagnostic.severity = severity;
            diagnostic.message = std::move(message);
            diagnostic.source_api = source_api;
            return diagnostic;
        }

        // One scalar SoA array of an entity, paired with its FieldType, so the
        // per-type bound check can be a simple loop.
        struct ScalarArrayView {
            FieldType type;
            size_t provided;
        };

        std::array<ScalarArrayView, 11> ScalarArrays(const PropertyContainer& entity) {
            return {{
                {FieldType::Bool, entity.bool_properties.size()},
                {FieldType::Int32, entity.int32_properties.size()},
                {FieldType::Int64, entity.int64_properties.size()},
                {FieldType::Float, entity.float_properties.size()},
                {FieldType::Double, entity.double_properties.size()},
                {FieldType::String, entity.string_properties.size()},
                {FieldType::Vector, entity.vector_properties.size()},
                {FieldType::Quat, entity.quat_properties.size()},
                {FieldType::Transform, entity.transform_properties.size()},
                {FieldType::FloatRange, entity.range_properties.size()},
                {FieldType::Struct, entity.any_struct_properties.size()},
            }};
        }

    } // namespace

    ValidationReport ValidateSchema(const std::string& schema_json) {
        return SchemaValidator {}.Validate(schema_json).ToReport("ValidateSchema");
    }

    ValidationReport ValidateEntity(const PropertyContainer& entity, const PropertyAddressCache& schema,
                                    const EntityLocation& where) {
        ValidationReport report;

        const auto stamp = [&](VtxDiagnostic& diagnostic) {
            diagnostic.frame_index = where.frame_index;
            diagnostic.bucket = where.bucket;
            diagnostic.unique_id = where.unique_id;
            diagnostic.source_api = "ValidateEntity";
        };

        const auto struct_it = schema.structs.find(entity.entity_type_id);
        if (entity.entity_type_id < 0 || struct_it == schema.structs.end()) {
            VtxDiagnostic diagnostic = MakeDiagnostic(VtxErrorCode::EntityTypeUnresolved, Severity::Error,
                                                      "entity type id " + std::to_string(entity.entity_type_id) +
                                                          " does not resolve to a schema struct",
                                                      "ValidateEntity");
            stamp(diagnostic);
            diagnostic.entity_type = std::to_string(entity.entity_type_id);
            report.Add(std::move(diagnostic));
            return report;
        }

        const StructSchemaCache& struct_cache = struct_it->second;
        for (const ScalarArrayView& array : ScalarArrays(entity)) {
            const size_t type_index = static_cast<size_t>(array.type);
            const int32_t max_count =
                type_index < struct_cache.type_max_indices.size() ? struct_cache.type_max_indices[type_index] : 0;
            if (static_cast<int64_t>(array.provided) > static_cast<int64_t>(max_count)) {
                const std::string type_name(VTX::ToString(array.type)); // reuse the schema-enum names
                VtxDiagnostic diagnostic = MakeDiagnostic(VtxErrorCode::FieldIndexOutOfRange, Severity::Error,
                                                          "entity provides " + std::to_string(array.provided) + " " +
                                                              type_name + " value(s) but schema '" + struct_cache.name +
                                                              "' defines at most " + std::to_string(max_count),
                                                          "ValidateEntity");
                stamp(diagnostic);
                diagnostic.entity_type = struct_cache.name;
                diagnostic.field_path = struct_cache.name + "[" + type_name + "]";
                diagnostic.expected_type = type_name;
                diagnostic.expected_container = "scalar";
                diagnostic.provided_type = type_name;
                diagnostic.provided_container = "scalar";
                report.Add(std::move(diagnostic));
            }
        }

        return report;
    }

    ValidationReport ValidateEntity(const PropertyContainer& entity, const SchemaRegistry& schema,
                                    const EntityLocation& where) {
        return ValidateEntity(entity, schema.GetPropertyCache(), where);
    }

    ValidationReport ValidateFrame(const Frame& frame, const PropertyAddressCache& schema, int32_t frame_index) {
        ValidationReport report;

        for (const auto& [bucket_name, bucket_index] : frame.bucket_map) {
            if (bucket_index >= frame.buckets.size()) {
                continue;
            }
            const Bucket& bucket = frame.buckets[bucket_index];

            std::unordered_set<std::string> seen_ids;
            for (const std::string& unique_id : bucket.unique_ids) {
                if (unique_id.empty()) {
                    continue;
                }
                if (!seen_ids.insert(unique_id).second) {
                    VtxDiagnostic diagnostic = MakeDiagnostic(
                        VtxErrorCode::DuplicateUniqueId, Severity::Error,
                        "duplicate unique_id '" + unique_id + "' in bucket '" + bucket_name + "'", "ValidateFrame");
                    diagnostic.frame_index = frame_index;
                    diagnostic.bucket = bucket_name;
                    diagnostic.unique_id = unique_id;
                    report.Add(std::move(diagnostic));
                }
            }

            for (size_t i = 0; i < bucket.entities.size(); ++i) {
                EntityLocation where;
                where.frame_index = frame_index;
                where.bucket = bucket_name;
                where.unique_id = i < bucket.unique_ids.size() ? bucket.unique_ids[i] : std::string {};
                report.Absorb(ValidateEntity(bucket.entities[i], schema, where));
            }
        }

        return report;
    }

    ValidationReport ValidateFrame(const Frame& frame, const SchemaRegistry& schema, int32_t frame_index) {
        return ValidateFrame(frame, schema.GetPropertyCache(), frame_index);
    }

} // namespace VTX
