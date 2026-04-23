#include "services/schema_view_service.h"

#include <fstream>
#include "services/field_type_utils.h"

namespace VtxServices {

    void SchemaViewService::RequestHighlight(SchemaHighlightState& highlight, const std::string& struct_name,
                                             const std::string& property_name, float ttl_seconds) {
        highlight.struct_name = struct_name;
        highlight.property_name = property_name;
        highlight.time_remaining = ttl_seconds;
        highlight.do_scroll = true;
    }

    void SchemaViewService::TickHighlight(SchemaHighlightState& highlight, float delta_seconds) {
        if (highlight.time_remaining <= 0.0f) {
            return;
        }
        highlight.time_remaining -= delta_seconds;
        if (highlight.time_remaining < 0.0f) {
            highlight.time_remaining = 0.0f;
        }
    }

    bool SchemaViewService::IsHighlightActive(const SchemaHighlightState& highlight) {
        return highlight.time_remaining > 0.0f;
    }

    bool SchemaViewService::ExportJsonToFile(const std::string& path, const std::string& json_content) {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << json_content;
        return true;
    }

    std::string SchemaViewService::ResolveDisplayType(const VTX::PropertyAddress& address) {
        if (!address.child_type_name.empty()) {
            return address.child_type_name;
        }

        return FieldTypeToString(address.type_id);
    }

    VTX::FieldContainerType SchemaViewService::ResolveContainerType(const VTX::PropertyAddress& address) {
        return address.container_type;
    }

} // namespace VtxServices
