#pragma once
#include <string>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <vector>
#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/game_schema_types.h"
namespace VTX {

    constexpr uint64_t MakePropertyLookupKey(int32_t index, VTX::FieldType type_id,
                                             VTX::FieldContainerType container_type) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(index)) << 32) |
               (static_cast<uint64_t>(static_cast<uint8_t>(type_id)) << 16) |
               static_cast<uint64_t>(static_cast<uint8_t>(container_type));
    }

    /**
     * @brief C++20 helper to map native C++ types to your FieldType enum at compile time.
     * Used for type-safety validation when querying properties.
     */
    template <typename T>
    constexpr VTX::FieldType GetExpectedFieldType() {
        if constexpr (std::same_as<T, bool>)
            return VTX::FieldType::Bool;
        else if constexpr (std::same_as<T, int32_t>)
            return VTX::FieldType::Int32;
        else if constexpr (std::same_as<T, int64_t>)
            return VTX::FieldType::Int64;
        else if constexpr (std::same_as<T, float>)
            return VTX::FieldType::Float;
        else if constexpr (std::same_as<T, double>)
            return VTX::FieldType::Double;
        else if constexpr (std::same_as<T, std::string>)
            return VTX::FieldType::String;
        else if constexpr (std::same_as<T, VTX::Vector>)
            return VTX::FieldType::Vector;
        else if constexpr (std::same_as<T, VTX::Quat>)
            return VTX::FieldType::Quat;
        else if constexpr (std::same_as<T, VTX::Transform>)
            return VTX::FieldType::Transform;
        else if constexpr (std::same_as<T, VTX::FloatRange>)
            return VTX::FieldType::FloatRange;
        // else if constexpr (std::same_as<T, VTX::PropertyContainer>) return VTX::FieldType::Struct;
        else
            return VTX::FieldType::None;
    }

    /**
     * @brief O(1) fast access key. 
     * Resolved ONCE during Setup() and used directly in the hot loop.
     */
    template <typename T>
    struct PropertyKey {
        int32_t index = -1;
        bool IsValid() const { return index > -1; }
        bool operator==(const PropertyKey& other) const { return index == other.index; }
    };

    /**
     * @brief Represents the metadata of a property in the schema.
     * Now includes the type (and optionally array flag) for safety validation.
     */
    struct PropertyAddress {
        int32_t index = -1;                            ///< Index in the SoA vector or FlatArray offset list.
        VTX::FieldType type_id = VTX::FieldType::None; ///< Stored to validate the type at runtime.
        VTX::FieldContainerType container_type =
            VTX::FieldContainerType::None; ///< Full container metadata from schema.
        std::string child_type_name;
        bool IsValid() const { return index > -1; }
    };

    struct OrderedPropertyView {
        std::string_view name;
        const PropertyAddress* address = nullptr;
    };

    /**
     * @brief Groups all exclusive properties of a specific Struct (e.g., "PRLBall").
     * Maintains O(1) hash-based lookups for strings.
     */
    struct StructSchemaCache {
        std::string name;
        std::unordered_map<std::string, PropertyAddress> properties;
        std::unordered_map<uint64_t, std::string> names_by_lookup_key;
        std::vector<std::string> property_order;

        std::vector<OrderedPropertyView> GetPropertiesInOrder() const {
            std::vector<OrderedPropertyView> ordered_properties;
            if (!property_order.empty()) {
                ordered_properties.reserve(property_order.size());
                for (const auto& property_name : property_order) {
                    const auto it = properties.find(property_name);
                    if (it != properties.end()) {
                        ordered_properties.push_back(OrderedPropertyView {
                            .name = it->first,
                            .address = &it->second,
                        });
                    }
                }
                return ordered_properties;
            }

            ordered_properties.reserve(properties.size());
            for (const auto& [property_name, property_address] : properties) {
                ordered_properties.push_back(OrderedPropertyView {
                    .name = property_name,
                    .address = &property_address,
                });
            }
            return ordered_properties;
        }
    };

    /**
     * @brief The Global Cache: Clean, type-safe, and collision-free.
     * Completely replaces the previous 22 individual maps.
     */
    struct PropertyAddressCache {
        // Key: Struct Name (e.g., "PRLBall", "PRLCar")
        // Value: The schema cache containing its specific properties
        std::unordered_map<int32_t, StructSchemaCache> structs;
        std::unordered_map<std::string, int32_t> name_to_id;
        void Clear() {
            structs.clear();
            name_to_id.clear();
        }
    };
} // namespace VTX

namespace std {
    template <typename T>
    struct hash<VTX::PropertyKey<T>> {
        size_t operator()(const VTX::PropertyKey<T>& key) const noexcept { return std::hash<int32_t>()(key.index); }
    };
} // namespace std
