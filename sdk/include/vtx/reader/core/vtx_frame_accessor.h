#pragma once
#include "vtx/common/vtx_concepts.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_property_cache.h"
#include <type_traits> 

namespace VTX
{
    struct PropertyMetadata {
        std::string name;
        VTX::FieldType type;
    };

    class EntityView {
    private:
        const PropertyContainer* data_ = nullptr;

    public:
        EntityView() : data_(nullptr) {}
        explicit EntityView(const PropertyContainer& data) : data_(&data) {}

        template <typename T>
        static constexpr auto GetContainerMember() {
            if constexpr (std::same_as<T, bool>) return &PropertyContainer::bool_properties;
            else if constexpr (std::same_as<T, int32_t>) return &PropertyContainer::int32_properties;
            else if constexpr (std::same_as<T, int64_t>) return &PropertyContainer::int64_properties;
            else if constexpr (std::same_as<T, float>) return &PropertyContainer::float_properties;
            else if constexpr (std::same_as<T, double>) return &PropertyContainer::double_properties;
            else if constexpr (std::same_as<T, std::string>) return &PropertyContainer::string_properties;
            else if constexpr (std::same_as<T, VTX::Vector>) return &PropertyContainer::vector_properties;
            else if constexpr (std::same_as<T, VTX::Quat>) return &PropertyContainer::quat_properties;
            else if constexpr (std::same_as<T, VTX::Transform>) return &PropertyContainer::transform_properties;
            else if constexpr (std::same_as<T, VTX::FloatRange>) return &PropertyContainer::range_properties;
            else if constexpr (std::same_as<T, EntityView>) return &PropertyContainer::any_struct_properties;
            else static_assert(std::same_as<T, void>, "type not supported in EntityView");
        }
        
        template <typename U> using ScalarRetType = std::conditional_t<std::is_same_v<U, bool>, bool, const U&>;

        template <VtxScalarType T>
        ScalarRetType<T> Get(PropertyKey<T> key) const {
            if (!data_ || !key.IsValid()) {
                static const T default_val = {}; 
                return default_val;
            }
            constexpr auto MemberPtr = GetContainerMember<T>();
            const auto& values = data_->*MemberPtr;
            if (static_cast<size_t>(key.index) >= values.size()) {
                static const T default_val = {};
                return default_val;
            }
            return values[key.index];
        }

        template <VtxArrayType T>
        auto GetArray(PropertyKey<T> key) const {
            constexpr auto MemberPtr = GetArrayContainerMember<T>();
            
            using ExactSpanType = decltype((data_->*MemberPtr).GetSubArray(0));
            
            if (!data_ || !key.IsValid()) {
                return ExactSpanType{}; 
            }
            
            return (data_->*MemberPtr).GetSubArray(key.index); 
        }
        
        EntityView GetView(PropertyKey<EntityView> key) const {
            if (!data_ || !key.IsValid() || static_cast<size_t>(key.index) >= data_->any_struct_properties.size()) return EntityView{}; 
            return EntityView(data_->any_struct_properties[key.index]);
        }

        std::span<const PropertyContainer> GetViewArray(PropertyKey<std::span<const PropertyContainer>> key) const {
            if (!data_ || !key.IsValid()) return {};
            return data_->any_struct_arrays.GetSubArray(key.index);
        }

        template <typename T>
        static constexpr auto GetArrayContainerMember() {
            if constexpr (std::is_same_v<T, int32_t>) return &PropertyContainer::int32_arrays;
            else if constexpr (std::is_same_v<T, int64_t>) return &PropertyContainer::int64_arrays;
            else if constexpr (std::is_same_v<T, float>) return &PropertyContainer::float_arrays;
            else if constexpr (std::is_same_v<T, double>) return &PropertyContainer::double_arrays;
            else if constexpr (std::is_same_v<T, std::string>) return &PropertyContainer::string_arrays;
            else if constexpr (std::is_same_v<T, VTX::Vector>) return &PropertyContainer::vector_arrays;
            else if constexpr (std::is_same_v<T, VTX::Quat>) return &PropertyContainer::quat_arrays;
            else if constexpr (std::is_same_v<T, VTX::Transform>) return &PropertyContainer::transform_arrays;
            else if constexpr (std::is_same_v<T, VTX::FloatRange>) return &PropertyContainer::range_arrays;
            else if constexpr (std::is_same_v<T, bool>) return &PropertyContainer::bool_arrays;
            else if constexpr (std::is_same_v<T, uint8_t>) return &PropertyContainer::byte_array_properties;
            else if constexpr (std::is_same_v<T, VTX::PropertyContainer>) return &PropertyContainer::any_struct_arrays;
            else static_assert(std::same_as<T, void>, "type not supported in EntityView");
        }
        
    };

    class FrameAccessor {
    private:
        PropertyAddressCache cache_;
        std::unordered_map<PropertyKey<EntityView>, PropertyAddressCache> data_;

        int32_t FindStructId(const std::string& structName) const {
            const auto it = cache_.name_to_id.find(structName);
            if (it != cache_.name_to_id.end()) {
                return it->second;
            }
            return -1;
        }
        
    public:
        void Initialize(const SchemaAdaptable auto& schema) {
            SchemaAdapter<std::remove_cvref_t<decltype(schema)>>::BuildCache(schema, cache_);
        }

        template <typename T> requires SchemaAdaptable<T>
        void Initialize(const std::unique_ptr<T>& schema) {
            //if (schema) SchemaAdapter<T>::BuildCache(*schema, cache_);
        }
    
        void InitializeFromCache(const PropertyAddressCache& prebuilt_cache) {
            cache_ = prebuilt_cache; 
        }
        
        template <VtxScalarType T>
        PropertyKey<T> Get(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == GetExpectedFieldType<T>() && addr.container_type == VTX::FieldContainerType::None) {
                        return PropertyKey<T>{ static_cast<int32_t>(addr.index) };
                    } else {
                        VTX_ERROR("Type mismatch for struct ID {}. Property: {}", structId, propName);
                    }
                }
            }
            return PropertyKey<T>{ -1 };
        }

        template <VtxScalarType T, typename EnumType, typename std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
        PropertyKey<T> Get(EnumType structEnum, const std::string& propName) const {
            return Get<T>(static_cast<int32_t>(structEnum), propName);
        }

        template <VtxScalarType T>
        PropertyKey<T> Get(const std::string& structName, const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return Get<T>(structId, propName);
            }
            return PropertyKey<T>{ -1 };
        }
        
        template <VtxArrayType T>
        PropertyKey<T> GetArray(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == GetExpectedFieldType<T>() && addr.container_type == VTX::FieldContainerType::Array) {
                        return PropertyKey<T>{ static_cast<int32_t>(addr.index) };
                    }
                }
            }
            return PropertyKey<T>{ -1 };
        }

        template <VtxArrayType T, typename EnumType, typename std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
        PropertyKey<T> GetArray(EnumType structEnum, const std::string& propName) const {
            return GetArray<T>(static_cast<int32_t>(structEnum), propName);
        }

        template <VtxArrayType T>
        PropertyKey<T> GetArray(const std::string& structName, const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return GetArray<T>(structId, propName);
            }
            return PropertyKey<T>{ -1 };
        }
                
        PropertyKey<VTX::EntityView> GetViewKey(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == VTX::FieldType::Struct && addr.container_type == VTX::FieldContainerType::None) {
                        return PropertyKey<VTX::EntityView>{ static_cast<int32_t>(addr.index) };
                    } else {
                        VTX_ERROR("Type mismatch for struct ID {}. Property: {}", structId, propName);
                    }
                }
            }
            return PropertyKey<VTX::EntityView>{ -1 };
        }

        template <typename EnumType, typename std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
        PropertyKey<VTX::EntityView> GetViewKey(EnumType structEnum, const std::string& propName) const {
            return GetViewKey(static_cast<int32_t>(structEnum), propName);
        }

        PropertyKey<VTX::EntityView> GetViewKey(const std::string& structName, const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return GetViewKey(structId, propName);
            }
            return PropertyKey<VTX::EntityView>{ -1 };
        }
        
        
        PropertyKey<std::span<const VTX::PropertyContainer>> GetViewArrayKey(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == VTX::FieldType::Struct && addr.container_type == VTX::FieldContainerType::Array) {
                        return PropertyKey<std::span<const VTX::PropertyContainer>>{ static_cast<int32_t>(addr.index) };
                    } else {
                        VTX_ERROR("Type mismatch for struct ID {}. Property: {}", structId, propName);
                    }
                }
            }
            return PropertyKey<std::span<const VTX::PropertyContainer>>{ -1 };
        }

        template <typename EnumType, typename std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
        PropertyKey<std::span<const VTX::PropertyContainer>> GetViewArrayKey(EnumType structEnum, const std::string& propName) const {
            return GetViewArrayKey(static_cast<int32_t>(structEnum), propName);
        }

        PropertyKey<std::span<const VTX::PropertyContainer>> GetViewArrayKey(const std::string& structName, const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return GetViewArrayKey(structId, propName);
            }
            return PropertyKey<std::span<const VTX::PropertyContainer>>{ -1 };
        }
        

        std::vector<std::string> GetAvailableStructNames() const {
            std::vector<std::string> names;
            names.reserve(cache_.name_to_id.size()); 
            for (const auto& [name, id] : cache_.name_to_id) {
                names.push_back(name);
            }
            return names;
        }

        std::vector<PropertyMetadata> GetPropertiesForStruct(int32_t structId) const {
            std::vector<PropertyMetadata> props;
            auto it = cache_.structs.find(structId);
            if (it != cache_.structs.end()) {
                props.reserve(it->second.properties.size());
                for (const auto& [propName, addr] : it->second.properties) {
                    props.push_back({propName, addr.type_id});
                }
            }
            return props;
        }

        std::vector<PropertyMetadata> GetPropertiesForStruct(const std::string& structName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return GetPropertiesForStruct(structId);
            }
            return {};
        }

        bool HasProperty(int32_t structId, const std::string& propName) const {
            auto it = cache_.structs.find(structId);
            if (it != cache_.structs.end()) return it->second.properties.contains(propName);
            return false;
        }

        bool HasProperty(const std::string& structName, const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return HasProperty(structId, propName);
            }
            return false;
        }
    };
}
