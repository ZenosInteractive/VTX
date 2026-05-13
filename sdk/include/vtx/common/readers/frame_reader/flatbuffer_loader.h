/**
 * @file flatbuffer_loader.h
 * @brief Generic FlatBuffers -> VTX::PropertyContainer loader, on top of GenericLoaderBase.
 *
 * Inherits LoadField / LoadBlob / AppendActorList / AppendSingleEntity /
 * StoreValue / EnsureSize / PushToFlatArray / FillFlatArray from the CRTP base.
 * Implements the format-specific bits: ResolveField (cache lookup) and the
 * pointer-based Load / LoadStruct / LoadArray that walk FlatBuffer tables.
 *
 * @author Zenos Interactive
 */

#pragma once

#include "vtx/common/readers/frame_reader/loader_base.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include <iterator>
#include <string>
#include <type_traits>

namespace VTX {

    template <typename T>
    struct FlatBufferBinding {
        static_assert(sizeof(T) == 0, "ERROR: Missing FlatBufferBinding<T> specialization.");
    };

    class GenericFlatBufferLoader : public GenericLoaderBase<GenericFlatBufferLoader> {
    public:
        explicit GenericFlatBufferLoader(const PropertyAddressCache& cache, bool debug = false)
            : cache_(&cache)
            , debug_mode_(debug) {}


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


        template <typename FBType>
        void Load(const FBType* src, PropertyContainer& dest, const std::string& struct_name) {
            if (!src)
                return;

            if (dest.entity_type_id == -1) {
                auto it = cache_->name_to_id.find(struct_name);
                if (it != cache_->name_to_id.end()) {
                    dest.entity_type_id = it->second;
                }
            }

            FlatBufferBinding<FBType>::Transfer(src, dest, *this, struct_name);
            dest.content_hash = Helpers::CalculateContainerHash(dest);
        }

        template <typename FrameFBType>
        void LoadFrame(const FrameFBType* src, VTX::Frame& dest, const std::string& schema_name) {
            if (!src)
                return;
            FlatBufferBinding<FrameFBType>::TransferToFrame(src, dest, *this, schema_name);
        }


        template <typename FBVectorType, typename IdFunc>
        void AppendActorList(VTX::Bucket& bucket, const std::string& schema_type, const FBVectorType* src_vector,
                             IdFunc id_func) {
            if (!src_vector || src_vector->size() == 0)
                return;
            GenericLoaderBase::AppendActorList(bucket, schema_type, *src_vector, id_func);
        }

        template <typename FBType, typename IdFunc>
        void AppendSingleEntity(VTX::Bucket& bucket, const std::string& schema_type, const FBType* src_item,
                                IdFunc id_func) {
            if (!src_item)
                return;
            GenericLoaderBase::AppendSingleEntity(bucket, schema_type, src_item, id_func);
        }


        template <typename NestedFBType>
        void LoadStruct(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                        const NestedFBType* src_nested) {
            if (!src_nested)
                return;
            const auto* addr = ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!addr)
                return;

            this->EnsureSize(dest.any_struct_properties, addr->index);
            Load(src_nested, dest.any_struct_properties[addr->index], addr->child_type_name);
        }

        template <typename FBVectorType>
        void LoadArray(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                       const FBVectorType* src_array) {
            if (!src_array || src_array->size() == 0)
                return;
            const auto* addr = ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!addr)
                return;

            const int32_t idx = addr->index;
            const FieldType type_id = addr->type_id;
            const std::string& child_schema = addr->child_type_name;
            const FieldContainerType container = addr->container_type;

            using IteratorT = typename FBVectorType::const_iterator;
            using ElementT = typename std::iterator_traits<IteratorT>::value_type;

            if constexpr (std::is_pointer_v<ElementT>) {
                for (auto it = src_array->begin(); it != src_array->end(); ++it) {
                    const auto* item = *it;
                    if (!item)
                        continue;

                    if (type_id == FieldType::Struct) {
                        PropertyContainer nested_container;
                        Load(item, nested_container, child_schema);

                        if (nested_container.entity_type_id != -1) {
                            if (container == FieldContainerType::Map) {
                                this->EnsureSize(dest.map_properties, idx);

                                std::string map_key;
                                if (!nested_container.string_properties.empty() &&
                                    !nested_container.string_properties[0].empty()) {
                                    map_key = nested_container.string_properties[0];
                                } else if (!nested_container.int32_properties.empty()) {
                                    map_key = std::to_string(nested_container.int32_properties[0]);
                                } else {
                                    map_key = "Key_" + std::to_string(dest.map_properties[idx].keys.size());
                                }

                                dest.map_properties[idx].keys.push_back(map_key);
                                dest.map_properties[idx].values.push_back(nested_container);
                            } else {
                                dest.any_struct_arrays.PushBack(idx, nested_container);
                            }
                        }
                    } else {
                        PropertyContainer temp;
                        Load(item, temp, child_schema);
                        this->PushToFlatArray(dest, type_id, idx, temp);
                    }
                }
            } else {
                this->FillFlatArray(dest, type_id, idx, *src_array);
            }
        }

    private:
        const PropertyAddressCache* cache_;
        bool debug_mode_;
    };

} // namespace VTX
