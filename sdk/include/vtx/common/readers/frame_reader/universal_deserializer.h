/**
 * @file universal_deserializer.h
 * 
 * @brief Generic deserialization system capable of reading any format into C++ structs.
 * 
 * @details This class uses Policy-Based Design to decouple the reading logic from specific formats
 * (like JSON or FlatBuffers) and error handling strategies.
 * It relies on compile-time traits (StructMapping) to inspect target structures.
 * 
 * @author Zenos Interactive
 */

#pragma once
#include <iostream>
#include <string>
#include <tuple>

#include "type_traits.h"
#include "vtx/common/adapters/flatbuffer/flatbuffer_adapter.h"
#include "vtx/common/adapters/json/json_adapter.h"
#include "vtx/common/vtx_error_policy.h"

namespace VTX {

    /**
     * @brief Helper struct to deduce the correct Mapping type based on the Adapter.
     * @details This allows UniversalDeserializer to switch between JsonMapping<T> and
     * FlatBufferMapping<T> automatically at compile-time.
     * @tparam Adapter The input data adapter type.
     * @tparam T The target C++ struct.
     */
    template <typename Adapter, typename T>
    struct MappingSelector {
        // Default case: Fail compilation if adapter is unknown
        static_assert(sizeof(T) == 0, "Unknown Adapter type_max_indices in MappingSelector. Please specialize.");
    };

    /** @brief Specialization for JSON: Selects JsonMapping. */
    template <typename T>
    struct MappingSelector<VTX::JsonAdapter, T> {
        using MappingType = VTX::JsonMapping<T>;
        static constexpr bool HasMapping = VTX::has_mapping_v<T>;
        static constexpr const char* Name = "JsonMapping";
    };

    /** @brief Specialization for FlatBuffers: Selects FlatBufferMapping. */
    template <typename T>
    struct MappingSelector<VTX::FlatBufferAdapter, T> {
        using MappingType = VTX::FlatBufferMapping<T>;
        static constexpr bool HasMapping = VTX::has_fb_mapping_v<T>;
        static constexpr const char* Name = "FlatBufferMapping";
    };

    /**
    * @class UniversalDeserializer
    * @brief A static class that drives the deserialization process.
    * @tparam ErrorPolicy Policy class determining how to handle missing keys or type mismatches (e.g., Strict, Lenient).
    * @tparam NamingPolicy Policy class determining how to transform key names before lookup (e.g., Exact, Lowercase).
    */
    template <typename ErrorPolicy = VTX::StrictErrorPolicy, typename NamingPolicy = VTX::ExactNamingPolicy >
    class UniversalDeserializer {
    public:

        /**
         * @brief Main entry point to load data into a struct.
         * @details This function orchestrates the process:
         * 1. Checks if type T has a valid mapping (compile-time assertion).
         * 2. Retrieves the field list from StructMapping<T>.
         * 3. Iterates over all fields using std::apply and recursion.
         * @tparam T The C++ struct to populate. Must have a specialization of `StructMapping<T>`.
         * @tparam FormatAdapter The adapter wrapper for the source data (e.g., `JsonAdapter`).
         * @param adapter The adapter instance pointing to the current data node.
         * @return T A fully populated instance of the struct.
         */
        template <typename T,typename FormatAdapter>
        static T Load(const FormatAdapter& adapter) {

            // Compile-time check to ensure T has been registered with the reflection system.
            using RawAdapter = std::decay_t<FormatAdapter>;
            using Selector = MappingSelector<RawAdapter, T>;
            using Mapping = typename Selector::MappingType;

            // Compile-time validation
            static_assert(Selector::HasMapping,
                "ERROR: The target struct T does not have a registered Mapping for this format.");

            T instance{};

            // Retrieve fields from the selected mapping (JsonMapping or FlatBufferMapping)
            constexpr auto mapping = Mapping::GetFields();

            std::apply([&](auto&&... fields) {
                (ProcessField(instance, adapter, fields), ...);
                }, mapping);

            return instance;
        }

    private:
       
        /**
         * @brief Recursive function to deserialize a specific value based on its type.
         * @details Uses `if constexpr` to select the correct loading strategy (Vector, Map, Struct, or Primitive).
         * @tparam T The type of the value to deserialize.
         * @tparam FormatAdapter The type of the data adapter.
         * @param adapter The adapter pointing to the data.
         * @param out[out] Reference to the variable where the value will be stored.
         */
        template <typename T, typename FormatAdapter>
        static void DeserializeValue(const FormatAdapter& adapter, T& out)
        {
            //std::vector<T>
            if constexpr (is_vector_v<T>) {
                if (!adapter.IsArray()) return;
                using ElementType = typename T::value_type;
                for (size_t i = 0; i < adapter.Size(); ++i) {
                    ElementType element;
                    DeserializeValue(adapter.GetElement(i), element);
                    out.push_back(element);
                }
            }
            //std::map<K, V>
            else if constexpr (is_map_v<T>) {
                if (!adapter.IsMap()) return;

                using KeyType = typename T::key_type;
                using MappedType = typename T::mapped_type;

                std::vector<std::string> keys = adapter.GetKeys();

                for (const std::string& key_str : keys) {
                    MappedType mapped_val;
                    DeserializeValue(adapter.GetChild(key_str), mapped_val);

                    KeyType final_key = adapter.template ParseKey<KeyType>(key_str);
                    out.emplace(std::move(final_key), std::move(mapped_val));
                }
            }
            //mapped struct
            else if constexpr (has_mapping_v<T>) {
                out = Load<T>(adapter);
            }
            //basic types (int, float, bool, std::string)
            else 
            {
                static_assert(std::is_default_constructible_v<T>,
                    "ERROR: T is not a basic type_max_indices or vector or map, or is not registered as StructMapping.");

                out = adapter.template GetValue<T>();
            }
        }

        /**
         * @brief Processes a single field from the struct mapping.
         * @details Applies NamingPolicy, checks for key existence, handles ErrorPolicy,
         * and delegates to `DeserializeValue`.
         * * @tparam T The type of the parent struct.
         * @tparam FieldType The type of the metadata field (contains name and member pointer).
         * @param instance The struct instance being populated.
         * @param adapter The adapter.
         * @param field The metadata for the current field being processed.
         */
        template <typename T, typename FormatAdapter, typename FieldType>
        static void ProcessField(T& instance, const FormatAdapter& adapter, const FieldType& field) {

            // Transform the code name to the data name (e.g. "MyVar" -> "my_var")
            std::string target_key = NamingPolicy::Transform(field.json_key);

            if (!adapter.HasKey(target_key)) {
                // Delegate to policy (Throws or Logs)
                ErrorPolicy::OnMissingKey(target_key);
                return;
            }

            DeserializeValue(adapter.GetChild(target_key), instance.*(field.member_ptr));
        }

    };
}
