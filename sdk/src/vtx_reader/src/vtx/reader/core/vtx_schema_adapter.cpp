#include "vtx/reader/core/vtx_schema_adapter.h"
#include "vtx_schema_generated.h"
#include "vtx_schema.pb.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"

namespace VTX {

    void PopulateCacheFromJsonString(const std::string& json_str, PropertyAddressCache& cache)
    {
        cache.Clear();
        if (json_str.empty()) {
            VTX_WARN("JSON string is empty in the .vtx file");
            return;
        }

        SchemaRegistry temp_registry;
        if (!temp_registry.LoadFromRawString(json_str)) {
            VTX_ERROR("Failed to parse embed json schema.");
            return;
        }

        for (const auto& [struct_name, struct_def] : temp_registry.GetDefinitions()) {
            int32_t type_id = temp_registry.GetStructTypeId(struct_name);
            if (type_id == -1) {
                 VTX_WARN("Struct '{}' does not have a TypeID registered in the enum.", struct_name);
                continue; 
            }
            cache.name_to_id[struct_name] = type_id;
            
            auto& struct_cache = cache.structs[type_id];   
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
    }
    // =========================================================================================
    // PROTOBUF (cppvtx::PropertySchema)
    // =========================================================================================
    void SchemaAdapter<cppvtx::ContextualSchema>::BuildCache(const cppvtx::ContextualSchema& src, PropertyAddressCache& cache)
    {
        PopulateCacheFromJsonString(src.schema(), cache);
    }

    // =========================================================================================
    // FLATBUFFERS (fbsvtx::PropertySchemaT)
    // =========================================================================================
    void SchemaAdapter<fbsvtx::ContextualSchemaT>::BuildCache(const fbsvtx::ContextualSchemaT& src, PropertyAddressCache& cache) 
    {
        PopulateCacheFromJsonString(src.schema, cache);

    }
}
