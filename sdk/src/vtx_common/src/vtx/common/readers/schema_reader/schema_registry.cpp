#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

#include "vtx/common/readers/schema_reader/schema_registry.h"

#include "vtx/common/vtx_logger.h"
#include "vtx/common/readers/schema_reader/game_schema_types.h"
#include "vtx/common/readers/schema_reader/schema_enums.h"
#include "vtx/common/readers/schema_reader/schema_validator.h"


using json = nlohmann::json;


VTX::SchemaValidationResult VTX::SchemaRegistry::ValidateSchema(const std::string& raw_json) {
    return VTX::SchemaValidator {}.Validate(raw_json);
}

VTX::SchemaRegistry::SchemaRegistry() {
    b_is_valid_ = true;
}

bool VTX::SchemaRegistry::LoadFromJson(const std::string& json_path, ELoadMethod load_method) {
    std::ifstream file(json_path);
    if (!file.is_open()) {
        VTX_ERROR("Cannot open JSON: {}", json_path);
        b_is_valid_ = false;
        return b_is_valid_;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    return LoadFromRawString(buffer.str());
}

bool VTX::SchemaRegistry::LoadFromRawString(const std::string& raw_json) {
    json_content_ = raw_json;
    json j;
    try {
        j = json::parse(json_content_);
    } catch (const json::parse_error& e) {
        VTX_ERROR("Error parsing JSON: {}", e.what());
        b_is_valid_ = false;
        return b_is_valid_;
    }

    if (!j.contains("property_mapping") || !j["property_mapping"].is_array()) {
        VTX_ERROR("JSON does not contain 'property_mapping'.");
        b_is_valid_ = false;
        return b_is_valid_;
    }

    // Reject malformed schemas up front -- before resolving anything into
    // indices and long before a frame is built. Unknown type/container values,
    // duplicate names, unresolved struct references, bad defaults, etc. are
    // fatal; warnings are surfaced but do not block the load.
    last_validation_ = VTX::SchemaValidator {}.Validate(json_content_);
    if (last_validation_.HasErrors()) {
        VTX_ERROR("Schema validation failed ({} error(s), {} warning(s)):\n{}", last_validation_.ErrorCount(),
                  last_validation_.WarningCount(), last_validation_.ToString());
        b_is_valid_ = false;
        return b_is_valid_;
    }
    if (last_validation_.WarningCount() > 0) {
        VTX_WARN("Schema validation warnings:\n{}", last_validation_.ToString());
    }

    structs_.clear();
    struct_type_ids_.clear();
    bucket_names_.clear();
    current_type_id_ = 0;

    if (j.contains("buckets") && j["buckets"].is_array()) {
        for (const auto& bucket_json : j["buckets"]) {
            if (bucket_json.is_string()) {
                bucket_names_.push_back(bucket_json.get<std::string>());
            }
        }
    }

    for (const auto& struct_json : j["property_mapping"]) {
        std::string struct_name = struct_json.value("struct", "");
        if (struct_name.empty())
            continue;

        struct_type_ids_[struct_name] = current_type_id_;
        current_type_id_++;

        VTX::SchemaStruct& current_struct = structs_[struct_name];
        current_struct.struct_name = struct_name;
        current_struct.type_max_indices.clear();

        std::map<std::pair<VTX::FieldType, VTX::FieldContainerType>, int32_t> index_counters;

        if (struct_json.contains("values") && struct_json["values"].is_array()) {
            for (const auto& field_json : struct_json["values"]) {
                VTX::SchemaField field;

                if (field_json.contains("meta")) {
                    const auto& meta_json = field_json["meta"];
                    field.meta.category = meta_json.value("category", "");

                    if (meta_json.contains("type")) {
                        if (meta_json["type"].is_string()) {
                            field.meta.type = meta_json["type"].get<std::string>();
                        } else {
                            field.meta.type = meta_json["type"].dump();
                        }
                    }

                    field.meta.display_name = meta_json.value("displayName", "");
                    field.meta.tooltip = meta_json.value("tooltip", "");
                    field.meta.default_value = meta_json.value("defaultValue", "");
                    field.meta.version = meta_json.value("version", 1);
                    field.meta.fixed_array_dim = meta_json.value("fixedArrayDim", 0);
                }

                field.name = field_json.value("name", "");
                field.struct_type = field_json.value("structType", "");

                // Validation already guaranteed these resolve; value_or keeps the
                // historical "unknown -> None" fallback as a defensive default.
                const std::string container = field_json.value("containerType", "None");
                field.container_type = VTX::ParseContainerType(container).value_or(VTX::FieldContainerType::None);

                field.type_id = VTX::ParseFieldType(field_json.value("typeId", "None")).value_or(VTX::FieldType::None);
                field.key_id = VTX::ParseFieldType(field_json.value("keyId", "None")).value_or(VTX::FieldType::None);

                field.index = index_counters[{field.type_id, field.container_type}]++;

                current_struct.fields.push_back(field);
            }
        }

        for (const auto& field : current_struct.fields) {
            current_struct.field_map[field.name] = &field;

            if (field.container_type == VTX::FieldContainerType::None) {
                size_t typeIdx = static_cast<size_t>(field.type_id);

                if (typeIdx >= current_struct.type_max_indices.size()) {
                    current_struct.type_max_indices.resize(typeIdx + 1, 0);
                }

                current_struct.type_max_indices[typeIdx] =
                    std::max(current_struct.type_max_indices[typeIdx], field.index + 1);
            }
        }
    }

    property_cache_.Clear();
    property_cache_.bucket_names = bucket_names_;
    for (const auto& [struct_name, struct_def] : GetDefinitions()) {
        int32_t type_id = GetStructTypeId(struct_name);
        if (type_id == -1) {
            VTX_WARN("Struct '{}' does not have a TypeID registered in the enum.", struct_name);
            continue;
        }

        property_cache_.name_to_id[struct_name] = type_id;

        auto& struct_cache = property_cache_.structs[type_id];
        struct_cache.name = struct_name;
        struct_cache.type_max_indices = struct_def.type_max_indices;

        for (const auto& field : struct_def.fields) {
            if (field.type_id != VTX::FieldType::None) {
                VTX::PropertyAddress addr;
                addr.index = field.index;
                addr.type_id = field.type_id;
                addr.container_type = field.container_type;
                addr.child_type_name = field.struct_type;
                struct_cache.properties[field.name] = addr;
                struct_cache
                    .names_by_lookup_key[VTX::MakePropertyLookupKey(field.index, field.type_id, field.container_type)] =
                    field.name;
                struct_cache.property_order.push_back(field.name);
            }
        }
    }

    b_is_valid_ = true;
    return b_is_valid_;
}

int32_t VTX::SchemaRegistry::GetIndex(const std::string& structName, const std::string& fieldName) const {
    const VTX::SchemaStruct* struct_info = GetStruct(structName);
    if (!struct_info) {
        VTX_WARN("Struct '{}' not found", structName);
        return -1;
    }

    auto field_it = struct_info->field_map.find(fieldName);
    if (field_it == struct_info->field_map.end()) {
        VTX_WARN("Field '{}' not found in '{}'", fieldName, structName);
        return -1;
    }

    return field_it->second->index;
}

const VTX::SchemaStruct* VTX::SchemaRegistry::GetStruct(const std::string& name) const {
    auto it = structs_.find(name);
    if (it != structs_.end()) {
        return &(it->second);
    }
    return nullptr;
}

bool VTX::SchemaRegistry::GetIsValid() const {
    return b_is_valid_;
}

const std::unordered_map<std::string, VTX::SchemaStruct>& VTX::SchemaRegistry::GetDefinitions() const {
    return structs_;
}

const VTX::SchemaField* VTX::SchemaRegistry::GetField(const std::string& struct_name,
                                                      const std::string& field_name) const {
    const auto* s = GetStruct(struct_name);
    if (!s)
        return nullptr;

    auto it = s->field_map.find(field_name);
    if (it != s->field_map.end()) {
        return it->second;
    }
    return nullptr;
}

int32_t VTX::SchemaRegistry::GetStructTypeId(const std::string& name) const {
    auto it = struct_type_ids_.find(name);
    if (it != struct_type_ids_.end()) {
        return it->second;
    }
    return -1;
}
