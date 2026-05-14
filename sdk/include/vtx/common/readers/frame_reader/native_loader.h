/**
 * @file native_loader.h
 * @brief Loader that walks C++ struct instances using StructMapping<T>
 *        and pushes their members into a VTX::PropertyContainer.
 * @author Zenos Interactive
 */

#pragma once

#include "vtx/common/adapters/native/struct_mapping.h"
#include "vtx/common/readers/frame_reader/loader_base.h"
#include "vtx/common/readers/frame_reader/type_traits.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace VTX {

    /**
     * @class GenericNativeLoader
     * @brief Twin of GenericFlatBufferLoader / GenericProtobufLoader for the
     *        "source is a plain C++ struct" case.
     *
     * Inherits from GenericLoaderBase via CRTP. Implements the two required hooks:
     *   - ResolveField: maps (entity_type_id, field_name) -> PropertyAddress*.
     *   - Load<T>: walks StructMapping<T>::GetFields() and dispatches each field
     *              to LoadField / LoadStruct / LoadArray as appropriate.
     */
    class GenericNativeLoader : public GenericLoaderBase<GenericNativeLoader> {
    public:
        explicit GenericNativeLoader(const PropertyAddressCache& cache, bool debug = false)
            : cache_(&cache)
            , debug_mode_(debug) {}

        /**
         * @brief Resolves a (struct, field) pair to a PropertyAddress via the cache.
         * @details struct_name is unused here (the cache is keyed by entity_type_id),
         *          kept in the signature for symmetry with the Proto loader and for debug.
         */
        const PropertyAddress* ResolveField(int32_t entity_type_id, const std::string& /*struct_name*/,
                                            const std::string& field_name) const {
            auto struct_it = cache_->structs.find(entity_type_id);
            if (struct_it == cache_->structs.end())
                return nullptr;
            auto prop_it = struct_it->second.properties.find(field_name);
            if (prop_it == struct_it->second.properties.end())
                return nullptr;
            return &prop_it->second;
        }

        /**
         * @brief Load a single C++ instance into a PropertyContainer.
         * @details Sets dest.entity_type_id (if not yet resolved), iterates
         *          StructMapping<T>::GetFields(), dispatches each field, and
         *          computes the container hash at the end.
         */
        template <typename T>
        void Load(const T& src, PropertyContainer& dest, const std::string& struct_name) {
            static_assert(has_struct_mapping_v<T>,
                          "GenericNativeLoader::Load<T> requires a StructMapping<T> specialization.");

            if (dest.entity_type_id == -1) {
                auto it = cache_->name_to_id.find(struct_name);
                if (it != cache_->name_to_id.end()) {
                    dest.entity_type_id = it->second;
                }
            }

            constexpr auto fields = StructMapping<T>::GetFields();
            std::apply([&](auto&&... f) { (ProcessField(src, dest, struct_name, f), ...); }, fields);
            dest.content_hash = Helpers::CalculateContainerHash(dest);
        }

        /**
         * @brief Load a top-level frame struct into a VTX::Frame.
         * @details Dispatches to StructFrameBinding<FrameT>::TransferToFrame,
         *          which the dev writes by hand (it owns the buckets/entities layout).
         */
        template <typename FrameT>
        void LoadFrame(const FrameT& src, VTX::Frame& dest, const std::string& schema_name) {
            StructFrameBinding<FrameT>::TransferToFrame(src, dest, *this, schema_name);
        }

        /**
         * @brief Load a nested scalar struct member into dest.any_struct_properties[slot].
         */
        template <typename NestedT>
        void LoadStruct(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                        const NestedT& src_nested) {
            const auto* addr = ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!addr) {
                return;
            }

            EnsureSize(dest.any_struct_properties, addr->index);
            Load(src_nested, dest.any_struct_properties[addr->index], addr->child_type_name);
        }

        /**
         * @brief Load a std::vector (or compatible range) member into the right flat array.
         * @details Primitives / VTX-native types -> base's FillFlatArray (push direct).
         *          Elements with StructMapping<> -> recurse per element, then push the
         *          resulting PropertyContainer to any_struct_arrays (Struct case) or
         *          collapse to a flat math array via PushToFlatArray.
         */
        template <typename Container>
        void LoadArray(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                       const Container& src_array) {
            if (src_array.empty()) {
                return;
            }
            const auto* addr = ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!addr) {
                return;
            }

            using ElementT = std::ranges::range_value_t<Container>;
            const int32_t idx = addr->index;
            const FieldType type = addr->type_id;

            if constexpr (has_struct_mapping_v<ElementT>) {
                const std::string& child_schema = addr->child_type_name;
                for (const auto& item : src_array) {
                    if (type == FieldType::Struct) {
                        PropertyContainer child;
                        Load(item, child, child_schema);
                        dest.any_struct_arrays.PushBack(idx, std::move(child));
                    } else {
                        PropertyContainer temp;
                        Load(item, temp, child_schema);
                        this->PushToFlatArray(dest, type, idx, temp);
                    }
                }
            } else {
                this->FillFlatArray(dest, type, idx, src_array);
            }
        }

    private:
        /**
         * @brief Per-field dispatcher called from Load<T>.
         * @details Looks at the C++ type of the member and routes:
         *          - std::vector<U>      -> LoadArray
         *          - has StructMapping   -> LoadStruct (recurse)
         *          - everything else     -> base's LoadField
         */
        template <typename T, typename Field>
        void ProcessField(const T& src, PropertyContainer& dest, const std::string& struct_name, const Field& field) {
            const auto& val = src.*(field.member_ptr);
            using V = std::remove_cv_t<std::remove_reference_t<decltype(val)>>;

            if constexpr (is_vector_v<V>) {
                LoadArray(dest, struct_name, field.name, val);
            } else if constexpr (has_struct_mapping_v<V>) {
                LoadStruct(dest, struct_name, field.name, val);
            } else {
                // Primitive (int, float, bool, std::string) or VTX-native type
                // (Vector, Quat, Transform, FloatRange, PropertyContainer).
                this->LoadField(dest, struct_name, field.name, val);
            }
        }

        const PropertyAddressCache* cache_;
        bool debug_mode_;
    };

} // namespace VTX
