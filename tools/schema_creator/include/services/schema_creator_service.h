#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace VtxServices {

    struct SchemaFieldMetaDoc {
        std::string type;
        std::string key_type;
        std::string category;
        std::string display_name;
        std::string tooltip;
        std::string default_value;
        int32_t version = 1;
        int32_t fixed_array_dim = 0;
    };

    struct SchemaFieldDoc {
        std::string name;
        std::string struct_type;
        std::string type_id;
        std::string key_id;
        std::string container_type;
        int32_t index = 0;
        SchemaFieldMetaDoc meta;
    };

    struct SchemaStructDoc {
        std::string struct_name;
        std::vector<SchemaFieldDoc> fields;
        std::vector<int32_t> type_max_indices;
    };

    struct SchemaBoneMappingDoc {
        std::string model_name;
        std::vector<std::string> bones;
    };

    struct SchemaDocument {
        std::string version = "1.0.0";
        std::vector<std::string> buckets;
        std::vector<SchemaBoneMappingDoc> bone_mapping;
        std::vector<SchemaStructDoc> structs;
    };

    enum class ValidationSeverity : uint8_t {
        Info = 0,
        Warning,
        Error,
    };

    struct ValidationIssue {
        ValidationSeverity severity = ValidationSeverity::Info;
        std::string message;
        std::string struct_name;
        std::string field_name;
    };

    struct ValidationReport {
        bool is_valid = true;
        std::vector<ValidationIssue> issues;
    };

    enum class EvolutionChangeKind : uint8_t {
        StructAdded = 0,
        StructRemoved,
        FieldAdded,
        FieldRemoved,
        FieldChanged,
        FieldMetaChanged,
    };

    struct EvolutionIssue {
        ValidationSeverity severity = ValidationSeverity::Info;
        EvolutionChangeKind kind = EvolutionChangeKind::FieldChanged;
        std::string message;
        std::string struct_name;
        std::string field_name;
    };

    struct EvolutionReport {
        bool has_breaking_changes = false;
        std::vector<EvolutionIssue> issues;
    };

    class SchemaCreatorService {
    public:
        static SchemaDocument CreateEmptyDocument();

        static bool LoadFromFile(const std::string& path, SchemaDocument& out_document, std::string& out_error);
        static bool SaveToFile(const SchemaDocument& document, const std::string& path, std::string& out_error);

        static bool ParseFromJsonString(const std::string& raw_json, SchemaDocument& out_document,
                                        std::string& out_error);
        static std::string SerializeToJson(const SchemaDocument& document, bool pretty = true);

        static ValidationReport ValidateSchema(const SchemaDocument& document);
        static EvolutionReport BuildEvolutionReport(const SchemaDocument& baseline, const SchemaDocument& candidate);

        static int32_t ComputeNextGenerationVersion(const SchemaDocument& document);
        static void ApplyNextGenerationVersion(SchemaDocument& candidate, const SchemaDocument& baseline);
        static std::string BuildSuggestedNextGenerationPath(const std::string& current_path);
    };

} // namespace VtxServices
