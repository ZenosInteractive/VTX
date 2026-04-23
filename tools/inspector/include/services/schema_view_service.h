#pragma once

#include <string>

#include "vtx/common/vtx_property_cache.h"

namespace VtxServices {

    struct SchemaHighlightState {
        std::string struct_name;
        std::string property_name;
        float time_remaining = 0.0f;
        bool do_scroll = false;
    };

    class SchemaViewService {
    public:
        static void RequestHighlight(SchemaHighlightState& highlight, const std::string& struct_name,
                                     const std::string& property_name, float ttl_seconds = 2.0f);
        static void TickHighlight(SchemaHighlightState& highlight, float delta_seconds);
        static bool IsHighlightActive(const SchemaHighlightState& highlight);

        static bool ExportJsonToFile(const std::string& path, const std::string& json_content);

        static std::string ResolveDisplayType(const VTX::PropertyAddress& address);
        static VTX::FieldContainerType ResolveContainerType(const VTX::PropertyAddress& address);
    };

} // namespace VtxServices
