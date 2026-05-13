/**
 * @file protobuff_loader.h
 * @brief Generic Protobuf -> VTX::PropertyContainer loader, on top of GenericLoaderBase.
 *
 * Inherits LoadField / LoadBlob / AppendActorList / AppendSingleEntity /
 * StoreValue / EnsureSize / PushToFlatArray / FillFlatArray from the CRTP base.
 * Implements the format-specific bits: ResolveField (cache lookup) and the
 * reference-based Load / LoadStruct / LoadArray that walk Protobuf messages.
 *
 * Constructor still accepts a SchemaRegistry& for source-compat with existing
 * callers, but only the PropertyAddressCache is retained internally.
 *
 * @author Zenos Interactive
 */

#pragma once

#include "vtx/common/readers/frame_reader/loader_base.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include <google/protobuf/message.h>
#include <string>
#include <type_traits>

namespace VTX {

    template <typename ProtoType>
    struct ProtoBinding;

    class GenericProtobufLoader : public GenericLoaderBase<GenericProtobufLoader> {
    public:
        explicit GenericProtobufLoader(const SchemaRegistry& schema, bool debug = false)
            : cache_(&schema.GetPropertyCache())
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


        template <typename ProtoType>
        void Load(const ProtoType& src, PropertyContainer& dest, const std::string& struct_name) {
            if (dest.entity_type_id == -1) {
                auto it = cache_->name_to_id.find(struct_name);
                if (it != cache_->name_to_id.end()) {
                    dest.entity_type_id = it->second;
                }
            }
            ProtoBinding<ProtoType>::Transfer(src, dest, *this, struct_name);
            dest.content_hash = Helpers::CalculateContainerHash(dest);
        }

        template <typename FrameProtoType>
        void LoadFrame(const FrameProtoType& src, VTX::Frame& dest, const std::string& schema_name) {
            ProtoBinding<FrameProtoType>::TransferToFrame(src, dest, *this, schema_name);
        }


        template <typename NestedProtoType>
        void LoadStruct(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                        const NestedProtoType& src_nested) {
            const auto* addr = ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!addr)
                return;

            this->EnsureSize(dest.any_struct_properties, addr->index);
            Load(src_nested, dest.any_struct_properties[addr->index], addr->child_type_name);
        }

        template <typename RepeatedProtoType>
        void LoadArray(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                       const RepeatedProtoType& src_array) {
            if (src_array.empty())
                return;
            const auto* addr = ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!addr)
                return;

            using ElementT = typename RepeatedProtoType::value_type;
            const int32_t idx = addr->index;
            const FieldType type_id = addr->type_id;

            if constexpr (std::is_base_of_v<google::protobuf::Message, ElementT>) {
                const std::string& child_schema = addr->child_type_name;
                for (const auto& item : src_array) {
                    if (type_id == FieldType::Struct) {
                        PropertyContainer child_container;
                        Load(item, child_container, child_schema);
                        dest.any_struct_arrays.PushBack(idx, child_container);
                    } else {
                        PropertyContainer temp;
                        Load(item, temp, child_schema);
                        this->PushToFlatArray(dest, type_id, idx, temp);
                    }
                }
            } else {
                this->FillFlatArray(dest, type_id, idx, src_array);
            }
        }

    private:
        const PropertyAddressCache* cache_;
        bool debug_mode_;
    };

} // namespace VTX
