/**
 * @file flatbuffer_adapter.h
 *
 * @brief Policies and logic for binding C++ structures to FlatBuffer tables.
 *
 * @details This file implements a static reflection mechanism specifically for
 * loading data from FlatBuffer binary tables into C++ objects (SoA or AoS).
 *
 * @author Zenos Interactive
 */

#pragma once
#include <string>
#include <tuple>
#include <flatbuffers/string.h>

#include "vtx/common/readers/frame_reader/type_traits.h"


namespace VTX {

    /**
     * @brief Forward declaration for the mapping trait.
     * @details Users must specialize this struct for their types.
     */
    template <typename T>
    struct FlatBufferMapping;

    /**
     * @struct FastField
     * @brief Binds a C++ member variable to a FlatBuffer accessor method.
     * @tparam CppType The C++ class being populated.
     * @tparam CppMemberType The type of the member variable in the C++ class.
     * @tparam FbTable The generated FlatBuffer table class.
     * @tparam FbReturnType The return type of the FlatBuffer accessor method.
     */
    template <typename CppType, typename CppMemberType, typename FbTable, typename FbReturnType>
    struct FastField {
        CppMemberType CppType::* cpp_member;        //  &BBChampion::Kills
        FbReturnType(FbTable::* fb_accessor)() const; // &FB::Champion::kills()
    };

    /**
     * @brief Helper function to create a FastField with automatic type deduction.
     * @param cpp_ptr Pointer to the C++ member variable.
     * @param fb_ptr Pointer to the FlatBuffer accessor method.
     * @return FastField instance.
     */
    template <typename CppType, typename CppMemberType, typename FbTable, typename FbReturnType>
    constexpr auto MapFB(CppMemberType CppType::* cpp_ptr, FbReturnType(FbTable::* fb_ptr)() const) {
        return FastField<CppType, CppMemberType, FbTable, FbReturnType>{cpp_ptr, fb_ptr};
    }

    /**
     * @class FlatBufferAdapter
     * @brief Static class that orchestrates the loading of FlatBuffer data.
     * @details Uses `std::apply` to iterate over the `FlatBufferMapping` tuple and
     * populate the target C++ structure.
     */
    class FlatBufferAdapter {
    public:
        template <typename T>
        static T Load(const typename FlatBufferMapping<T>::FbType* fb_table) {
            T instance{};

            if (!fb_table) return instance;

            // Retrieve the compile-time mapping tuple
            constexpr auto mapping = FlatBufferMapping<T>::GetFields();

            // Iterate over all mapped fields and process them
            std::apply([&](auto&&... fields) {
                (ProcessField(instance, fb_table, fields), ...);
                }, mapping);

            return instance;
        }

    private:

        /**
         * @brief Processes a single field mapping.
         * @details Calls the FB accessor, converts the type if necessary, and assigns to the C++ member.
         */
        template <typename T, typename FbTable, typename FieldType>
        static void ProcessField(T& instance, const FbTable* fb_table, const FieldType& field) {
            // Calls to the generated function created by flatc (fb_table->kills())
            auto raw_fb_value = (fb_table->*(field.fb_accessor))();

            //Infer var type_max_indices( int32_t, std::vector, etc.)
            using TargetType = std::remove_reference_t<decltype(instance.*(field.cpp_member))>;

            //Convert and assign
            instance.*(field.cpp_member) = Convert<TargetType>(raw_fb_value);
        }


        // --- Conversion Overloads (SFINAE) ---

        /** @brief Conversion for arithmetic types (int, float, etc.). */
        template <typename Target, typename Source>
        static std::enable_if_t<std::is_arithmetic_v<Target>, Target>
            Convert(Source fb_val) {
            return static_cast<Target>(fb_val);
        }

        /** @brief Conversion for Enum types. */
        template <typename Target, typename Source>
        static std::enable_if_t<std::is_enum_v<Target>, Target>
            Convert(Source fb_val) {
            return static_cast<Target>(fb_val);
        }

        /** @brief Conversion for Strings (FlatBuffer String* -> std::string). */
        template <typename Target>
        static std::enable_if_t<std::is_same_v<Target, std::string>, std::string>
            Convert(const flatbuffers::String* fb_str) {
            return fb_str ? fb_str->str() : "";
        }

        /** @brief Recursive conversion for nested structures (Sub-tables). */
        template <typename Target, typename FbSubTable>
        static std::enable_if_t<has_fb_mapping_v<Target>, Target>
            Convert(const FbSubTable* fb_sub_table) {
            Target result;
            if (fb_sub_table) {
                result = Load<Target>(fb_sub_table);
            }
            return result;
        }

        /**
        * @brief Conversion for Vectors of objects (FlatBuffer Vector<Offset<T>> -> std::vector<T>).
        * @details Handles both mapped structs and basic types inside vectors.
        */
        template <typename Target, typename FbSubTable>
        static std::enable_if_t<VTX::is_vector_v<Target>, Target>
            Convert(const flatbuffers::Vector<flatbuffers::Offset<FbSubTable>>* fb_vector) {
            Target result;
            if (!fb_vector) return result;

            using ElementType = typename Target::value_type;

            result.reserve(fb_vector->size());

            for (size_t i = 0; i < fb_vector->size(); ++i) {
                const FbSubTable* fb_element = fb_vector->Get(i);

                if constexpr (has_fb_mapping_v<ElementType>) {
                    result.push_back(Load<ElementType>(fb_element));
                }
                else {
                    result.push_back(static_cast<ElementType>(*fb_element));
                }
            }

            return result;
        }

        /**
        * @brief Conversion for Vectors of Pointers (Structs in FB are often pointers).
        */
        template <typename Target, typename U>
        static std::enable_if_t<VTX::is_vector_v<Target>, Target>
            Convert(const flatbuffers::Vector<const U*>* fb_vec) {
            Target result;
            if (!fb_vec) return result;

            result.reserve(fb_vec->size());
            for (const U* element : *fb_vec) {
                // Since 'element' is a pointer to a struct, we pass it to Load/Convert
                result.push_back(VTX::FlatBufferAdapter::Load<typename Target::value_type>(element));
            }
            return result;
        }
    };
}
