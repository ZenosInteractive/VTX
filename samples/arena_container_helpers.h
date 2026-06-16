#pragma once
// arena_container_helpers.h -- shared helper for populating a VTX Map field.
//
// The FlatBuffers loader special-cases `containerType: Map` and builds
// `PropertyContainer::map_properties` for you (see flatbuffer_loader.h). The
// Protobuf, native and binary loaders do NOT -- their LoadArray pushes nested
// structs into `any_struct_arrays` unconditionally. So those three paths build
// the map explicitly via AppendMapEntry below, using the SAME key convention as
// the FlatBuffers loader so all four .vtx outputs end up identical:
//
//   key = entry's first non-empty string property
//       , else its first int32 property
//       , else a synthetic "Key_N".
//
// `value` is a fully-loaded child PropertyContainer (e.g. an AmmoEntry), exactly
// what the FlatBuffers loader stores as the map value.

#include <string>
#include <utility>

#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"

namespace ArenaHelpers {

    /// Append one entry to the Map-typed field @p field_name of @p dest.
    /// @tparam LoaderT any GenericLoaderBase-derived loader (exposes ResolveField).
    template <typename LoaderT>
    inline void AppendMapEntry(LoaderT& loader, VTX::PropertyContainer& dest, const std::string& schema_name,
                               const char* field_name, VTX::PropertyContainer value) {
        const VTX::PropertyAddress* addr = loader.ResolveField(dest.entity_type_id, schema_name, field_name);
        if (!addr || value.entity_type_id == -1) {
            return;
        }
        if (dest.map_properties.size() <= static_cast<size_t>(addr->index)) {
            dest.map_properties.resize(static_cast<size_t>(addr->index) + 1);
        }
        VTX::MapContainer& map = dest.map_properties[addr->index];

        std::string key;
        if (!value.string_properties.empty() && !value.string_properties.front().empty()) {
            key = value.string_properties.front();
        } else if (!value.int32_properties.empty()) {
            key = std::to_string(value.int32_properties.front());
        } else {
            key = "Key_" + std::to_string(map.keys.size());
        }

        map.keys.push_back(std::move(key));
        map.values.push_back(std::move(value));
    }

} // namespace ArenaHelpers
