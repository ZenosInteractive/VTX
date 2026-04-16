/**
* @file vtx_reflection.h
 * 
 * @brief WIP - A way to make some basic reflection system for mapping structs from data sources to cpp
 * 
 * @details  WIP - NOT USABLE
 * @author Zenos Interactive
 */

#pragma once
#include <string>
#include "readers/schema_reader/game_schema_types.h"

namespace VTX {
    
    struct FieldMapping {
        std::string name;
        std::string json_name;
        FieldType type;
        int32_t index;       // index in PropertyContainer (SoA)
        std::string sub_type; // for complexObject / struct (struct name)
    };

    #define VTX_BEGIN_STRUCT(struct_name) \
    inline void Register_##struct_name(SchemaRegistry& reg) { \
    SchemaStruct s; s.name = #struct_name;

    #define VTX_FIELD(field_name, json_key, type_id, idx) \
    s.fields.push_back({#field_name, json_key, type_id, idx, ""});

    #define VTX_COMPLEX_FIELD(field_name, json_key, idx, sub_struct) \
    s.fields.push_back({#field_name, json_key, FieldType::Struct, idx, #sub_struct});

    #define VTX_END_STRUCT() \
    reg.AddStruct(std::move(s)); \
    }
}
