/**
 * @file game_schema_types.h
 * 
 * @brief Structures defining the metadata schema of the game data.
 * 
 * @details These structures are usually populated by parsing an external JSON manifest.
 * They allow the engine to map string names (e.g., "Health") to internal indices and types.
 * 
 * @author Zenos Interactive
 */
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <string_view>

namespace VTX {
    enum class FieldType : uint8_t {
        None = 0,
        Int8,
        Int32,
        Int64,
        Float,
        Double,
        Bool,
        String,
        Vector,
        Quat,
        Transform,
        FloatRange,
        Struct,
        Enum
    };

    enum class FieldContainerType : uint8_t { None = 0, Array, Map };

    /**
    * @brief Metadata of a field.
    */
    struct SchemaMeta {
        std::string type; ///< Bucket type_max_indices string (e.g., "int32", "float", "vector","struct"). For debugging
        std::string key_type;      ///< If this is a Map, the type_max_indices of the key (e.g., "string", "enum").
        std::string category;      ///< Organizational category (e.g., "Stats", "Transform").
        std::string display_name;  ///< Human-readable name for UI.
        std::string tooltip;       ///< Description or help text.
        std::string default_value; ///< Default value as a string.
        int32_t version;           ///< Version of this specific field definition.

        //internal only, schema resolver will generate this
        int32_t fixed_array_dim; ///< If > 0, indicates this field is a fixed-size array.
    };

    /**
     * @brief Definition of a single property/field within a struct.
     */
    struct SchemaField {
        std::string name;        ///< Internal variable name (e.g., "health_current").
        std::string struct_type; ///< If type_max_indices is a struct, the struct name
        FieldType type_id; ///< Bucket type_max_indices string (e.g., "int32", "float", "vector"). For quick swithc-case
        FieldType key_id;
        FieldContainerType
            container_type; ///< Bucket type_max_indices `container(e.g., "struct", "array", ",a"). For quick swithc-case
        SchemaMeta meta;    ///< Additional UI metadata.

        //internal only, schema resolver will generate this
        int32_t index; ///< The index in the generic PropertyContainer arrays.
    };

    /**
    * @brief Definition of a complex structure (like a Class or Struct in C++).
    */
    struct SchemaStruct {
        std::string struct_name;         ///< Name of the structure.
        std::vector<SchemaField> fields; ///< List of fields contained in this structure.

        /** * @brief Max indices required for each type in this struct.
         * @details Used to pre-allocate vectors in PropertyContainer (e.g., needs 5 ints, 3 floats).
         */
        std::vector<int32_t> type_max_indices;

        /// @brief Fast lookup map: Property Name -> Field Pointer.
        std::unordered_map<std::string, const SchemaField*> field_map;

        std::vector<SchemaField> GetFieldsByType(FieldType type, FieldContainerType container) const {
            std::vector<SchemaField> result;
            for (const auto& f : fields) {
                if (f.type_id == type && f.container_type == container) {
                    result.push_back(f);
                }
            }
            return result;
        }
    };


} // namespace VTX