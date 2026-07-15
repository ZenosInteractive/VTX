#include "vtx/reader/core/vtx_schema_adapter.h"
#include "vtx_schema_generated.h"
#include "vtx_schema.pb.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"

namespace VTX {

    void PopulateCacheFromJsonString(const std::string& json_str, PropertyAddressCache& cache) {
        cache.Clear();
        if (json_str.empty()) {
            VTX_WARN("JSON string is empty in the .vtx file");
            return;
        }

        SchemaRegistry temp_registry;
        if (!temp_registry.LoadFromRawString(json_str)) {
            VTX_ERROR("Embedded schema is invalid; continuing without property resolution:\n{}",
                      temp_registry.GetValidationResult().ToString());
            return;
        }

        // The registry already builds the full cache while loading (including
        // type_max_indices and the schema's bucket names) -- reuse it verbatim.
        cache = temp_registry.GetPropertyCache();
    }
    // =========================================================================================
    // PROTOBUF (cppvtx::PropertySchema)
    // =========================================================================================
    void SchemaAdapter<cppvtx::ContextualSchema>::BuildCache(const cppvtx::ContextualSchema& src,
                                                             PropertyAddressCache& cache) {
        PopulateCacheFromJsonString(src.schema(), cache);
    }

    // =========================================================================================
    // FLATBUFFERS (fbsvtx::PropertySchemaT)
    // =========================================================================================
    void SchemaAdapter<fbsvtx::ContextualSchemaT>::BuildCache(const fbsvtx::ContextualSchemaT& src,
                                                              PropertyAddressCache& cache) {
        PopulateCacheFromJsonString(src.schema, cache);
    }
} // namespace VTX
