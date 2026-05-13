/**
* @file loader_base.h
* @author Zenos Interactive
*/

#pragma once
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"

#include <ranges>
#include <type_traits>
#include <string>
#include <string_view>
#include <cstdint>
#include <cstddef>
namespace VTX {
    /**
     * @brief CRTP base for the "source-format -> PropertyContainer" loader family.
     *
     * Shares between GenericFlatBufferLoader / GenericProtobufLoader /
     * GenericNativeLoader (and future siblings) the format-agnostic plumbing:
     *   - LoadField<T>(dest, struct_name, field_name, value)
     *   - LoadBlob
     *   - AppendActorList / AppendSingleEntity
     *   - Internal helpers: StoreValue, EnsureSize, PushToFlatArray, FillFlatArray
     *
     * The Derived loader MUST provide:
     *   - const PropertyAddress* ResolveField(int32_t entity_type_id,
     *                                         const std::string& struct_name,
     *                                         const std::string& field_name) const;
     *   - template <typename T>
     *     void Load(const T& src, PropertyContainer& dest, const std::string& struct_name);
     *
     * Everything else flows through this base via CRTP. No virtuals, no vtable.
     */
    template <typename Derived>
    class GenericLoaderBase {
    public:
        template <typename T>
        void LoadField(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                       const T& value) {
            const PropertyAddress* address = AsDerived().ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!address) {
                return;
            }

            const int32_t idx = address->index;
            switch (address->type_id) {
            case FieldType::String:
                StoreValue(dest.string_properties, idx, value);
                break;
            case FieldType::Int8:
            case FieldType::Int32:
            case FieldType::Enum:
                StoreValue(dest.int32_properties, idx, value);
                break;
            case FieldType::Int64:
                StoreValue(dest.int64_properties, idx, value);
                break;
            case FieldType::Float:
                StoreValue(dest.float_properties, idx, value);
                break;
            case FieldType::Double:
                StoreValue(dest.double_properties, idx, value);
                break;
            case FieldType::Bool:
                StoreValue(dest.bool_properties, idx, value);
                break;
            case FieldType::Vector:
                StoreValue(dest.vector_properties, idx, value);
                break;
            case FieldType::Quat:
                StoreValue(dest.quat_properties, idx, value);
                break;
            case FieldType::Transform:
                StoreValue(dest.transform_properties, idx, value);
                break;
            case FieldType::FloatRange:
                StoreValue(dest.range_properties, idx, value);
                break;
            case FieldType::Struct:
                StoreValue(dest.any_struct_properties, idx, value);
                break;
            case FieldType::None:
            default:
                break;
            }
        }

        void LoadBlob(PropertyContainer& dest, const std::string& struct_name, const std::string& field_name,
                      const void* data, size_t byte_size) {
            if (!data || byte_size == 0) {
                return;
            }
            const PropertyAddress* addr = AsDerived().ResolveField(dest.entity_type_id, struct_name, field_name);
            if (!addr) {
                return;
            }
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < byte_size; ++i) {
                dest.byte_array_properties.PushBack(addr->index, bytes[i]);
            }
        }

        template <typename SrcIterable, typename IdFunc>
        void AppendActorList(Bucket& bucket, const std::string& schema_type, const SrcIterable& src, IdFunc id_func) {
            if constexpr (requires { src.size(); }) {
                bucket.entities.reserve(bucket.entities.size() + src.size());
            }
            for (const auto& item : src) {
                ExtractActor(item, bucket, schema_type, id_func);
            }
        }

        template <typename Src, typename IdFunc>
        void AppendSingleEntity(Bucket& bucket, const std::string& schema_type, const Src& src, IdFunc id_func) {
            ExtractActor(src, bucket, schema_type, id_func);
        }

    protected:
        template <typename It>
        void FillFlatArray(PropertyContainer& dest, FieldType type, int32_t idx, const It& src) {
            using ValueType = std::ranges::range_value_t<It>;
            constexpr bool b_is_string =
                std::is_convertible_v<ValueType, std::string> || std::is_convertible_v<ValueType, std::string_view>;
            for (const auto& it : src) {
                if constexpr (b_is_string) {
                    if (type == FieldType::String)
                        dest.string_arrays.PushBack(idx, std::string(it));
                } else if constexpr (std::is_arithmetic_v<ValueType>) {
                    switch (type) {
                    case FieldType::Int8:
                    case FieldType::Int32:
                    case FieldType::Enum:
                        dest.int32_arrays.PushBack(idx, static_cast<int32_t>(it));
                        break;
                    case FieldType::Int64:
                        dest.int64_arrays.PushBack(idx, static_cast<int64_t>(it));
                        break;
                    case FieldType::Float:
                        dest.float_arrays.PushBack(idx, static_cast<float>(it));
                        break;
                    case FieldType::Double:
                        dest.double_arrays.PushBack(idx, static_cast<double>(it));
                        break;
                    case FieldType::Bool:
                        dest.bool_arrays.PushBack(idx, static_cast<bool>(it));
                        break;
                    case FieldType::String:
                        dest.string_arrays.PushBack(idx, std::to_string(it));
                        break;
                    case FieldType::None:
                    default:
                        break;
                    }
                } else if constexpr (std::is_same_v<ValueType, VTX::Vector>) {
                    if (type == FieldType::Vector)
                        dest.vector_arrays.PushBack(idx, it);
                } else if constexpr (std::is_same_v<ValueType, VTX::Quat>) {
                    if (type == FieldType::Quat)
                        dest.quat_arrays.PushBack(idx, it);
                } else if constexpr (std::is_same_v<ValueType, VTX::Transform>) {
                    if (type == FieldType::Transform)
                        dest.transform_arrays.PushBack(idx, it);
                } else if constexpr (std::is_same_v<ValueType, VTX::FloatRange>) {
                    if (type == FieldType::FloatRange)
                        dest.range_arrays.PushBack(idx, it);
                } else if constexpr (std::is_same_v<ValueType, VTX::PropertyContainer>) {
                    if (type == FieldType::Struct)
                        dest.any_struct_arrays.PushBack(idx, it);
                }
            }
        }

        void PushToFlatArray(PropertyContainer& dest, FieldType type, int32_t idx,
                             const PropertyContainer& temp) const {
            switch (type) {
            case FieldType::Vector:
                if (!temp.vector_properties.empty())
                    dest.vector_arrays.PushBack(idx, temp.vector_properties[0]);
                break;
            case FieldType::Quat:
                if (!temp.quat_properties.empty())
                    dest.quat_arrays.PushBack(idx, temp.quat_properties[0]);
                break;
            case FieldType::Transform:
                if (!temp.transform_properties.empty())
                    dest.transform_arrays.PushBack(idx, temp.transform_properties[0]);
                break;
            case FieldType::FloatRange:
                if (!temp.range_properties.empty())
                    dest.range_arrays.PushBack(idx, temp.range_properties[0]);
                break;
            case FieldType::Struct:
                if (temp.entity_type_id != -1)
                    dest.any_struct_arrays.PushBack(idx, temp);
                break;
            default:
                break;
            }
        }

        template <typename Vec>
        static void EnsureSize(Vec& v, size_t index) {
            if (v.size() <= index)
                v.resize(index + 1);
        }

        template <typename Vec, typename V>
        static void StoreValue(Vec& vec, size_t index, const V& val) {
            EnsureSize(vec, index);
            using VecValueT = typename Vec::value_type;
            if constexpr (std::is_same_v<VecValueT, std::string>) {
                if constexpr (std::is_convertible_v<V, std::string>)
                    vec[index] = val;
                else if constexpr (std::is_arithmetic_v<V>)
                    vec[index] = std::to_string(val);
            } else if constexpr (std::is_assignable_v<VecValueT&, V>) {
                vec[index] = static_cast<VecValueT>(val);
            }
        }

    private:
        Derived& AsDerived() { return static_cast<Derived&>(*this); }
        const Derived& AsDerived() const { return static_cast<const Derived&>(*this); }

        template <typename Src, typename IdFunc>
        void ExtractActor(const Src& src, Bucket& bucket, const std::string& schema_type, IdFunc id_func) {
            PropertyContainer& entity = bucket.entities.emplace_back();
            AsDerived().Load(src, entity, schema_type);
            bucket.unique_ids.push_back(id_func(src));
        }
    };
} // namespace VTX
