/**
 * @file flatbuffer_loader.h
 * @brief Provides an ULTRA-FAST generic loading mechanism from FlatBuffers to VTX native structures using PropertyAddressCache.
 */

#pragma once
#include <iostream>
#include <string_view>
#include <type_traits>
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"

namespace VTX {

    template<typename T>
    struct FlatBufferBinding {
         static_assert(sizeof(T) == 0, "ERROR: Missing Bindings for this FlatBuffer type.");
    };

    class GenericFlatBufferLoader {
    private:
        const PropertyAddressCache* cache_; 
        bool debug_mode_;

    public:
        explicit GenericFlatBufferLoader(const PropertyAddressCache& cache, bool debug = false) 
            : cache_(&cache), debug_mode_(debug) {}
        
        template <typename FBType>
        void Load(const FBType* src, PropertyContainer& dest, const std::string& struct_name) {
            if (!src) return;

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
        void LoadFrame(const FrameFBType* src, VTX::Frame& dest, const std::string& schemaName) {
            if (!src) return;
            FlatBufferBinding<FrameFBType>::TransferToFrame(src, dest, *this, schemaName);
        }

        template <typename FBVectorType, typename IdExtractorFunc>
        void AppendActorList(VTX::Bucket& targetBlock, const std::string& schemaType, const FBVectorType* src_vector, IdExtractorFunc idExtractor) {
            if (!src_vector || src_vector->size() == 0) return;
            targetBlock.entities.reserve(targetBlock.entities.size() + src_vector->size());

            for (auto it = src_vector->begin(); it != src_vector->end(); ++it) {
                ExtractActorWithIdFunc(*it, targetBlock, schemaType, idExtractor);
            }
        }

        template <typename FBType, typename IdExtractorFunc>
        void AppendSingleEntity(VTX::Bucket& targetBlock, const std::string& schemaType, const FBType* src_item, IdExtractorFunc idExtractor) {
            if (!src_item) return;
            ExtractActorWithIdFunc(src_item, targetBlock, schemaType, idExtractor);
        }
                
        template <typename T>
        void LoadField(PropertyContainer& dest, const std::string& /*struct_name*/, const std::string& field_name, const T& value) {
            
            auto struct_it = cache_->structs.find(dest.entity_type_id);
            if (struct_it == cache_->structs.end()) return;

            auto prop_it = struct_it->second.properties.find(field_name);
            if (prop_it == struct_it->second.properties.end()) return;

            const PropertyAddress& addr = prop_it->second;

            switch (addr.type_id) {
                case FieldType::String:     StoreValue(dest.string_properties, addr.index, value); break;
                case FieldType::Int8:
                case FieldType::Int32:
                case FieldType::Enum:       StoreValue(dest.int32_properties, addr.index, value);  break;
                case FieldType::Int64:      StoreValue(dest.int64_properties, addr.index, value);  break;
                case FieldType::Float:      StoreValue(dest.float_properties, addr.index, value);  break;
                case FieldType::Double:     StoreValue(dest.double_properties, addr.index, value); break;
                case FieldType::Bool:       StoreValue(dest.bool_properties, addr.index, value);   break;
                case FieldType::Vector:     StoreValue(dest.vector_properties, addr.index, value); break;
                case FieldType::Quat:       StoreValue(dest.quat_properties, addr.index, value);   break;
                case FieldType::Transform:  StoreValue(dest.transform_properties, addr.index, value); break;
                case FieldType::FloatRange: StoreValue(dest.range_properties, addr.index, value);  break;
                case FieldType::Struct: StoreValue(dest.any_struct_properties, addr.index, value); break;
                default: break;
            }
        }

        void LoadBlob(PropertyContainer& dest, const std::string& /*struct_name*/, const std::string& field_name, const void* data, size_t byte_size) {
            if (!data || byte_size == 0) return;

            auto struct_it = cache_->structs.find(dest.entity_type_id);
            if (struct_it == cache_->structs.end()) return;

            auto prop_it = struct_it->second.properties.find(field_name);
            if (prop_it == struct_it->second.properties.end()) return;

            int32_t idx = prop_it->second.index;
            const uint8_t* byte_data = static_cast<const uint8_t*>(data);

            for (size_t i = 0; i < byte_size; ++i) {
                dest.byte_array_properties.PushBack(idx, byte_data[i]);
            }
        }

        template <typename NestedFBType>
        void LoadStruct(PropertyContainer& dest, const std::string& /*struct_name*/, const std::string& field_name, const NestedFBType* src_nested) {
            if (!src_nested) return;

            auto struct_it = cache_->structs.find(dest.entity_type_id);
            if (struct_it == cache_->structs.end()) return;

            auto prop_it = struct_it->second.properties.find(field_name);
            if (prop_it == struct_it->second.properties.end()) return;

            int32_t index = prop_it->second.index;
            const std::string& child_schema = prop_it->second.child_type_name;

            EnsureSize(dest.any_struct_properties, index);
            Load(src_nested, dest.any_struct_properties[index], child_schema);
        }

        template <typename FBVectorType>
        void LoadArray(PropertyContainer& dest, const std::string& /*struct_name*/, const std::string& field_name, const FBVectorType* src_array) {
            if (!src_array || src_array->size() == 0) return;

            auto struct_it = cache_->structs.find(dest.entity_type_id);
            if (struct_it == cache_->structs.end()) return;

            auto prop_it = struct_it->second.properties.find(field_name);
            if (prop_it == struct_it->second.properties.end()) return;

            const int32_t idx = prop_it->second.index;
            const FieldType type_id = prop_it->second.type_id;
        
            const std::string& child_schema = prop_it->second.child_type_name; 
            const VTX::FieldContainerType container = prop_it->second.container_type;
            
            using IteratorT = typename FBVectorType::const_iterator;
            using ElementT = typename std::iterator_traits<IteratorT>::value_type;
        
            if constexpr (std::is_pointer_v<ElementT>) {
                for (auto it = src_array->begin(); it != src_array->end(); ++it) {
                    const auto* item = *it; 
                    if (!item) continue;

                    if (type_id == FieldType::Struct) {
                        PropertyContainer nested_container;
                        Load(item, nested_container, child_schema); 
                        
                        if (nested_container.entity_type_id != -1) {
                            if (container == VTX::FieldContainerType::Map) {
                            
                                EnsureSize(dest.map_properties, idx);
                                
                                std::string map_key;
                                if (!nested_container.string_properties.empty() && !nested_container.string_properties[0].empty()) {
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
                        PushToFlatArray(dest, type_id, idx, temp);
                    }
                }
            } else {
                FillFlatArray(dest, type_id, idx, src_array);
            }
        }
    
    private:
        template <typename FBVectorT>
        void FillFlatArray(PropertyContainer& dest, FieldType type, int32_t idx, const FBVectorT* src) {
            if (!src) return;
            for (auto it = src->begin(); it != src->end(); ++it) {
                const auto& val = *it; 
                switch (type) {
                    case FieldType::Int8:
                    case FieldType::Int32:
                    case FieldType::Enum:   dest.int32_arrays.PushBack(idx, static_cast<int32_t>(val)); break;
                    case FieldType::Int64:  dest.int64_arrays.PushBack(idx, static_cast<int64_t>(val)); break;
                    case FieldType::Float:  dest.float_arrays.PushBack(idx, static_cast<float>(val)); break;
                    case FieldType::Double: dest.double_arrays.PushBack(idx, static_cast<double>(val)); break;
                    case FieldType::Bool:   dest.bool_arrays.PushBack(idx, static_cast<bool>(val)); break;
                    case FieldType::String:
                        if constexpr (std::is_assignable_v<std::string&, decltype(val)>) {
                            dest.string_arrays.PushBack(idx, val);
                        } else {
                            dest.string_arrays.PushBack(idx, std::to_string(val));
                        }
                        break;
                    default: break;
                }
            }
        }
        
        void PushToFlatArray(PropertyContainer& dest, FieldType type, int32_t idx, const PropertyContainer& temp) const {
            switch (type) {
                case FieldType::Vector: if (!temp.vector_properties.empty()) dest.vector_arrays.PushBack(idx, temp.vector_properties[0]); break;
                case FieldType::Quat: if (!temp.quat_properties.empty()) dest.quat_arrays.PushBack(idx, temp.quat_properties[0]); break;
                case FieldType::Transform: if (!temp.transform_properties.empty()) dest.transform_arrays.PushBack(idx, temp.transform_properties[0]); break;
                case FieldType::FloatRange: if (!temp.range_properties.empty()) dest.range_arrays.PushBack(idx, temp.range_properties[0]); break;
                default: break;
            }
        }

        template <typename ActorPtrT, typename IdExtractorFunc>
        void ExtractActorWithIdFunc(const ActorPtrT src, VTX::Bucket& block, const std::string& schemaType, IdExtractorFunc idExtractor) {
            PropertyContainer& entity = block.entities.emplace_back();
            Load(src, entity, schemaType);
            std::string uid = idExtractor(src);
            block.unique_ids.push_back(uid);
        }
        
        template <typename Vec>
        inline void EnsureSize(Vec& v, size_t index) {
            if (v.size() <= index) {
                v.resize(index + 1);
            }
        }

        template <typename Vec, typename V>
        inline void StoreValue(Vec& vector, size_t index, const V& val) {
            EnsureSize(vector, index);
            
            if constexpr (std::is_same_v<typename Vec::value_type, std::string>) {
                if constexpr (std::is_convertible_v<V, std::string>) {
                    vector[index] = val;
                } else if constexpr (std::is_arithmetic_v<V>) {
                    vector[index] = std::to_string(val);
                }
            } 
            else if constexpr (std::is_assignable_v<typename Vec::value_type&, V>) {
                vector[index] = static_cast<typename Vec::value_type>(val);
            }
        }
    };
} // namespace VTX