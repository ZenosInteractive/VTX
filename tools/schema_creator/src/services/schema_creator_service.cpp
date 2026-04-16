#include "services/schema_creator_service.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "vtx/common/readers/schema_reader/schema_registry.h"

namespace {

using json = nlohmann::json;

// Returns lowercase copy of input string for case-insensitive comparisons.
std::string ToLowerCopy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Converts type token to canonical schema typeId.
std::string CanonicalizeTypeId(const std::string& type_id) {
    const std::string normalized = ToLowerCopy(type_id);
    if (normalized == "int8" || normalized == "uint8" || normalized == "byte") {
        return "Int8";
    }
    if (normalized == "int32" || normalized == "uint32" || normalized == "int") {
        return "Int32";
    }
    if (normalized == "int64" || normalized == "uint64" || normalized == "long") {
        return "Int64";
    }
    if (normalized == "float") {
        return "Float";
    }
    if (normalized == "double") {
        return "Double";
    }
    if (normalized == "bool" || normalized == "boolean") {
        return "Bool";
    }
    if (normalized == "string" || normalized == "fname" || normalized == "fstring") {
        return "String";
    }
    if (normalized == "vector") {
        return "Vector";
    }
    if (normalized == "quat") {
        return "Quat";
    }
    if (normalized == "transform") {
        return "Transform";
    }
    if (normalized == "range" || normalized == "floatrange") {
        return "FloatRange";
    }
    if (normalized == "struct") {
        return "Struct";
    }
    if (normalized == "enum") {
        return "Enum";
    }
    if (normalized == "none" || normalized.empty()) {
        return "None";
    }
    return type_id;
}

// Normalizes containerType token for comparisons.
std::string NormalizeContainerType(const std::string& container_type) {
    const std::string normalized = ToLowerCopy(container_type);
    if (normalized == "none" || normalized.empty()) {
        return "none";
    }
    if (normalized == "array") {
        return "array";
    }
    if (normalized == "map") {
        return "map";
    }
    return "";
}

// Converts containerType token to canonical schema containerType.
std::string CanonicalizeContainerType(const std::string& container_type) {
    const std::string normalized = NormalizeContainerType(container_type);
    if (normalized == "array") {
        return "Array";
    }
    if (normalized == "map") {
        return "Map";
    }
    return "None";
}

// Suggests lowercase meta.type token when not explicitly provided.
std::string InferMetaTypeFromTypeId(const std::string& type_id) {
    const std::string canonical = CanonicalizeTypeId(type_id);
    if (canonical == "None") {
        return "";
    }
    if (canonical == "FloatRange") {
        return "floatrange";
    }
    return ToLowerCopy(canonical);
}

// Suggests lowercase meta.keyType token when not explicitly provided.
std::string InferMetaKeyTypeFromKeyId(const std::string& key_id) {
    const std::string canonical = CanonicalizeTypeId(key_id);
    if (canonical == "None") {
        return "";
    }
    if (canonical == "FloatRange") {
        return "floatrange";
    }
    return ToLowerCopy(canonical);
}

// Resolves typeId token tuple into SDK field type enum.
VTX::FieldType ResolveFieldTypeFromTypeId(const std::string& raw_type_id, const std::string& struct_type) {
    const std::string type = ToLowerCopy(raw_type_id);
    const std::string nested = ToLowerCopy(struct_type);

    if (type == "struct") {
        if (nested == "vector") {
            return VTX::FieldType::Vector;
        }
        if (nested == "quat") {
            return VTX::FieldType::Quat;
        }
        if (nested == "transform") {
            return VTX::FieldType::Transform;
        }
        if (nested == "floatrange") {
            return VTX::FieldType::FloatRange;
        }
        return VTX::FieldType::Struct;
    }
    if (type == "enum") {
        return VTX::FieldType::Enum;
    }

    if (type == "uint8" || type == "int8" || type == "byte") {
        return VTX::FieldType::Int8;
    }
    if (type == "int32" || type == "int" || type == "uint32") {
        return VTX::FieldType::Int32;
    }
    if (type == "int64" || type == "long" || type == "uint64") {
        return VTX::FieldType::Int64;
    }
    if (type == "float") {
        return VTX::FieldType::Float;
    }
    if (type == "double") {
        return VTX::FieldType::Double;
    }
    if (type == "bool" || type == "boolean") {
        return VTX::FieldType::Bool;
    }
    if (type == "string" || type == "fname" || type == "fstring") {
        return VTX::FieldType::String;
    }
    if (type == "vector") {
        return VTX::FieldType::Vector;
    }
    if (type == "quat") {
        return VTX::FieldType::Quat;
    }
    if (type == "transform") {
        return VTX::FieldType::Transform;
    }
    if (type == "range" || type == "floatrange") {
        return VTX::FieldType::FloatRange;
    }

    return VTX::FieldType::None;
}

// Checks whether raw field type token is recognized by schema validation rules.
bool IsKnownTypeToken(const std::string& raw_type_id) {
    const std::string canonical = CanonicalizeTypeId(raw_type_id);
    if (canonical == "None") {
        return false;
    }
    return ResolveFieldTypeFromTypeId(canonical, "") != VTX::FieldType::None;
}

// Recomputes SDK runtime indices from ordered typeId+containerType groups.
void RecomputeFieldIndices(VtxServices::SchemaStructDoc& schema_struct) {
    std::unordered_map<std::string, int32_t> index_counters;
    for (auto& field : schema_struct.fields) {
        const std::string index_key = CanonicalizeTypeId(field.type_id) + "|" + CanonicalizeContainerType(field.container_type);
        field.index = index_counters[index_key]++;
    }
}

// Returns a normalized copy with canonical tokens and derived indices.
VtxServices::SchemaDocument NormalizeDocumentForSchemaRegistry(const VtxServices::SchemaDocument& document) {
    VtxServices::SchemaDocument normalized = document;
    normalized.version = normalized.version.empty() ? "1.0.0" : normalized.version;
    for (auto& schema_struct : normalized.structs) {
        for (auto& field : schema_struct.fields) {
            field.type_id = CanonicalizeTypeId(field.type_id);
            field.key_id = CanonicalizeTypeId(field.key_id);
            field.container_type = CanonicalizeContainerType(field.container_type);
            if (field.meta.type.empty()) {
                field.meta.type = InferMetaTypeFromTypeId(field.type_id);
            }
            if (field.meta.key_type.empty()) {
                field.meta.key_type = InferMetaKeyTypeFromKeyId(field.key_id);
            }
        }
        RecomputeFieldIndices(schema_struct);
    }
    return normalized;
}

struct FieldSignature {
    std::string type_id;
    std::string struct_type;
    std::string key_id;
    std::string container_type;
    int32_t index = 0;
};

// Builds normalized signature used for compatibility comparisons.
FieldSignature BuildFieldSignature(const VtxServices::SchemaFieldDoc& field) {
    return FieldSignature{
        .type_id = ToLowerCopy(CanonicalizeTypeId(field.type_id)),
        .struct_type = ToLowerCopy(field.struct_type),
        .key_id = ToLowerCopy(CanonicalizeTypeId(field.key_id)),
        .container_type = ToLowerCopy(CanonicalizeContainerType(field.container_type)),
        .index = field.index,
    };
}

// Compares field signatures for compatibility-relevant differences.
bool AreSignaturesEqual(const FieldSignature& lhs, const FieldSignature& rhs) {
    return lhs.type_id == rhs.type_id &&
           lhs.struct_type == rhs.struct_type &&
           lhs.key_id == rhs.key_id &&
           lhs.container_type == rhs.container_type &&
           lhs.index == rhs.index;
}

// Compares field metadata block for non-signature changes.
bool AreMetadataEqual(const VtxServices::SchemaFieldDoc& lhs, const VtxServices::SchemaFieldDoc& rhs) {
    return lhs.meta.type == rhs.meta.type &&
           lhs.meta.key_type == rhs.meta.key_type &&
           lhs.meta.category == rhs.meta.category &&
           lhs.meta.display_name == rhs.meta.display_name &&
           lhs.meta.tooltip == rhs.meta.tooltip &&
           lhs.meta.default_value == rhs.meta.default_value &&
           lhs.meta.version == rhs.meta.version &&
           lhs.meta.fixed_array_dim == rhs.meta.fixed_array_dim;
}

// Finds a field by logical name in a schema struct.
const VtxServices::SchemaFieldDoc* FindFieldByName(const VtxServices::SchemaStructDoc& schema_struct, const std::string& field_name) {
    for (const auto& field : schema_struct.fields) {
        if (field.name == field_name) {
            return &field;
        }
    }
    return nullptr;
}

// Appends a validation issue entry into report.
void AddValidationIssue(
    VtxServices::ValidationReport& report,
    VtxServices::ValidationSeverity severity,
    const std::string& message,
    const std::string& struct_name = {},
    const std::string& field_name = {}) {
    report.issues.push_back(VtxServices::ValidationIssue{
        .severity = severity,
        .message = message,
        .struct_name = struct_name,
        .field_name = field_name,
    });
}

// Appends evolution issue and updates breaking-change flag when needed.
void AddEvolutionIssue(
    VtxServices::EvolutionReport& report,
    VtxServices::ValidationSeverity severity,
    VtxServices::EvolutionChangeKind kind,
    const std::string& message,
    const std::string& struct_name = {},
    const std::string& field_name = {}) {
    report.issues.push_back(VtxServices::EvolutionIssue{
        .severity = severity,
        .kind = kind,
        .message = message,
        .struct_name = struct_name,
        .field_name = field_name,
    });
    if (severity == VtxServices::ValidationSeverity::Error) {
        report.has_breaking_changes = true;
    }
}

// Builds struct-name lookup for diff/validation flows.
std::unordered_map<std::string, const VtxServices::SchemaStructDoc*> BuildStructLookup(const VtxServices::SchemaDocument& document) {
    std::unordered_map<std::string, const VtxServices::SchemaStructDoc*> lookup;
    for (const auto& schema_struct : document.structs) {
        lookup[schema_struct.struct_name] = &schema_struct;
    }
    return lookup;
}

// Builds field-name lookup for a single struct.
std::unordered_map<std::string, const VtxServices::SchemaFieldDoc*> BuildFieldLookup(const VtxServices::SchemaStructDoc& schema_struct) {
    std::unordered_map<std::string, const VtxServices::SchemaFieldDoc*> lookup;
    for (const auto& field : schema_struct.fields) {
        lookup[field.name] = &field;
    }
    return lookup;
}

} // namespace

namespace VtxServices {

// Creates a new empty schema document model.
SchemaDocument SchemaCreatorService::CreateEmptyDocument() {
    return SchemaDocument{};
}

// Loads schema JSON file and parses into in-memory document model.
bool SchemaCreatorService::LoadFromFile(const std::string& path, SchemaDocument& out_document, std::string& out_error) {
    out_error.clear();
    std::ifstream file(path, std::ios::in);
    if (!file.is_open()) {
        out_error = "Unable to open schema file.";
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    return ParseFromJsonString(buffer.str(), out_document, out_error);
}

// Serializes in-memory document and writes it to disk.
bool SchemaCreatorService::SaveToFile(const SchemaDocument& document, const std::string& path, std::string& out_error) {
    out_error.clear();
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        out_error = "Unable to write schema file.";
        return false;
    }

    file << SerializeToJson(document, true);
    return true;
}

// Parses schema JSON string into editable document representation.
bool SchemaCreatorService::ParseFromJsonString(const std::string& raw_json, SchemaDocument& out_document, std::string& out_error) {
    out_error.clear();
    json root;
    try {
        root = json::parse(raw_json);
    } catch (const std::exception& ex) {
        out_error = std::string("JSON parse error: ") + ex.what();
        return false;
    }

    if (!root.contains("property_mapping") || !root["property_mapping"].is_array()) {
        out_error = "Invalid schema JSON: missing 'property_mapping' array.";
        return false;
    }

    // Step 1: Parse top-level new-schema metadata.
    SchemaDocument parsed;
    parsed.version = root.value("version", "1.0.0");
    if (root.contains("buckets") && root["buckets"].is_array()) {
        for (const auto& bucket_json : root["buckets"]) {
            if (bucket_json.is_string()) {
                parsed.buckets.push_back(bucket_json.get<std::string>());
            }
        }
    }

    if (root.contains("bone_mapping") && root["bone_mapping"].is_object()) {
        for (auto it = root["bone_mapping"].begin(); it != root["bone_mapping"].end(); ++it) {
            SchemaBoneMappingDoc mapping;
            mapping.model_name = it.key();
            if (it.value().is_array()) {
                for (const auto& bone_json : it.value()) {
                    if (bone_json.is_string()) {
                        mapping.bones.push_back(bone_json.get<std::string>());
                    }
                }
            }
            parsed.bone_mapping.push_back(std::move(mapping));
        }
    }

    // Step 2: Walk struct entries and map them into document model.
    for (const auto& struct_json : root["property_mapping"]) {
        if (!struct_json.is_object()) {
            continue;
        }

        SchemaStructDoc schema_struct;
        schema_struct.struct_name = struct_json.value("struct", "");

        // Step 3: Parse field list and metadata payload.
        if (struct_json.contains("values") && struct_json["values"].is_array()) {
            for (const auto& field_json : struct_json["values"]) {
                if (!field_json.is_object()) {
                    continue;
                }

                SchemaFieldDoc field;
                field.name = field_json.value("name", "");
                field.struct_type = field_json.value("structType", "");
                field.type_id = field_json.value("typeId", "");
                field.key_id = field_json.value("keyId", "None");
                field.container_type = field_json.value("containerType", "None");

                if (field_json.contains("meta") && field_json["meta"].is_object()) {
                    const auto& meta_json = field_json["meta"];
                    field.meta.type = meta_json.value("type", "");
                    field.meta.key_type = meta_json.value("keyType", "");
                    field.meta.category = meta_json.value("category", "");
                    field.meta.display_name = meta_json.value("displayName", "");
                    field.meta.tooltip = meta_json.value("tooltip", "");
                    field.meta.default_value = meta_json.value("defaultValue", "");
                    field.meta.version = meta_json.value("version", 1);
                    field.meta.fixed_array_dim = meta_json.value("fixedArrayDim", 0);
                }

                schema_struct.fields.push_back(std::move(field));
            }
        }

        RecomputeFieldIndices(schema_struct);
        parsed.structs.push_back(std::move(schema_struct));
    }

    out_document = NormalizeDocumentForSchemaRegistry(parsed);
    return true;
}

// Serializes editable document model to canonical schema JSON payload.
std::string SchemaCreatorService::SerializeToJson(const SchemaDocument& document, bool pretty) {
    const SchemaDocument normalized = NormalizeDocumentForSchemaRegistry(document);

    json root;
    root["version"] = normalized.version.empty() ? "1.0.0" : normalized.version;
    root["buckets"] = json::array();
    for (const auto& bucket_name : normalized.buckets) {
        root["buckets"].push_back(bucket_name);
    }

    root["bone_mapping"] = json::object();
    for (const auto& mapping : normalized.bone_mapping) {
        root["bone_mapping"][mapping.model_name] = mapping.bones;
    }

    root["property_mapping"] = json::array();
    for (const auto& schema_struct : normalized.structs) {
        json struct_json;
        struct_json["struct"] = schema_struct.struct_name;
        struct_json["values"] = json::array();

        for (const auto& field : schema_struct.fields) {
            json field_json;
            field_json["name"] = field.name;
            field_json["structType"] = field.struct_type;
            field_json["typeId"] = CanonicalizeTypeId(field.type_id);
            field_json["keyId"] = CanonicalizeTypeId(field.key_id);
            field_json["containerType"] = CanonicalizeContainerType(field.container_type);

            json meta_json;
            meta_json["type"] = field.meta.type.empty() ? InferMetaTypeFromTypeId(field.type_id) : field.meta.type;
            meta_json["keyType"] = field.meta.key_type.empty() ? InferMetaKeyTypeFromKeyId(field.key_id) : field.meta.key_type;
            meta_json["category"] = field.meta.category;
            meta_json["displayName"] = field.meta.display_name;
            meta_json["tooltip"] = field.meta.tooltip;
            meta_json["defaultValue"] = field.meta.default_value;
            meta_json["version"] = field.meta.version;
            meta_json["fixedArrayDim"] = field.meta.fixed_array_dim;
            field_json["meta"] = std::move(meta_json);

            struct_json["values"].push_back(std::move(field_json));
        }

        root["property_mapping"].push_back(std::move(struct_json));
    }

    return pretty ? root.dump(4) : root.dump();
}

// Validates schema document for structural and compatibility issues.
ValidationReport SchemaCreatorService::ValidateSchema(const SchemaDocument& document) {
    ValidationReport report;
    const SchemaDocument normalized = NormalizeDocumentForSchemaRegistry(document);

    if (normalized.structs.empty()) {
        AddValidationIssue(report, ValidationSeverity::Warning, "Schema is empty. Add at least one struct.");
    }

    // Step 1: Validate struct/field names and local field constraints.
    std::unordered_set<std::string> struct_names;
    for (const auto& schema_struct : normalized.structs) {
        if (schema_struct.struct_name.empty()) {
            AddValidationIssue(report, ValidationSeverity::Error, "Struct name is empty.");
            continue;
        }
        if (!struct_names.insert(schema_struct.struct_name).second) {
            AddValidationIssue(
                report,
                ValidationSeverity::Error,
                "Duplicate struct name: '" + schema_struct.struct_name + "'.",
                schema_struct.struct_name);
        }

        // Track duplicates by logical field name within each struct scope.
        std::unordered_set<std::string> field_names;
        for (const auto& field : schema_struct.fields) {
            if (field.name.empty()) {
                AddValidationIssue(
                    report,
                    ValidationSeverity::Error,
                    "Field name is empty.",
                    schema_struct.struct_name);
                continue;
            }
            if (!field_names.insert(field.name).second) {
                AddValidationIssue(
                    report,
                    ValidationSeverity::Error,
                    "Duplicate field name: '" + field.name + "'.",
                    schema_struct.struct_name,
                    field.name);
            }

            const std::string container = NormalizeContainerType(field.container_type);
            if (container.empty()) {
                AddValidationIssue(
                    report,
                    ValidationSeverity::Warning,
                    "containerType should be one of: 'None', 'Array', 'Map'.",
                    schema_struct.struct_name,
                    field.name);
            }

            if (CanonicalizeTypeId(field.type_id) == "Struct" &&
                field.struct_type.empty()) {
                AddValidationIssue(
                    report,
                    ValidationSeverity::Error,
                    "Field with typeId 'Struct' requires 'structType'.",
                    schema_struct.struct_name,
                    field.name);
            }

            if (container == "map" && CanonicalizeTypeId(field.key_id) == "None") {
                AddValidationIssue(
                    report,
                    ValidationSeverity::Warning,
                    "Map field should define 'keyId'.",
                    schema_struct.struct_name,
                    field.name);
            }

            if (CanonicalizeTypeId(field.type_id) == "None") {
                AddValidationIssue(
                    report,
                    ValidationSeverity::Error,
                    "Field typeId cannot be empty/None.",
                    schema_struct.struct_name,
                    field.name);
            } else if (!IsKnownTypeToken(field.type_id)) {
                AddValidationIssue(
                    report,
                    ValidationSeverity::Warning,
                    "Unknown field typeId token '" + field.type_id + "'.",
                    schema_struct.struct_name,
                    field.name);
            }
        }
    }

    // Step 2: Ensure SDK parser accepts produced schema JSON.
    VTX::SchemaRegistry registry;
    if (!registry.LoadFromRawString(SerializeToJson(normalized, true))) {
        AddValidationIssue(
            report,
            ValidationSeverity::Error,
            "SDK SchemaRegistry failed to parse the current schema JSON.");
    }

    // Step 3: Compute report validity from issue severities.
    report.is_valid = true;
    for (const auto& issue : report.issues) {
        if (issue.severity == ValidationSeverity::Error) {
            report.is_valid = false;
            break;
        }
    }
    return report;
}

// Compares baseline vs candidate and reports additive vs breaking evolution deltas.
EvolutionReport SchemaCreatorService::BuildEvolutionReport(const SchemaDocument& baseline, const SchemaDocument& candidate) {
    EvolutionReport report;
    const SchemaDocument normalized_baseline = NormalizeDocumentForSchemaRegistry(baseline);
    const SchemaDocument normalized_candidate = NormalizeDocumentForSchemaRegistry(candidate);

    const auto baseline_structs = BuildStructLookup(normalized_baseline);
    const auto candidate_structs = BuildStructLookup(normalized_candidate);

    // Pass 1: additive structs.
    for (const auto& [struct_name, candidate_struct] : candidate_structs) {
        (void)candidate_struct;
        if (baseline_structs.find(struct_name) == baseline_structs.end()) {
            AddEvolutionIssue(
                report,
                ValidationSeverity::Info,
                EvolutionChangeKind::StructAdded,
                "Struct added.",
                struct_name);
        }
    }

    // Pass 2: breaking/removal/signature changes against baseline.
    for (const auto& [struct_name, baseline_struct] : baseline_structs) {
        if (candidate_structs.find(struct_name) == candidate_structs.end()) {
            AddEvolutionIssue(
                report,
                ValidationSeverity::Error,
                EvolutionChangeKind::StructRemoved,
                "Struct removed. Breaking change.",
                struct_name);
            continue;
        }

        const auto* candidate_struct = candidate_structs.at(struct_name);
        const auto baseline_fields = BuildFieldLookup(*baseline_struct);
        const auto candidate_fields = BuildFieldLookup(*candidate_struct);

        for (const auto& [field_name, candidate_field] : candidate_fields) {
            if (baseline_fields.find(field_name) == baseline_fields.end()) {
                AddEvolutionIssue(
                    report,
                    ValidationSeverity::Info,
                    EvolutionChangeKind::FieldAdded,
                    "Field added (compatible additive change).",
                    struct_name,
                    field_name);
                continue;
            }

            const auto* baseline_field = baseline_fields.at(field_name);
            if (!AreSignaturesEqual(BuildFieldSignature(*baseline_field), BuildFieldSignature(*candidate_field))) {
                AddEvolutionIssue(
                    report,
                    ValidationSeverity::Error,
                    EvolutionChangeKind::FieldChanged,
                    "Field signature changed (typeId/keyId/containerType/index/structType). Breaking change.",
                    struct_name,
                    field_name);
                continue;
            }

            if (!AreMetadataEqual(*baseline_field, *candidate_field)) {
                AddEvolutionIssue(
                    report,
                    ValidationSeverity::Warning,
                    EvolutionChangeKind::FieldMetaChanged,
                    "Field metadata changed (non-breaking, review downstream usage).",
                    struct_name,
                    field_name);
            }
        }

        for (const auto& [field_name, baseline_field] : baseline_fields) {
            (void)baseline_field;
            if (candidate_fields.find(field_name) == candidate_fields.end()) {
                AddEvolutionIssue(
                    report,
                    ValidationSeverity::Error,
                    EvolutionChangeKind::FieldRemoved,
                    "Field removed. Breaking change.",
                    struct_name,
                    field_name);
            }
        }
    }

    return report;
}

// Computes highest version marker across all fields in document.
int32_t SchemaCreatorService::ComputeNextGenerationVersion(const SchemaDocument& document) {
    int32_t max_version = 1;
    for (const auto& schema_struct : document.structs) {
        for (const auto& field : schema_struct.fields) {
            max_version = std::max(max_version, field.meta.version);
        }
    }
    return max_version;
}

// Applies next-generation version stamping to changed/new fields.
void SchemaCreatorService::ApplyNextGenerationVersion(SchemaDocument& candidate, const SchemaDocument& baseline) {
    const SchemaDocument normalized_baseline = NormalizeDocumentForSchemaRegistry(baseline);
    const SchemaDocument normalized_candidate = NormalizeDocumentForSchemaRegistry(candidate);

    const int32_t next_version = std::max(
        ComputeNextGenerationVersion(normalized_baseline),
        ComputeNextGenerationVersion(normalized_candidate)) + 1;
    const auto baseline_structs = BuildStructLookup(normalized_baseline);
    const auto normalized_candidate_structs = BuildStructLookup(normalized_candidate);

    // Step 1: Walk candidate structs and align against baseline counterpart.
    for (auto& candidate_struct : candidate.structs) {
        const auto baseline_struct_it = baseline_structs.find(candidate_struct.struct_name);
        if (baseline_struct_it == baseline_structs.end()) {
            // New struct in next generation: stamp all fields with the new version.
            for (auto& field : candidate_struct.fields) {
                field.meta.version = next_version;
            }
            continue;
        }

        const auto normalized_candidate_struct_it = normalized_candidate_structs.find(candidate_struct.struct_name);
        if (normalized_candidate_struct_it == normalized_candidate_structs.end()) {
            continue;
        }

        const auto* baseline_struct = baseline_struct_it->second;
        const auto* normalized_candidate_struct = normalized_candidate_struct_it->second;

        // Step 2: Bump version only for added/changed fields.
        for (auto& field : candidate_struct.fields) {
            const auto* baseline_field = FindFieldByName(*baseline_struct, field.name);
            const auto* normalized_candidate_field = FindFieldByName(*normalized_candidate_struct, field.name);
            const bool should_bump = (baseline_field == nullptr) ||
                (normalized_candidate_field == nullptr) ||
                !AreSignaturesEqual(BuildFieldSignature(*baseline_field), BuildFieldSignature(*normalized_candidate_field)) ||
                !AreMetadataEqual(*baseline_field, *normalized_candidate_field);

            if (should_bump) {
                field.meta.version = next_version;
            } else if (field.meta.version <= 0) {
                // Preserve existing stable version for untouched fields.
                field.meta.version = std::max<int32_t>(1, baseline_field->meta.version);
            }
        }
    }
}

// Suggests a save path with incremented version suffix for next-generation saves.
std::string SchemaCreatorService::BuildSuggestedNextGenerationPath(const std::string& current_path) {
    if (current_path.empty()) {
        return "schema_v2.json";
    }

    std::filesystem::path source_path(current_path);
    std::string stem = source_path.stem().string();
    std::string extension = source_path.extension().string();
    if (extension.empty()) {
        extension = ".json";
    }

    const std::regex version_suffix("(.*)_v([0-9]+)$");
    std::smatch match;
    if (std::regex_match(stem, match, version_suffix) && match.size() == 3) {
        const int version = std::max(1, std::stoi(match[2].str()));
        stem = match[1].str() + "_v" + std::to_string(version + 1);
    } else {
        stem += "_v2";
    }

    const std::filesystem::path suggested = source_path.parent_path() / (stem + extension);
    return suggested.string();
}

} // namespace VtxServices
