import json
import sys
import os

def normalize_schema_entries(data):
    """Supports legacy `schema` and clean-schema `property_mapping` payloads."""
    schema_list = data.get("schema")
    if isinstance(schema_list, list) and schema_list:
        return schema_list

    property_mapping = data.get("property_mapping")
    if not isinstance(property_mapping, list):
        return []

    type_id_to_type = {
        "Bool": "bool",
        "Int8": "int8",
        "Int32": "int32",
        "Int64": "int64",
        "Float": "float",
        "Double": "double",
        "String": "string",
        "Transform": "transform",
        "Vector": "vector",
        "Quat": "quat",
        "FloatRange": "range",
        "Struct": "struct",
    }
    container_type_to_container = {
        "none": "",
        "array": "array",
        "map": "map",
        "struct": "struct",
    }

    normalized = []
    for struct_def in property_mapping:
        struct_name = struct_def.get("struct", "")
        values = []

        for field in struct_def.get("values", []):
            meta = field.get("meta", {}) or {}
            raw_type = meta.get("type")
            if isinstance(raw_type, str):
                raw_type = raw_type.lower()

            if not raw_type:
                raw_type = type_id_to_type.get(field.get("typeId", ""), "")

            if raw_type == "byte":
                raw_type = "int8"
            if raw_type == "floatrange":
                raw_type = "range"

            struct_type = field.get("structType", "")
            container_type = str(field.get("containerType", "")).lower()
            container = container_type_to_container.get(container_type, "")

            values.append({
                "name": field.get("name", ""),
                "type": raw_type,
                "structType": struct_type,
                "keyType": meta.get("keyType", ""),
                "container": container
            })

        normalized.append({
            "struct": struct_name,
            "values": values
        })

    return normalized

def clean_enum_name(name):
    """Converts strings like 'smoke_grenade' or 'he' to PascalCase 'SmokeGrenade', 'He'"""
    parts = str(name).replace(" ", "_").split("_")
    return "".join(p.capitalize() for p in parts)


def resolve_property_types(val, struct_name, known_structs, type_map):
    raw_type_field = val.get("type", "")
    is_list_enum = isinstance(raw_type_field, list)
    raw_type = "implicit_enum" if is_list_enum else str(raw_type_field).lower()

    struct_type = val.get("structType", "").lower()
    container = val.get("container", "").lower()
    is_array = val.get("array", False) or (container == "array")
    b_is_enum = val.get("bIsEnum", False)
    b_is_actor = val.get("bIsActor", False)

    if raw_type in ["struct", "complexobject", "object", "none", ""] and struct_type:
        lookup_type = struct_type
    else:
        lookup_type = raw_type
    lookup_type = lookup_type.replace("vtx::", "").replace("std::", "")

    if is_list_enum:
        accessor_type = "int32_t"
        user_ret_type = f"{struct_name}::E{val.get('name', '')}"
    elif b_is_enum or lookup_type in ["byte", "uint8"]:
        accessor_type = "int32_t"
        user_ret_type = "uint8_t"
    elif b_is_actor:
        accessor_type = "int64_t"
        user_ret_type = "int64_t"
    elif lookup_type in known_structs:
        accessor_type = "VTX::EntityView"
        user_ret_type = "VTX::EntityView"
    else:
        accessor_type = type_map.get(lookup_type)
        if not accessor_type:
            print(f"  [WARNING] Unknown type '{lookup_type}' in property '{val.get('name', '')}'. Using float by default.")
            accessor_type = "float"
        user_ret_type = accessor_type

    return {
        "is_array": is_array,
        "is_list_enum": is_list_enum,
        "accessor_type": accessor_type,
        "user_ret_type": user_ret_type,
    }


def emit_getter(lines, val, struct_name, info, complex_types, source_member):
    prop_name = val.get("name", "")
    accessor_type = info["accessor_type"]
    user_ret_type = info["user_ret_type"]
    is_array = info["is_array"]

    if accessor_type == "VTX::EntityView":
        if is_array:
            span_type = "VTX::PropertyContainer"
            lines.append(f"        /** @brief Returns a span of nested structures */")
            lines.append(f"        inline std::span<const {span_type}> Get{prop_name}() const {{")
            lines.append(f"            static VTX::PropertyKey<std::span<const {span_type}>> cached_key = accessor.GetViewArrayKey(EntityType::{struct_name}, {struct_name}::{prop_name});")
            lines.append(f"            return {source_member}.AsView().GetViewArray(cached_key);" if source_member == "data_mut" else f"            return {source_member}.GetViewArray(cached_key);")
            lines.append("        }")
        else:
            lines.append(f"        /** @brief Returns a nested view */")
            lines.append(f"        inline VTX::EntityView Get{prop_name}() const {{")
            lines.append(f"            static VTX::PropertyKey<VTX::EntityView> cached_key = accessor.GetViewKey(EntityType::{struct_name}, {struct_name}::{prop_name});")
            lines.append(f"            return {source_member}.AsView().GetView(cached_key);" if source_member == "data_mut" else f"            return {source_member}.GetView(cached_key);")
            lines.append("        }")
    else:
        if is_array:
            span_ret_type = "uint8_t" if accessor_type == "bool" else accessor_type
            lines.append(f"        /** @brief Returns a span of {accessor_type} */")
            lines.append(f"        inline std::span<const {span_ret_type}> Get{prop_name}() const {{")
            lines.append(f"            static VTX::PropertyKey<{accessor_type}> cached_key = accessor.GetArray<{accessor_type}>(EntityType::{struct_name}, {struct_name}::{prop_name});")
            lines.append(f"            return {source_member}.AsView().GetArray<{accessor_type}>(cached_key);" if source_member == "data_mut" else f"            return {source_member}.GetArray<{accessor_type}>(cached_key);")
            lines.append("        }")
        else:
            ret_ref = "&" if user_ret_type in complex_types else ""
            const_ref = f"const {user_ret_type}{ret_ref}" if user_ret_type in complex_types else user_ret_type
            lines.append(f"        inline {const_ref} Get{prop_name}() const {{")
            lines.append(f"            static VTX::PropertyKey<{accessor_type}> cached_key = accessor.Get<{accessor_type}>(EntityType::{struct_name}, {struct_name}::{prop_name});")
            if user_ret_type != accessor_type:
                lines.append(f"            return static_cast<{user_ret_type}>({source_member}.Get<{accessor_type}>(cached_key));")
            else:
                lines.append(f"            return {source_member}.Get<{accessor_type}>(cached_key);")
            lines.append("        }")
    lines.append("")


def emit_setter(lines, val, struct_name, info, complex_types):
    prop_name = val.get("name", "")
    accessor_type = info["accessor_type"]
    user_ret_type = info["user_ret_type"]
    is_array = info["is_array"]

    if accessor_type == "VTX::EntityView":
        # Nested struct: writeable accessor returns a mutator.
        if is_array:
            lines.append(f"        /** @brief Returns a mutable span of nested PropertyContainers */")
            lines.append(f"        inline std::span<VTX::PropertyContainer> GetMutable{prop_name}() {{")
            lines.append(f"            static VTX::PropertyKey<std::span<const VTX::PropertyContainer>> cached_key = accessor.GetViewArrayKey(EntityType::{struct_name}, {struct_name}::{prop_name});")
            lines.append(f"            return data_mut.GetMutableViewArray(cached_key);")
            lines.append("        }")
        else:
            lines.append(f"        /** @brief Returns a mutator over the nested struct */")
            lines.append(f"        inline VTX::EntityMutator GetMutable{prop_name}() {{")
            lines.append(f"            static VTX::PropertyKey<VTX::EntityView> cached_key = accessor.GetViewKey(EntityType::{struct_name}, {struct_name}::{prop_name});")
            lines.append(f"            return data_mut.GetMutableView(cached_key);")
            lines.append("        }")
    else:
        if is_array:
            span_ret_type = "uint8_t" if accessor_type == "bool" else accessor_type
            lines.append(f"        /** @brief Returns a mutable span of {accessor_type} */")
            lines.append(f"        inline std::span<{span_ret_type}> GetMutable{prop_name}() {{")
            lines.append(f"            static VTX::PropertyKey<{accessor_type}> cached_key = accessor.GetArray<{accessor_type}>(EntityType::{struct_name}, {struct_name}::{prop_name});")
            lines.append(f"            return data_mut.GetMutableArray<{accessor_type}>(cached_key);")
            lines.append("        }")
        else:
            param_type = f"const {user_ret_type}&" if user_ret_type in complex_types else user_ret_type
            lines.append(f"        inline void Set{prop_name}({param_type} value) {{")
            lines.append(f"            static VTX::PropertyKey<{accessor_type}> cached_key = accessor.Get<{accessor_type}>(EntityType::{struct_name}, {struct_name}::{prop_name});")
            if user_ret_type != accessor_type:
                lines.append(f"            data_mut.Set<{accessor_type}>(cached_key, static_cast<{accessor_type}>(value));")
            else:
                lines.append(f"            data_mut.Set<{accessor_type}>(cached_key, value);")
            lines.append("        }")
    lines.append("")


def generate_cpp_header(json_path, output_path, namespace="Schema"):
    print(f"[VTX CodeGen] Reading schema from: {json_path}")

    try:
        with open(json_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except Exception as e:
        print(f"[VTX CodeGen] Error reading JSON: {e}")
        sys.exit(1)

    type_map = {
        "int": "int32_t",
        "int8": "int32_t",
        "int32": "int32_t",
        "int64": "int64_t",
        "uint64": "uint64_t",
        "uint16": "uint16_t",
        "float": "float",
        "double": "double",
        "string": "std::string",
        "fstring": "std::string",
        "fname": "std::string",
        "bool": "bool",
        "transform": "VTX::Transform",
        "vector": "VTX::Vector",
        "quat": "VTX::Quat",
        "range": "VTX::FloatRange"
    }

    complex_types = ["std::string", "VTX::Vector", "VTX::Quat", "VTX::Transform", "VTX::FloatRange"]

    lines = [
        "// =============================================================================",
        "// Autogenerated code by vtx_codegen.py! DO NOT MODIFY!",
        "// =============================================================================",
        "#pragma once",
        "#include <string>",
        "#include <cstdint>",
        "#include <span>",
        "#include \"vtx/common/vtx_property_cache.h\"",
        "#include \"vtx/common/vtx_frame_accessor.h\"",
        "#include \"vtx/writer/core/vtx_frame_mutation_view.h\"",
        "",
        f"namespace VTX::{namespace} {{",
        ""
    ]

    schema_list = normalize_schema_entries(data)

    lines.append("    /** @brief Strongly typed IDs for all entities in the schema */")
    lines.append("    enum class EntityType : int32_t {")

    known_structs = {}
    for i, struct_def in enumerate(schema_list):
        name = struct_def.get("struct", "")
        if name:
            known_structs[name.lower()] = name
            lines.append(f"        {name} = {i},")

    lines.append("        Unknown = -1")
    lines.append("    };")
    lines.append("")

    for struct_def in schema_list:
        struct_name = struct_def.get("struct", "")
        if not struct_name:
            continue

        # 1. Constexpr names + nested enums for list-typed properties.
        lines.append(f"    namespace {struct_name} {{")
        lines.append(f"        constexpr const char* StructName = \"{struct_name}\";")

        values = struct_def.get("values", [])
        for val in values:
            prop_name = val.get("name", "")
            if not prop_name:
                continue
            lines.append(f"        constexpr const char* {prop_name} = \"{prop_name}\";")
            raw_type_field = val.get("type", "")
            if isinstance(raw_type_field, list):
                lines.append(f"        enum class E{prop_name} : int32_t {{")
                for enum_idx, enum_str in enumerate(raw_type_field):
                    clean = clean_enum_name(enum_str)
                    lines.append(f"            {clean} = {enum_idx},")
                lines.append("        };")
        lines.append("    }")
        lines.append("")

        # 2. View class -- read-only.
        lines.append(f"    class {struct_name}View {{")
        lines.append("    private:")
        lines.append("        VTX::EntityView data_view;")
        lines.append("        const VTX::FrameAccessor& accessor;")
        lines.append("")
        lines.append("    public:")
        lines.append(f"        {struct_name}View(VTX::EntityView view, const VTX::FrameAccessor& acc) ")
        lines.append(f"            : data_view(view), accessor(acc) {{}}")
        lines.append("")
        lines.append(f"        {struct_name}View(const VTX::PropertyContainer& container, const VTX::FrameAccessor& acc) ")
        lines.append(f"            : data_view(container), accessor(acc) {{}}")
        lines.append("")
        for val in values:
            if not val.get("name", ""):
                continue
            info = resolve_property_types(val, struct_name, known_structs, type_map)
            emit_getter(lines, val, struct_name, info, complex_types, "data_view")
        lines.append("    };")
        lines.append("")

        # 3. Mutator class -- read + write.
        lines.append(f"    class {struct_name}Mutator {{")
        lines.append("    private:")
        lines.append("        VTX::EntityMutator data_mut;")
        lines.append("        const VTX::FrameAccessor& accessor;")
        lines.append("")
        lines.append("    public:")
        lines.append(f"        {struct_name}Mutator(VTX::EntityMutator m, const VTX::FrameAccessor& acc) ")
        lines.append(f"            : data_mut(m), accessor(acc) {{}}")
        lines.append("")
        lines.append(f"        {struct_name}Mutator(VTX::PropertyContainer& container, const VTX::FrameAccessor& acc) ")
        lines.append(f"            : data_mut(container), accessor(acc) {{}}")
        lines.append("")
        for val in values:
            if not val.get("name", ""):
                continue
            info = resolve_property_types(val, struct_name, known_structs, type_map)
            emit_getter(lines, val, struct_name, info, complex_types, "data_mut")
            emit_setter(lines, val, struct_name, info, complex_types)
        lines.append("    };")
        lines.append("")

    # 4. Strongly-typed iteration helpers.  Filter a BucketMutator by
    #    entity_type_id and yield the matching strongly-typed mutator.
    lines.append("    // ----------------------------------------------------------------")
    lines.append("    //  Strongly-typed iteration helpers")
    lines.append("    // ----------------------------------------------------------------")
    lines.append("    //  ForEachX(bucket, accessor, fn) walks a BucketMutator and calls")
    lines.append("    //  fn(XMutator&) only for entities whose entity_type_id matches the")
    lines.append("    //  struct.  Read-only counterparts (XView) are available via")
    lines.append("    //  ForEachXView.")
    lines.append("")
    for struct_def in schema_list:
        struct_name = struct_def.get("struct", "")
        if not struct_name:
            continue
        lines.append(f"    template <class Fn>")
        lines.append(f"    void ForEach{struct_name}(VTX::BucketMutator& bucket, const VTX::FrameAccessor& accessor, Fn fn) {{")
        lines.append(f"        constexpr int32_t kTypeId = static_cast<int32_t>(EntityType::{struct_name});")
        lines.append(f"        for (auto entity : bucket) {{")
        lines.append(f"            const auto* raw = entity.raw();")
        lines.append(f"            if (raw && raw->entity_type_id == kTypeId) {{")
        lines.append(f"                {struct_name}Mutator obj(entity, accessor);")
        lines.append(f"                fn(obj);")
        lines.append(f"            }}")
        lines.append(f"        }}")
        lines.append(f"    }}")
        lines.append("")

        lines.append(f"    template <class Fn>")
        lines.append(f"    void ForEach{struct_name}View(const VTX::Bucket& bucket, const VTX::FrameAccessor& accessor, Fn fn) {{")
        lines.append(f"        constexpr int32_t kTypeId = static_cast<int32_t>(EntityType::{struct_name});")
        lines.append(f"        for (const auto& container : bucket.entities) {{")
        lines.append(f"            if (container.entity_type_id == kTypeId) {{")
        lines.append(f"                {struct_name}View obj(container, accessor);")
        lines.append(f"                fn(obj);")
        lines.append(f"            }}")
        lines.append(f"        }}")
        lines.append(f"    }}")
        lines.append("")

    lines.append(f"}} // namespace VTX::{namespace}")

    output_content = "\n".join(lines)
    if os.path.exists(output_path):
        with open(output_path, 'r', encoding='utf-8') as f:
            if f.read() == output_content:
                print("[VTX CodeGen] No changes detected.")
                return

    output_dir = os.path.dirname(output_path)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(output_content)
    print(f"[VTX CodeGen] Generated: {output_path}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Use: python vtx_codegen.py <input.json> <output.h> [namespace]")
        sys.exit(1)
    namespace_param = sys.argv[3] if len(sys.argv) > 3 else "Schema"
    generate_cpp_header(sys.argv[1], sys.argv[2], namespace_param)
