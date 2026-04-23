#pragma once
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_concepts.h"

namespace cppvtx {
    class ContextualSchema;
}

namespace fbsvtx {
    struct ContextualSchemaT;
}

namespace VTX {

    // =========================================================================================
    // PROTOBUF (cppvtx::PropertySchema)
    // =========================================================================================
    template <>
    struct SchemaAdapter<cppvtx::ContextualSchema> {
        static void BuildCache(const cppvtx::ContextualSchema& src, PropertyAddressCache& cache);
    };

    // =========================================================================================
    // FLATBUFFERS (fbsvtx::PropertySchemaT)
    // =========================================================================================
    template <>
    struct SchemaAdapter<fbsvtx::ContextualSchemaT> {
        static void BuildCache(const fbsvtx::ContextualSchemaT& src, PropertyAddressCache& cache);
    };

    /* Example of how easily extensible this is now:
    struct MyCustomSchema { std::map<string, map<string, PropertyAddress>> data; };
    
    template <>
    struct SchemaAdapter<MyCustomSchema> {
        static void BuildCache(const MyCustomSchema& src, PropertyAddressCache& cache) {
             for(auto& [struct_name, props] : src.data) {
                 for(auto& [prop_name, addr] : props) {
                     cache.structs[struct_name].properties[prop_name] = addr;
                 }
             }
        }
    };
    */

} // namespace VTX