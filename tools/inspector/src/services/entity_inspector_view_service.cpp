#include "services/entity_inspector_view_service.h"

#include <cstdio>
#include <limits>

namespace VtxServices {

namespace {

using Node = EntityPropertyNode;

void BuildPropertyNodesRecursive(
    const VTX::PropertyContainer& pc,
    const VTX::PropertyAddressCache* global_cache,
    const std::string& id_prefix,
    std::vector<Node>& out_nodes);

Node MakeGroupNode(const std::string& id, const std::string& label, bool open_by_default = true) {
    Node node;
    node.id = id;
    node.label = label;
    node.open_by_default = open_by_default;
    return node;
}

Node MakePropertyNode(
    const std::string& id,
    const std::string& label,
    const std::string& value,
    const std::string& raw_value,
    const std::string& struct_name,
    int schema_index,
    VTX::FieldType schema_type,
    VTX::FieldContainerType schema_container_type) {
    Node node;
    node.id = id;
    node.label = label;
    node.value = value;
    node.raw_value = raw_value;
    node.struct_name = struct_name;
    node.schema_index = schema_index;
    node.schema_type = schema_type;
    node.schema_container_type = schema_container_type;
    node.is_property = true;
    return node;
}

void AttachSchemaLookup(
    Node& node,
    const std::string& struct_name,
    int schema_index,
    VTX::FieldType schema_type,
    VTX::FieldContainerType schema_container_type,
    bool schema_focus_on_secondary_click = false) {
    node.struct_name = struct_name;
    node.schema_index = schema_index;
    node.schema_type = schema_type;
    node.schema_container_type = schema_container_type;
    node.schema_focus_on_secondary_click = schema_focus_on_secondary_click;
}

const VTX::PropertyAddress* FindSchemaAddress(
    const VTX::StructSchemaCache* struct_cache,
    VTX::FieldType expected_type,
    VTX::FieldContainerType container_type,
    size_t index) {
    if (!struct_cache) {
        return nullptr;
    }

    const auto exact_it = struct_cache->names_by_lookup_key.find(
        VTX::MakePropertyLookupKey(static_cast<int32_t>(index), expected_type, container_type));
    if (exact_it != struct_cache->names_by_lookup_key.end()) {
        const auto property_it = struct_cache->properties.find(exact_it->second);
        if (property_it != struct_cache->properties.end()) {
            return &property_it->second;
        }
    }

    if (expected_type == VTX::FieldType::Int32) {
        const auto enum_it = struct_cache->names_by_lookup_key.find(
            VTX::MakePropertyLookupKey(static_cast<int32_t>(index), VTX::FieldType::Enum, container_type));
        if (enum_it != struct_cache->names_by_lookup_key.end()) {
            const auto property_it = struct_cache->properties.find(enum_it->second);
            if (property_it != struct_cache->properties.end()) {
                return &property_it->second;
            }
        }

        const auto int8_it = struct_cache->names_by_lookup_key.find(
            VTX::MakePropertyLookupKey(static_cast<int32_t>(index), VTX::FieldType::Int8, container_type));
        if (int8_it != struct_cache->names_by_lookup_key.end()) {
            const auto property_it = struct_cache->properties.find(int8_it->second);
            if (property_it != struct_cache->properties.end()) {
                return &property_it->second;
            }
        }
    }

    return nullptr;
}

void AttachSchemaPropertyLabel(
    Node& node,
    const std::string& parent_struct_name,
    const std::string& property_name,
    size_t index) {
    if (property_name.empty()) {
        return;
    }

    node.schema_label = property_name + " [" + std::to_string(index) + "]";
    node.schema_focus_struct_name = parent_struct_name;
    node.schema_focus_property_name = property_name;
    node.schema_focus_on_secondary_click = true;
}

void AttachSchemaStructLabel(
    Node& node,
    const std::string& child_struct_name,
    const std::string& label) {
    if (child_struct_name.empty()) {
        return;
    }

    node.schema_label = label;
    node.schema_focus_struct_name = child_struct_name;
    node.schema_focus_on_secondary_click = true;
}

// Checks whether a schema-addressed field currently has any stored value in the container.
bool HasFieldValue(const VTX::PropertyContainer& pc, const VTX::PropertyAddress& address) {
    const auto has_index = [index = address.index](size_t size) {
        return index >= 0 && static_cast<size_t>(index) < size;
    };
    const auto has_subarray_slot = [index = address.index](const auto& flat_array) {
        return index >= 0 &&
            static_cast<size_t>(index) < flat_array.SubArrayCount();
    };

    if (address.container_type == VTX::FieldContainerType::Array) {
        switch (address.type_id) {
            case VTX::FieldType::Int8:       return has_subarray_slot(pc.byte_array_properties) || has_subarray_slot(pc.int32_arrays);
            case VTX::FieldType::Int32:
            case VTX::FieldType::Enum:       return has_subarray_slot(pc.int32_arrays);
            case VTX::FieldType::Int64:      return has_subarray_slot(pc.int64_arrays);
            case VTX::FieldType::Float:      return has_subarray_slot(pc.float_arrays);
            case VTX::FieldType::Double:     return has_subarray_slot(pc.double_arrays);
            case VTX::FieldType::Bool:       return has_subarray_slot(pc.bool_arrays);
            case VTX::FieldType::String:     return has_subarray_slot(pc.string_arrays);
            case VTX::FieldType::Vector:     return has_subarray_slot(pc.vector_arrays);
            case VTX::FieldType::Quat:       return has_subarray_slot(pc.quat_arrays);
            case VTX::FieldType::Transform:  return has_subarray_slot(pc.transform_arrays);
            case VTX::FieldType::FloatRange: return has_subarray_slot(pc.range_arrays);
            case VTX::FieldType::Struct:     return has_subarray_slot(pc.any_struct_arrays);
            case VTX::FieldType::None:
            default:                         return false;
        }
    }

    if (address.container_type == VTX::FieldContainerType::Map) {
        if (address.type_id != VTX::FieldType::Struct) {
            return false;
        }
        return has_index(pc.map_properties.size());
    }

    switch (address.type_id) {
        case VTX::FieldType::Int8:
        case VTX::FieldType::Int32:
        case VTX::FieldType::Enum:       return has_index(pc.int32_properties.size());
        case VTX::FieldType::Int64:      return has_index(pc.int64_properties.size());
        case VTX::FieldType::Float:      return has_index(pc.float_properties.size());
        case VTX::FieldType::Double:     return has_index(pc.double_properties.size());
        case VTX::FieldType::Bool:       return has_index(pc.bool_properties.size());
        case VTX::FieldType::String:     return has_index(pc.string_properties.size());
        case VTX::FieldType::Vector:     return has_index(pc.vector_properties.size());
        case VTX::FieldType::Quat:       return has_index(pc.quat_properties.size());
        case VTX::FieldType::Transform:  return has_index(pc.transform_properties.size());
        case VTX::FieldType::FloatRange: return has_index(pc.range_properties.size());
        case VTX::FieldType::Struct:
            return has_index(pc.any_struct_properties.size()) &&
                pc.any_struct_properties[static_cast<size_t>(address.index)].entity_type_id != -1;
        case VTX::FieldType::None:
        default:
            return false;
    }
}

// Produces schema field names that currently have no value stored in the selected entity.
std::vector<std::string> BuildMissingFieldNames(
    const VTX::PropertyContainer& pc,
    const VTX::StructSchemaCache* struct_cache) {
    std::vector<std::string> missing_fields;
    if (!struct_cache) {
        return missing_fields;
    }

    const auto ordered_properties = struct_cache->GetPropertiesInOrder();
    missing_fields.reserve(ordered_properties.size());
    for (const auto& ordered_property : ordered_properties) {
        if (!ordered_property.address) {
            continue;
        }
        if (!HasFieldValue(pc, *ordered_property.address)) {
            missing_fields.emplace_back(ordered_property.name);
        }
    }
    return missing_fields;
}

template <typename T, typename DisplayFormatFn, typename RawFormatFn>
void AppendPrimitiveNodes(
    std::vector<Node>& parent_children,
    const std::string& id_prefix,
    const char* group_title,
    const std::vector<T>& values,
    VTX::FieldType type_id,
    const VTX::StructSchemaCache* struct_cache,
    const std::string& struct_name,
    const char* label_prefix,
    DisplayFormatFn&& display_format_fn,
    RawFormatFn&& raw_format_fn) {
    if (values.empty()) {
        return;
    }

    Node group = MakeGroupNode(
        id_prefix + "/" + group_title,
        std::string(group_title) + " (" + std::to_string(values.size()) + ")");

    group.children.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        char label[64];
        std::snprintf(label, sizeof(label), "%s [%zu]", label_prefix, i);
        group.children.push_back(MakePropertyNode(
            group.id + "/" + std::to_string(i),
            label,
            display_format_fn(values[i]),
            raw_format_fn(values[i]),
            struct_name,
            static_cast<int>(i),
            type_id,
            VTX::FieldContainerType::None));
    }

    parent_children.push_back(std::move(group));
}

template <typename FlatArrayT, typename DisplayFormatFn, typename RawFormatFn>
void AppendFlatArrayGroup(
    std::vector<Node>& parent_children,
    const std::string& id_prefix,
    const char* group_title,
    const FlatArrayT& flat_array,
    VTX::FieldType type_id,
    const VTX::StructSchemaCache* struct_cache,
    const std::string& struct_name,
    DisplayFormatFn&& display_format_fn,
    RawFormatFn&& raw_format_fn) {
    if (flat_array.SubArrayCount() == 0) {
        return;
    }

    Node arrays_group = MakeGroupNode(
        id_prefix + "/" + group_title,
        std::string(group_title) + " (" + std::to_string(flat_array.SubArrayCount()) + " arrays)");

    arrays_group.children.reserve(flat_array.SubArrayCount());
    for (size_t i = 0; i < flat_array.SubArrayCount(); ++i) {
        const auto sub_array = flat_array.GetSubArray(i);
        Node array_node = MakeGroupNode(
            arrays_group.id + "/" + std::to_string(i),
            "Array [" + std::to_string(i) + "] (" + std::to_string(sub_array.size()) + " items)");
        const std::string array_schema_name = EntityInspectorViewService::ResolveSchemaName(
            struct_cache,
            type_id,
            VTX::FieldContainerType::Array,
            i);
        AttachSchemaLookup(
            array_node,
            struct_name,
            static_cast<int>(i),
            type_id,
            VTX::FieldContainerType::Array,
            true);
        AttachSchemaPropertyLabel(array_node, struct_name, array_schema_name, i);

        for (size_t j = 0; j < sub_array.size(); ++j) {
            char label[64];
            std::snprintf(label, sizeof(label), "Item [%zu]", j);
            Node item_node = MakePropertyNode(
                array_node.id + "/" + std::to_string(j),
                label,
                display_format_fn(sub_array[j]),
                raw_format_fn(sub_array[j]),
                struct_name,
                static_cast<int>(i),
                type_id,
                VTX::FieldContainerType::Array);
            if (!array_schema_name.empty()) {
                item_node.schema_label = std::string(label);
            }
            array_node.children.push_back(std::move(item_node));
        }
        if (sub_array.empty()) {
            array_node.children.push_back(MakeGroupNode(array_node.id + "/empty", "Empty Array"));
        }

        arrays_group.children.push_back(std::move(array_node));
    }

    parent_children.push_back(std::move(arrays_group));
}

void AppendMapEntries(
    Node& parent_node,
    const VTX::MapContainer& map_cont,
    const VTX::PropertyAddressCache* global_cache) {
    parent_node.children.reserve(map_cont.keys.size());
    for (size_t j = 0; j < map_cont.keys.size(); ++j) {
        Node key_node = MakeGroupNode(
            parent_node.id + "/" + std::to_string(j),
            "Key: " + map_cont.keys[j]);
        if (j < map_cont.values.size()) {
            const auto& map_value = map_cont.values[j];
            const std::string nested_struct_name = EntityInspectorViewService::ResolveStructName(
                EntityInspectorViewService::ResolveStructCache(global_cache, map_value.entity_type_id));
            if (!nested_struct_name.empty()) {
                AttachSchemaStructLabel(
                    key_node,
                    nested_struct_name,
                    nested_struct_name + " [" + map_cont.keys[j] + "]");
            }
            BuildPropertyNodesRecursive(map_cont.values[j], global_cache, key_node.id, key_node.children);
        }
        parent_node.children.push_back(std::move(key_node));
    }
}

void BuildPropertyNodesRecursive(
    const VTX::PropertyContainer& pc,
    const VTX::PropertyAddressCache* global_cache,
    const std::string& id_prefix,
    std::vector<Node>& out_nodes) {
    const VTX::StructSchemaCache* struct_cache =
        EntityInspectorViewService::ResolveStructCache(global_cache, pc.entity_type_id);
    const std::string struct_name = EntityInspectorViewService::ResolveStructName(struct_cache);

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Booleans",
        pc.bool_properties,
        VTX::FieldType::Bool,
        struct_cache,
        struct_name,
        "Bool",
        [](bool value) { return value ? "True" : "False"; },
        [](bool value) { return value ? "True" : "False"; });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Int32",
        pc.int32_properties,
        VTX::FieldType::Int32,
        struct_cache,
        struct_name,
        "Int32",
        [](int32_t value) { return std::to_string(value); },
        [](int32_t value) { return std::to_string(value); });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Int64",
        pc.int64_properties,
        VTX::FieldType::Int64,
        struct_cache,
        struct_name,
        "Int64",
        [](int64_t value) { return std::to_string(value); },
        [](int64_t value) { return std::to_string(value); });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Floats",
        pc.float_properties,
        VTX::FieldType::Float,
        struct_cache,
        struct_name,
        "Float",
        [](float value) { return EntityInspectorViewService::FormatFloat(value); },
        [](float value) { return EntityInspectorViewService::FormatRawFloat(value); });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Doubles",
        pc.double_properties,
        VTX::FieldType::Double,
        struct_cache,
        struct_name,
        "Double",
        [](double value) { return EntityInspectorViewService::FormatDouble(value); },
        [](double value) { return EntityInspectorViewService::FormatRawDouble(value); });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Strings",
        pc.string_properties,
        VTX::FieldType::String,
        struct_cache,
        struct_name,
        "String",
        [](const std::string& value) { return value; },
        [](const std::string& value) { return value; });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Vectors",
        pc.vector_properties,
        VTX::FieldType::Vector,
        struct_cache,
        struct_name,
        "Vector",
        [](const VTX::Vector& value) { return EntityInspectorViewService::FormatVector(value); },
        [](const VTX::Vector& value) { return EntityInspectorViewService::FormatRawVector(value); });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Quaternions",
        pc.quat_properties,
        VTX::FieldType::Quat,
        struct_cache,
        struct_name,
        "Quat",
        [](const VTX::Quat& value) { return EntityInspectorViewService::FormatQuat(value); },
        [](const VTX::Quat& value) { return EntityInspectorViewService::FormatRawQuat(value); });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Transforms",
        pc.transform_properties,
        VTX::FieldType::Transform,
        struct_cache,
        struct_name,
        "Transform",
        [](const VTX::Transform& value) { return EntityInspectorViewService::FormatTransform(value); },
        [](const VTX::Transform& value) { return EntityInspectorViewService::FormatRawTransform(value); });

    AppendPrimitiveNodes(
        out_nodes,
        id_prefix,
        "Ranges",
        pc.range_properties,
        VTX::FieldType::FloatRange,
        struct_cache,
        struct_name,
        "Range",
        [](const VTX::FloatRange& value) { return EntityInspectorViewService::FormatRange(value); },
        [](const VTX::FloatRange& value) { return EntityInspectorViewService::FormatRawRange(value); });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Byte Arrays",
        pc.byte_array_properties,
        VTX::FieldType::Int8,
        struct_cache,
        struct_name,
        [](uint8_t value) {
            char val[32];
            std::snprintf(val, sizeof(val), "0x%02X", static_cast<unsigned int>(value));
            return std::string(val);
        },
        [](uint8_t value) { return std::to_string(static_cast<unsigned int>(value)); });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Bool Arrays",
        pc.bool_arrays,
        VTX::FieldType::Bool,
        struct_cache,
        struct_name,
        [](uint8_t value) { return value != 0 ? std::string("True") : std::string("False"); },
        [](uint8_t value) { return value != 0 ? std::string("1") : std::string("0"); });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Int32 Arrays",
        pc.int32_arrays,
        VTX::FieldType::Int32,
        struct_cache,
        struct_name,
        [](int32_t value) { return std::to_string(value); },
        [](int32_t value) { return std::to_string(value); });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Int64 Arrays",
        pc.int64_arrays,
        VTX::FieldType::Int64,
        struct_cache,
        struct_name,
        [](int64_t value) { return std::to_string(value); },
        [](int64_t value) { return std::to_string(value); });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Float Arrays",
        pc.float_arrays,
        VTX::FieldType::Float,
        struct_cache,
        struct_name,
        [](float value) {
            return EntityInspectorViewService::FormatFloat(value);
        },
        [](float value) {
            return EntityInspectorViewService::FormatRawFloat(value);
        });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Double Arrays",
        pc.double_arrays,
        VTX::FieldType::Double,
        struct_cache,
        struct_name,
        [](double value) {
            return EntityInspectorViewService::FormatDouble(value);
        },
        [](double value) {
            return EntityInspectorViewService::FormatRawDouble(value);
        });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "String Arrays",
        pc.string_arrays,
        VTX::FieldType::String,
        struct_cache,
        struct_name,
        [](const std::string& value) { return value; },
        [](const std::string& value) { return value; });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Vector Arrays",
        pc.vector_arrays,
        VTX::FieldType::Vector,
        struct_cache,
        struct_name,
        [](const VTX::Vector& value) {
            return EntityInspectorViewService::FormatVector(value);
        },
        [](const VTX::Vector& value) {
            return EntityInspectorViewService::FormatRawVector(value);
        });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Quat Arrays",
        pc.quat_arrays,
        VTX::FieldType::Quat,
        struct_cache,
        struct_name,
        [](const VTX::Quat& value) {
            return EntityInspectorViewService::FormatQuat(value);
        },
        [](const VTX::Quat& value) {
            return EntityInspectorViewService::FormatRawQuat(value);
        });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Transform Arrays",
        pc.transform_arrays,
        VTX::FieldType::Transform,
        struct_cache,
        struct_name,
        [](const VTX::Transform& value) {
            return EntityInspectorViewService::FormatTransform(value);
        },
        [](const VTX::Transform& value) {
            return EntityInspectorViewService::FormatRawTransform(value);
        });

    AppendFlatArrayGroup(
        out_nodes,
        id_prefix,
        "Range Arrays",
        pc.range_arrays,
        VTX::FieldType::FloatRange,
        struct_cache,
        struct_name,
        [](const VTX::FloatRange& value) {
            return EntityInspectorViewService::FormatRange(value);
        },
        [](const VTX::FloatRange& value) {
            return EntityInspectorViewService::FormatRawRange(value);
        });

    if (!pc.any_struct_properties.empty()) {
        Node group = MakeGroupNode(
            id_prefix + "/structs",
            "Structs (" + std::to_string(pc.any_struct_properties.size()) + ")");

        for (size_t i = 0; i < pc.any_struct_properties.size(); ++i) {
            const auto& nested = pc.any_struct_properties[i];
            
            Node struct_node = MakeGroupNode(
                group.id + "/" + std::to_string(i),
                "Struct [" + std::to_string(i) + "] (Type ID: " + std::to_string(nested.entity_type_id) + ")");
            const std::string struct_schema_name = EntityInspectorViewService::ResolveSchemaName(
                struct_cache,
                VTX::FieldType::Struct,
                VTX::FieldContainerType::None,
                i);
            AttachSchemaLookup(
                struct_node,
                struct_name,
                static_cast<int>(i),
                VTX::FieldType::Struct,
                VTX::FieldContainerType::None,
                true);
            AttachSchemaPropertyLabel(struct_node, struct_name, struct_schema_name, i);
            BuildPropertyNodesRecursive(nested, global_cache, struct_node.id, struct_node.children);
            group.children.push_back(std::move(struct_node));
        }

        out_nodes.push_back(std::move(group));
    }

    if (pc.any_struct_arrays.SubArrayCount() > 0) {
        Node group = MakeGroupNode(
            id_prefix + "/struct_arrays",
            "Struct Arrays (" + std::to_string(pc.any_struct_arrays.SubArrayCount()) + " arrays)");

        for (size_t i = 0; i < pc.any_struct_arrays.SubArrayCount(); ++i) {
            const auto sub_array = pc.any_struct_arrays.GetSubArray(i);
            const std::string array_schema_name = EntityInspectorViewService::ResolveSchemaName(
                struct_cache,
                VTX::FieldType::Struct,
                VTX::FieldContainerType::Array,
                i);
            const int32_t nested_type_id = !sub_array.empty() ? sub_array.front().entity_type_id : -1;
            Node array_node = MakeGroupNode(
                group.id + "/" + std::to_string(i),
                nested_type_id >= 0
                    ? "Array [" + std::to_string(i) + "] (Type ID: " + std::to_string(nested_type_id) + ")"
                    : "Array [" + std::to_string(i) + "] (" + std::to_string(sub_array.size()) + " structs)");
            AttachSchemaLookup(
                array_node,
                struct_name,
                static_cast<int>(i),
                VTX::FieldType::Struct,
                VTX::FieldContainerType::Array,
                true);
            AttachSchemaPropertyLabel(array_node, struct_name, array_schema_name, i);

            const std::string nested_schema_struct_name = [&]() -> std::string {
                const auto* address = FindSchemaAddress(
                    struct_cache,
                    VTX::FieldType::Struct,
                    VTX::FieldContainerType::Array,
                    i);
                return address ? address->child_type_name : std::string{};
            }();

            for (size_t j = 0; j < sub_array.size(); ++j) {
                const auto& nested = sub_array[j];
                Node item_node = MakeGroupNode(
                    array_node.id + "/" + std::to_string(j),
                    "Item [" + std::to_string(j) + "]");
                std::string nested_struct_name = EntityInspectorViewService::ResolveStructName(
                    EntityInspectorViewService::ResolveStructCache(global_cache, nested.entity_type_id));
                if (nested_struct_name.empty()) {
                    nested_struct_name = nested_schema_struct_name;
                }
                if (!nested_struct_name.empty()) {
                    AttachSchemaStructLabel(
                        item_node,
                        nested_struct_name,
                        nested_struct_name + " [" + std::to_string(j) + "]");
                }
                BuildPropertyNodesRecursive(nested, global_cache, item_node.id, item_node.children);
                array_node.children.push_back(std::move(item_node));
            }
            if (sub_array.empty()) {
                array_node.children.push_back(MakeGroupNode(array_node.id + "/empty", "Empty Array"));
            }

            group.children.push_back(std::move(array_node));
        }

        out_nodes.push_back(std::move(group));
    }

    if (!pc.map_properties.empty()) {
        Node group = MakeGroupNode(
            id_prefix + "/maps",
            "Maps (" + std::to_string(pc.map_properties.size()) + ")");

        for (size_t i = 0; i < pc.map_properties.size(); ++i) {
            const auto& map_cont = pc.map_properties[i];
            Node map_node = MakeGroupNode(
                group.id + "/" + std::to_string(i),
                "Map [" + std::to_string(i) + "] (" + std::to_string(map_cont.keys.size()) + " pairs)");
            const std::string map_schema_name = EntityInspectorViewService::ResolveSchemaName(
                struct_cache,
                VTX::FieldType::Struct,
                VTX::FieldContainerType::Map,
                i);
            AttachSchemaPropertyLabel(map_node, struct_name, map_schema_name, i);
            AppendMapEntries(map_node, map_cont, global_cache);

            group.children.push_back(std::move(map_node));
        }

        out_nodes.push_back(std::move(group));
    }

    if (pc.map_arrays.SubArrayCount() > 0) {
        Node group = MakeGroupNode(
            id_prefix + "/map_arrays",
            "Map Arrays (" + std::to_string(pc.map_arrays.SubArrayCount()) + " arrays)");

        for (size_t i = 0; i < pc.map_arrays.SubArrayCount(); ++i) {
            const auto sub_array = pc.map_arrays.GetSubArray(i);
            Node array_node = MakeGroupNode(
                group.id + "/" + std::to_string(i),
                "Array [" + std::to_string(i) + "] (" + std::to_string(sub_array.size()) + " maps)");
            const std::string map_array_schema_name = EntityInspectorViewService::ResolveSchemaName(
                struct_cache,
                VTX::FieldType::Struct,
                VTX::FieldContainerType::Array,
                i);
            AttachSchemaPropertyLabel(array_node, struct_name, map_array_schema_name, i);

            for (size_t j = 0; j < sub_array.size(); ++j) {
                const auto& map_cont = sub_array[j];
                Node map_node = MakeGroupNode(
                    array_node.id + "/" + std::to_string(j),
                    "Map [" + std::to_string(j) + "] (" + std::to_string(map_cont.keys.size()) + " pairs)");
                if (!map_array_schema_name.empty()) {
                    map_node.schema_label = "Item [" + std::to_string(j) + "]";
                    map_node.schema_focus_struct_name = struct_name;
                    map_node.schema_focus_property_name = map_array_schema_name;
                    map_node.schema_focus_on_secondary_click = true;
                }
                AppendMapEntries(map_node, map_cont, global_cache);
                array_node.children.push_back(std::move(map_node));
            }

            group.children.push_back(std::move(array_node));
        }

        out_nodes.push_back(std::move(group));
    }
}

} // namespace

FrameResolveResult EntityInspectorViewService::ResolveFrameToDraw(VTX::IVtxReaderFacade& reader, const FrameResolveContext& context) {
    FrameResolveResult result;
    result.next_last_drawn_frame_index = context.last_drawn_frame_index;

    if (context.is_scrubbing) {
        result.is_loading = true;
        result.status_message = "Scrubbing to Frame " + std::to_string(context.current_frame) + "...";
        if (context.last_drawn_frame_index != -1) {
            result.frame_to_draw = reader.GetFrame(context.last_drawn_frame_index);
            if (result.frame_to_draw) {
                result.showing_stale_frame = true;
                result.stale_frame_index = context.last_drawn_frame_index;
            }
        }
        return result;
    }

    result.frame_to_draw = reader.GetFrame(context.current_frame);
    if (!result.frame_to_draw) {
        result.is_loading = true;
        result.status_message = "Loading chunk data for frame " + std::to_string(context.current_frame) + "...";
        if (context.last_drawn_frame_index != -1) {
            result.frame_to_draw = reader.GetFrame(context.last_drawn_frame_index);
            if (result.frame_to_draw) {
                result.showing_stale_frame = true;
                result.stale_frame_index = context.last_drawn_frame_index;
            }
        }
        return result;
    }

    result.status_message = "Inspecting Frame: " + std::to_string(context.current_frame);
    result.next_last_drawn_frame_index = context.current_frame;
    return result;
}

std::vector<EntityBucketView> EntityInspectorViewService::BuildEntityBuckets(
    const VTX::Frame& frame,
    const EntityInspectorState& state,
    const VTX::PropertyAddressCache* global_cache) {
    // Build bucket/entity list items with selection matched by bucket + unique ID.
    std::vector<EntityBucketView> buckets_vm;
    const auto& buckets = frame.GetBuckets();
    buckets_vm.reserve(buckets.size());

    int bucket_idx = 0;
    for (const auto& bucket : buckets) {
        const int current_bucket_idx = bucket_idx++;
        EntityBucketView bucket_vm;
        bucket_vm.id = "bucket_" + std::to_string(current_bucket_idx);
        bucket_vm.label = "Bucket " + std::to_string(current_bucket_idx) + " (" + std::to_string(bucket.entities.size()) + ")";
        bucket_vm.bucket_index = current_bucket_idx;
        bucket_vm.entities.reserve(bucket.entities.size());

        for (size_t i = 0; i < bucket.entities.size(); ++i) {
            const std::string entity_id = (i < bucket.unique_ids.size())
                ? bucket.unique_ids[i]
                : "UnknownEntity_" + std::to_string(i);
            const int entity_type_id = bucket.entities[i].entity_type_id;
            const std::string entity_type_name = ResolveStructName(ResolveStructCache(global_cache, entity_type_id));
            bucket_vm.entities.push_back(EntityListItem{
                .entity_id = entity_id,
                .entity_type_id = entity_type_id,
                .entity_type_name = entity_type_name,
                .is_selected = (state.selected_bucket_index == bucket_vm.bucket_index)
                    && (state.selected_entity_id == entity_id),
            });
        }

        buckets_vm.push_back(std::move(bucket_vm));
    }

    return buckets_vm;
}

EntityPropertiesViewModel EntityInspectorViewService::BuildPropertyTree(
    const VTX::PropertyContainer& pc,
    const VTX::PropertyAddressCache* global_cache) {
    EntityPropertiesViewModel vm;
    BuildPropertyNodesRecursive(pc, global_cache, "root", vm.roots);
    return vm;
}

EntityPropertyCommand EntityInspectorViewService::BuildActivateCommand(
    const EntityPropertyNode& node,
    const VTX::PropertyAddressCache* global_cache) {
    EntityPropertyCommand command;
    if (!node.is_property) {
        return command;
    }

    command.copy_to_clipboard = true;
    command.clipboard_text = node.raw_value;
    const std::string schema_name = ResolveSchemaName(global_cache, node);
    if (!schema_name.empty() && !node.struct_name.empty()) {
        command.request_schema_focus = true;
        command.struct_name = node.struct_name;
        command.property_name = schema_name;
    }
    return command;
}

EntityPropertyCommand EntityInspectorViewService::BuildSchemaFocusCommand(
    const EntityPropertyNode& node,
    const VTX::PropertyAddressCache* global_cache) {
    EntityPropertyCommand command;
    if (!node.schema_focus_on_secondary_click) {
        return command;
    }

    if (!node.schema_focus_struct_name.empty()) {
        command.request_schema_focus = true;
        command.struct_name = node.schema_focus_struct_name;
        command.property_name = node.schema_focus_property_name;
        return command;
    }

    if (node.struct_name.empty()) {
        return command;
    }

    const std::string schema_name = ResolveSchemaName(global_cache, node);
    if (schema_name.empty()) {
        return command;
    }

    command.request_schema_focus = true;
    command.struct_name = node.struct_name;
    command.property_name = schema_name;
    return command;
}

EntityInspectorScreenResult EntityInspectorViewService::BuildScreen(
    VTX::IVtxReaderFacade* reader,
    bool has_loaded_replay,
    bool is_scrubbing_timeline,
    int current_frame,
    const EntityInspectorState& state) {
    EntityInspectorScreenResult result;
    result.state = state;
    result.view_model.has_replay = has_loaded_replay && reader != nullptr;
    if (!result.view_model.has_replay) {
        return result;
    }

    const auto frame_result = ResolveFrameToDraw(
        *reader,
        FrameResolveContext{
            .is_scrubbing = is_scrubbing_timeline,
            .current_frame = current_frame,
            .last_drawn_frame_index = state.last_drawn_frame_index,
        });
    result.state.last_drawn_frame_index = frame_result.next_last_drawn_frame_index;
    result.view_model.is_loading = frame_result.is_loading;
    result.view_model.disable_panels = frame_result.is_loading;
    result.view_model.status_message = frame_result.status_message;
    result.view_model.status_tone = frame_result.is_loading
        ? (is_scrubbing_timeline ? EntityStatusTone::Warning : EntityStatusTone::Error)
        : EntityStatusTone::Normal;
    if (frame_result.showing_stale_frame) {
        result.view_model.stale_frame_message = "(Showing stale frame " + std::to_string(frame_result.stale_frame_index) + ")";
    }
    if (!frame_result.frame_to_draw) {
        result.view_model.empty_properties_message = "Waiting for frame data to stream from disk...";
        return result;
    }

    result.view_model.has_frame = true;
    VTX::PropertyAddressCache cache_copy;
    const VTX::PropertyAddressCache* global_cache = nullptr;
    if (reader) {
        cache_copy = reader->GetPropertyAddressCache();
        global_cache = &cache_copy;
    }
    result.view_model = BuildFrameViewModel(*frame_result.frame_to_draw, state, global_cache);
    result.view_model.has_replay = true;
    result.view_model.has_frame = true;
    result.view_model.is_loading = frame_result.is_loading;
    result.view_model.disable_panels = frame_result.is_loading;
    result.view_model.status_message = frame_result.status_message;
    result.view_model.status_tone = frame_result.is_loading
        ? (is_scrubbing_timeline ? EntityStatusTone::Warning : EntityStatusTone::Error)
        : EntityStatusTone::Normal;
    result.view_model.stale_frame_message = frame_result.showing_stale_frame
        ? "(Showing stale frame " + std::to_string(frame_result.stale_frame_index) + ")"
        : std::string{};
    return result;
}

EntityInspectorState EntityInspectorViewService::SelectEntity(
    const EntityInspectorState& state,
    int bucket_index,
    const std::string& entity_id) {
    // Persist the selected entity identity as (bucket index, unique ID).
    EntityInspectorState next_state = state;
    next_state.selected_bucket_index = bucket_index;
    next_state.selected_entity_id = entity_id;
    return next_state;
}

EntityInspectorEffect EntityInspectorViewService::BuildPropertyActivateEffect(
    const EntityPropertyNode& node,
    const VTX::PropertyAddressCache* global_cache) {
    const auto command = BuildActivateCommand(node, global_cache);
    return EntityInspectorEffect{
        .copy_to_clipboard = command.copy_to_clipboard,
        .clipboard_text = command.clipboard_text,
        .request_schema_focus = command.request_schema_focus,
        .focus_schema_window = command.request_schema_focus,
        .schema_struct_name = command.struct_name,
        .schema_property_name = command.property_name,
    };
}

EntityInspectorEffect EntityInspectorViewService::BuildPropertyContextEffect(
    const EntityPropertyNode& node,
    const VTX::PropertyAddressCache* global_cache) {
    const auto command = BuildSchemaFocusCommand(node, global_cache);
    return EntityInspectorEffect{
        .request_schema_focus = command.request_schema_focus,
        .schema_struct_name = command.struct_name,
        .schema_property_name = command.property_name,
    };
}

const VTX::PropertyContainer* EntityInspectorViewService::FindSelectedEntity(
    const VTX::Frame& frame,
    const EntityInspectorState& state) {
    // Resolve selected entity inside the selected bucket first to avoid cross-bucket ID collisions.
    if (state.selected_entity_id.empty() || state.selected_bucket_index < 0) {
        return nullptr;
    }

    const auto& buckets = frame.GetBuckets();
    if (state.selected_bucket_index >= static_cast<int>(buckets.size())) {
        return nullptr;
    }

    const auto& bucket = buckets[static_cast<size_t>(state.selected_bucket_index)];
    for (size_t i = 0; i < bucket.entities.size(); ++i) {
        const std::string id = (i < bucket.unique_ids.size())
            ? bucket.unique_ids[i]
            : "UnknownEntity_" + std::to_string(i);
        if (id == state.selected_entity_id) {
            return &bucket.entities[i];
        }
    }
    return nullptr;
}

EntityInspectorViewModel EntityInspectorViewService::BuildFrameViewModel(
    const VTX::Frame& frame,
    const EntityInspectorState& state,
    const VTX::PropertyAddressCache* global_cache) {
    // Build details view model using the exact selected bucket/entity tuple.
    EntityInspectorViewModel view_model;
    view_model.buckets = BuildEntityBuckets(frame, state, global_cache);
    if (view_model.buckets.empty()) {
        view_model.empty_properties_message = "No buckets in this frame.";
        return view_model;
    }

    if (state.selected_entity_id.empty() || state.selected_bucket_index < 0) {
        view_model.empty_properties_message = "Select an entity from the list to view its properties.";
        return view_model;
    }

    const VTX::PropertyContainer* pc = FindSelectedEntity(frame, state);
    if (!pc) {
        view_model.empty_properties_message = "Entity '" + state.selected_entity_id + "' in Bucket " +
            std::to_string(state.selected_bucket_index) + " no longer exists.";
        return view_model;
    }

    view_model.header.entity_label = "Entity: " + state.selected_entity_id + " (Bucket " +
        std::to_string(state.selected_bucket_index) + ")";
    view_model.header.type_label = "Type ID: " + std::to_string(pc->entity_type_id);
    const VTX::StructSchemaCache* selected_struct_cache = ResolveStructCache(global_cache, pc->entity_type_id);
    view_model.header.type_name = ResolveStructName(selected_struct_cache);
    view_model.header.missing_field_names = BuildMissingFieldNames(*pc, selected_struct_cache);
    view_model.header.show_hash = pc->content_hash != 0;
    if (view_model.header.show_hash) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "Hash: 0x%llX", static_cast<unsigned long long>(pc->content_hash));
        view_model.header.hash_label = buffer;
    }
    view_model.properties = BuildPropertyTree(*pc, global_cache);
    return view_model;
}

const VTX::StructSchemaCache* EntityInspectorViewService::ResolveStructCache(
    const VTX::PropertyAddressCache* global_cache,
    int32_t entity_type_id) {
    if (!global_cache) {
        return nullptr;
    }

    auto it = global_cache->structs.find(entity_type_id);
    if (it == global_cache->structs.end()) {
        return nullptr;
    }

    return &it->second;
}

std::string EntityInspectorViewService::ResolveSchemaName(
    const VTX::StructSchemaCache* struct_cache,
    VTX::FieldType expected_type,
    VTX::FieldContainerType container_type,
    size_t index) {
    if (!struct_cache) {
        return "";
    }

    const auto exact_it = struct_cache->names_by_lookup_key.find(
        VTX::MakePropertyLookupKey(static_cast<int32_t>(index), expected_type, container_type));
    if (exact_it != struct_cache->names_by_lookup_key.end()) {
        return exact_it->second;
    }

    if (expected_type == VTX::FieldType::Int32) {
        const auto enum_it = struct_cache->names_by_lookup_key.find(
            VTX::MakePropertyLookupKey(static_cast<int32_t>(index), VTX::FieldType::Enum, container_type));
        if (enum_it != struct_cache->names_by_lookup_key.end()) {
            return enum_it->second;
        }

        const auto int8_it = struct_cache->names_by_lookup_key.find(
            VTX::MakePropertyLookupKey(static_cast<int32_t>(index), VTX::FieldType::Int8, container_type));
        if (int8_it != struct_cache->names_by_lookup_key.end()) {
            return int8_it->second;
        }
    }

    return "";
}

const VTX::StructSchemaCache* EntityInspectorViewService::ResolveStructCache(
    const VTX::PropertyAddressCache* global_cache,
    const std::string& struct_name) {
    if (!global_cache || struct_name.empty()) {
        return nullptr;
    }

    const auto type_it = global_cache->name_to_id.find(struct_name);
    if (type_it == global_cache->name_to_id.end()) {
        return nullptr;
    }

    return ResolveStructCache(global_cache, type_it->second);
}

std::string EntityInspectorViewService::ResolveSchemaName(
    const VTX::PropertyAddressCache* global_cache,
    const EntityPropertyNode& node) {
    if (!global_cache || node.schema_index < 0 || node.struct_name.empty()) {
        return "";
    }

    const auto* struct_cache = ResolveStructCache(global_cache, node.struct_name);
    return ResolveSchemaName(
        struct_cache,
        node.schema_type,
        node.schema_container_type,
        static_cast<size_t>(node.schema_index));
}

std::string EntityInspectorViewService::ResolveStructName(const VTX::StructSchemaCache* struct_cache) {
    if (!struct_cache) {
        return "";
    }
    return struct_cache->name;
}

std::string EntityInspectorViewService::FormatVector(const VTX::Vector& v) {
    char val[128];
    std::snprintf(val, sizeof(val), "X: %.3f Y: %.3f Z: %.3f", v.x, v.y, v.z);
    return val;
}

std::string EntityInspectorViewService::FormatQuat(const VTX::Quat& q) {
    char val[128];
    std::snprintf(val, sizeof(val), "X: %.3f Y: %.3f Z: %.3f W: %.3f", q.x, q.y, q.z, q.w);
    return val;
}

std::string EntityInspectorViewService::FormatTransform(const VTX::Transform& t) {
    char val[320];
    std::snprintf(
        val,
        sizeof(val),
        "Loc(%.1f, %.1f, %.1f) Rot(%.2f, %.2f, %.2f, %.2f) Scale(%.2f, %.2f, %.2f)",
        t.translation.x,
        t.translation.y,
        t.translation.z,
        t.rotation.x,
        t.rotation.y,
        t.rotation.z,
        t.rotation.w,
        t.scale.x,
        t.scale.y,
        t.scale.z);
    return val;
}

std::string EntityInspectorViewService::FormatRange(const VTX::FloatRange& r) {
    char val[128];
    std::snprintf(val, sizeof(val), "Min: %.2f Max: %.2f ValNorm: %.2f", r.min, r.max, r.value_normalized);
    return val;
}

std::string EntityInspectorViewService::FormatFloat(float value) {
    char val[64];
    std::snprintf(val, sizeof(val), "%.4f", value);
    return val;
}

std::string EntityInspectorViewService::FormatDouble(double value) {
    char val[64];
    std::snprintf(val, sizeof(val), "%.6f", value);
    return val;
}

std::string EntityInspectorViewService::FormatRawVector(const VTX::Vector& v) {
    char val[160];
    std::snprintf(
        val,
        sizeof(val),
        "X: %.*g Y: %.*g Z: %.*g",
        std::numeric_limits<float>::max_digits10,
        v.x,
        std::numeric_limits<float>::max_digits10,
        v.y,
        std::numeric_limits<float>::max_digits10,
        v.z);
    return val;
}

std::string EntityInspectorViewService::FormatRawQuat(const VTX::Quat& q) {
    char val[192];
    std::snprintf(
        val,
        sizeof(val),
        "X: %.*g Y: %.*g Z: %.*g W: %.*g",
        std::numeric_limits<float>::max_digits10,
        q.x,
        std::numeric_limits<float>::max_digits10,
        q.y,
        std::numeric_limits<float>::max_digits10,
        q.z,
        std::numeric_limits<float>::max_digits10,
        q.w);
    return val;
}

std::string EntityInspectorViewService::FormatRawTransform(const VTX::Transform& t) {
    char val[384];
    std::snprintf(
        val,
        sizeof(val),
        "Loc(%.*g, %.*g, %.*g) Rot(%.*g, %.*g, %.*g, %.*g) Scale(%.*g, %.*g, %.*g)",
        std::numeric_limits<float>::max_digits10,
        t.translation.x,
        std::numeric_limits<float>::max_digits10,
        t.translation.y,
        std::numeric_limits<float>::max_digits10,
        t.translation.z,
        std::numeric_limits<float>::max_digits10,
        t.rotation.x,
        std::numeric_limits<float>::max_digits10,
        t.rotation.y,
        std::numeric_limits<float>::max_digits10,
        t.rotation.z,
        std::numeric_limits<float>::max_digits10,
        t.rotation.w,
        std::numeric_limits<float>::max_digits10,
        t.scale.x,
        std::numeric_limits<float>::max_digits10,
        t.scale.y,
        std::numeric_limits<float>::max_digits10,
        t.scale.z);
    return val;
}

std::string EntityInspectorViewService::FormatRawRange(const VTX::FloatRange& r) {
    char val[192];
    std::snprintf(
        val,
        sizeof(val),
        "Min: %.*g Max: %.*g ValNorm: %.*g",
        std::numeric_limits<float>::max_digits10,
        r.min,
        std::numeric_limits<float>::max_digits10,
        r.max,
        std::numeric_limits<float>::max_digits10,
        r.value_normalized);
    return val;
}

std::string EntityInspectorViewService::FormatRawFloat(float value) {
    char val[64];
    std::snprintf(val, sizeof(val), "%.*g", std::numeric_limits<float>::max_digits10, value);
    return val;
}

std::string EntityInspectorViewService::FormatRawDouble(double value) {
    char val[64];
    std::snprintf(val, sizeof(val), "%.*g", std::numeric_limits<double>::max_digits10, value);
    return val;
}

} // namespace VtxServices
