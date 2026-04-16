#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

#include "vtx/common/readers/schema_reader/schema_registry.h"

#include "vtx/common/vtx_logger.h"
#include "vtx/common/readers/schema_reader/game_schema_types.h"


using json = nlohmann::json;


static VTX::FieldType StringToTypeEnum(const std::string& type) {
    static const std::unordered_map<std::string, VTX::FieldType> map_type = {
        {"int8", VTX::FieldType::Int8},
        {"Int8", VTX::FieldType::Int8},
        {"uint8", VTX::FieldType::Int8},
        {"UInt8", VTX::FieldType::Int8},
        {"int32", VTX::FieldType::Int32},
        {"Int32", VTX::FieldType::Int32},
        {"uint32", VTX::FieldType::Int32},
        {"UInt32", VTX::FieldType::Int32},
        {"int", VTX::FieldType::Int32},
        {"Int", VTX::FieldType::Int32},
        {"int64", VTX::FieldType::Int64},
        {"Int64", VTX::FieldType::Int64},
        {"uint64", VTX::FieldType::Int64},
        {"UInt64", VTX::FieldType::Int64},
        {"long", VTX::FieldType::Int64},
        {"Long", VTX::FieldType::Int64},
        {"float", VTX::FieldType::Float},
        {"Float", VTX::FieldType::Float},
        {"double", VTX::FieldType::Double},
        {"Double", VTX::FieldType::Double},
        {"bool", VTX::FieldType::Bool},
        {"Bool", VTX::FieldType::Bool},
        {"string", VTX::FieldType::String},
        {"String", VTX::FieldType::String},
        {"vector", VTX::FieldType::Vector},
        {"Vector", VTX::FieldType::Vector},
        {"quat", VTX::FieldType::Quat},
        {"Quat", VTX::FieldType::Quat},
        {"transform", VTX::FieldType::Transform},
        {"Transform", VTX::FieldType::Transform},
        {"struct", VTX::FieldType::Struct},
        {"Struct", VTX::FieldType::Struct}
    };

    auto it = map_type.find(type);
    if (it != map_type.end()) return it->second;

    return VTX::FieldType::None;
};



static VTX::FieldContainerType StringToContainerEnum(const std::string& container) {
    static const std::unordered_map<std::string, VTX::FieldContainerType> map_container = {
        {"array", VTX::FieldContainerType::Array},
        {"map", VTX::FieldContainerType::Map},
    };

    auto it = map_container.find(container);
    if (it != map_container.end()) return it->second;

    return VTX::FieldContainerType::None;
}

VTX::SchemaRegistry::SchemaRegistry()
{
    b_is_valid_ = true;
}

bool VTX::SchemaRegistry::LoadFromJson(const std::string& json_path,ELoadMethod load_method) {
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

bool VTX::SchemaRegistry::LoadFromRawString(const std::string& raw_json)
{
    json_content_ = raw_json;
    json j;
    try {
        j = json::parse(json_content_);
    }
    catch (const json::parse_error& e) {
        VTX_ERROR("Error parsing JSON: {}", e.what());
        b_is_valid_ = false;
        return b_is_valid_;
    }

    if (!j.contains("property_mapping") || !j["property_mapping"].is_array()) {
        VTX_ERROR("JSON does not contain 'property_mapping'.");
        b_is_valid_ = false;
        return b_is_valid_;
    }

    structs_.clear();
    struct_type_ids_.clear();
    current_type_id_ = 0;

    for (const auto& struct_json : j["property_mapping"]) {
        std::string struct_name = struct_json.value("struct", "");
        if (struct_name.empty()) continue;

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

                std::string container = field_json.value("containerType", "None");
                field.container_type = StringToContainerEnum(container);

                field.type_id = VTX::FieldType::None;
                if (field_json.contains("typeId")) {
                    field.type_id = StringToTypeEnum(field_json.value("typeId", "None"));
                }

                field.key_id = VTX::FieldType::None;
                if (field_json.contains("keyId")) {
                    field.key_id = StringToTypeEnum(field_json.value("keyId", "None"));
                }

                field.index = index_counters[{field.type_id, field.container_type}]++;

                current_struct.fields.push_back(field);
            }
        }

        for (const auto& field : current_struct.fields)
        {
            current_struct.field_map[field.name] = &field;

            if (field.container_type == VTX::FieldContainerType::None)
            {
                size_t typeIdx = static_cast<size_t>(field.type_id);

                if (typeIdx >= current_struct.type_max_indices.size()) {
                    current_struct.type_max_indices.resize(typeIdx + 1, 0);
                }

                current_struct.type_max_indices[typeIdx] = std::max(current_struct.type_max_indices[typeIdx], field.index + 1);
            }
        }
    }

    property_cache_.Clear();
    for (const auto& [struct_name, struct_def] : GetDefinitions()) {
        int32_t type_id = GetStructTypeId(struct_name);
        if (type_id == -1) {
            VTX_WARN("Struct '{}' does not have a TypeID registered in the enum.", struct_name);
            continue;
        }

        property_cache_.name_to_id[struct_name] = type_id;

        auto& struct_cache = property_cache_.structs[type_id];
        struct_cache.name = struct_name;

        for (const auto& field : struct_def.fields) {
            if (field.type_id != VTX::FieldType::None) {
                VTX::PropertyAddress addr;
                addr.index = field.index;
                addr.type_id = field.type_id;
                addr.container_type = field.container_type;
                addr.child_type_name = field.struct_type;
                struct_cache.properties[field.name] = addr;
                struct_cache.names_by_lookup_key[VTX::MakePropertyLookupKey(field.index, field.type_id, field.container_type)] = field.name;
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

bool VTX::SchemaRegistry::GetIsValid() const{
    return b_is_valid_;
}

const std::unordered_map<std::string, VTX::SchemaStruct>& VTX::SchemaRegistry::GetDefinitions() const {
    return structs_;
}

const VTX::SchemaField* VTX::SchemaRegistry::GetField(const std::string& struct_name, const std::string& field_name) const {
    const auto* s = GetStruct(struct_name);
    if (!s) return nullptr;

    auto it = s->field_map.find(field_name);
    if (it != s->field_map.end()) {
        return it->second;
    }
    return nullptr;
}

int32_t VTX::SchemaRegistry::GetStructTypeId(const std::string& name) const
{
    auto it = struct_type_ids_.find(name);
    if (it != struct_type_ids_.end()) {
        return it->second;
    }
    return -1;
}
