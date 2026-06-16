#pragma once
#include "vtx/common/vtx_concepts.h"
#include "vtx/common/vtx_diagnostics.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/readers/schema_reader/schema_enums.h"
#include <string>
#include <type_traits>

namespace VTX {
    struct PropertyMetadata {
        std::string name;
        VTX::FieldType type;
    };

    class MapView;

    class EntityView {
    private:
        const PropertyContainer* data_ = nullptr;

    public:
        EntityView()
            : data_(nullptr) {}
        explicit EntityView(const PropertyContainer& data)
            : data_(&data) {}

        template <typename T>
        static constexpr auto GetContainerMember() {
            if constexpr (std::same_as<T, bool>)
                return &PropertyContainer::bool_properties;
            else if constexpr (std::same_as<T, int32_t>)
                return &PropertyContainer::int32_properties;
            else if constexpr (std::same_as<T, int64_t>)
                return &PropertyContainer::int64_properties;
            else if constexpr (std::same_as<T, float>)
                return &PropertyContainer::float_properties;
            else if constexpr (std::same_as<T, double>)
                return &PropertyContainer::double_properties;
            else if constexpr (std::same_as<T, std::string>)
                return &PropertyContainer::string_properties;
            else if constexpr (std::same_as<T, VTX::Vector>)
                return &PropertyContainer::vector_properties;
            else if constexpr (std::same_as<T, VTX::Quat>)
                return &PropertyContainer::quat_properties;
            else if constexpr (std::same_as<T, VTX::Transform>)
                return &PropertyContainer::transform_properties;
            else if constexpr (std::same_as<T, VTX::FloatRange>)
                return &PropertyContainer::range_properties;
            else if constexpr (std::same_as<T, EntityView>)
                return &PropertyContainer::any_struct_properties;
            else
                static_assert(std::same_as<T, void>, "type not supported in EntityView");
        }

        template <typename U>
        using ScalarRetType = std::conditional_t<std::is_same_v<U, bool>, bool, const U&>;

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

        template <VtxScalarType T>
        VtxResult<T> TryGet(PropertyKey<T> key) const {
            if (!data_) {
                VtxError error;
                error.code = VtxErrorCode::InvalidArgument;
                error.message = "entity view is empty";
                error.source_api = "TryGet";
                return VtxResult<T>::Failure(error);
            }
            if (!key.IsValid()) {
                VtxError error;
                error.code = VtxErrorCode::InvalidArgument;
                error.message = "invalid property key";
                error.source_api = "TryGet";
                return VtxResult<T>::Failure(error);
            }
            constexpr auto MemberPtr = GetContainerMember<T>();
            const auto& values = data_->*MemberPtr;
            if (static_cast<size_t>(key.index) >= values.size()) {
                VtxError error;
                error.code = VtxErrorCode::FieldIndexOutOfRange;
                error.message = "property index " + std::to_string(key.index) + " is out of range (size " +
                                std::to_string(values.size()) + ")";
                error.source_api = "TryGet";
                return VtxResult<T>::Failure(error);
            }
            return VtxResult<T>::Success(values[key.index]);
        }

        template <VtxArrayType T>
        auto GetArray(PropertyKey<T> key) const {
            constexpr auto MemberPtr = GetArrayContainerMember<T>();

            using ExactSpanType = decltype((data_->*MemberPtr).GetSubArray(0));

            if (!data_ || !key.IsValid()) {
                return ExactSpanType {};
            }

            return (data_->*MemberPtr).GetSubArray(key.index);
        }

        EntityView GetView(PropertyKey<EntityView> key) const {
            if (!data_ || !key.IsValid() || static_cast<size_t>(key.index) >= data_->any_struct_properties.size())
                return EntityView {};
            return EntityView(data_->any_struct_properties[key.index]);
        }

        std::span<const PropertyContainer> GetViewArray(PropertyKey<std::span<const PropertyContainer>> key) const {
            if (!data_ || !key.IsValid())
                return {};
            return data_->any_struct_arrays.GetSubArray(key.index);
        }

        MapView GetMap(PropertyKey<MapView> key) const;

        template <typename T>
        static constexpr auto GetArrayContainerMember() {
            if constexpr (std::is_same_v<T, int32_t>)
                return &PropertyContainer::int32_arrays;
            else if constexpr (std::is_same_v<T, int64_t>)
                return &PropertyContainer::int64_arrays;
            else if constexpr (std::is_same_v<T, float>)
                return &PropertyContainer::float_arrays;
            else if constexpr (std::is_same_v<T, double>)
                return &PropertyContainer::double_arrays;
            else if constexpr (std::is_same_v<T, std::string>)
                return &PropertyContainer::string_arrays;
            else if constexpr (std::is_same_v<T, VTX::Vector>)
                return &PropertyContainer::vector_arrays;
            else if constexpr (std::is_same_v<T, VTX::Quat>)
                return &PropertyContainer::quat_arrays;
            else if constexpr (std::is_same_v<T, VTX::Transform>)
                return &PropertyContainer::transform_arrays;
            else if constexpr (std::is_same_v<T, VTX::FloatRange>)
                return &PropertyContainer::range_arrays;
            else if constexpr (std::is_same_v<T, bool>)
                return &PropertyContainer::bool_arrays;
            else if constexpr (std::is_same_v<T, uint8_t>)
                return &PropertyContainer::byte_array_properties;
            else if constexpr (std::is_same_v<T, VTX::PropertyContainer>)
                return &PropertyContainer::any_struct_arrays;
            else
                static_assert(std::same_as<T, void>, "type not supported in EntityView");
        }
    };

    // Read-only view over a Map property (parallel keys[] + value containers).
    class MapView {
    private:
        const MapContainer* data_ = nullptr;

    public:
        MapView() = default;
        explicit MapView(const MapContainer& data)
            : data_(&data) {}

        bool IsValid() const { return data_ != nullptr; }
        size_t Size() const { return data_ ? data_->keys.size() : 0; }
        bool Empty() const { return Size() == 0; }

        const std::vector<std::string>& Keys() const {
            static const std::vector<std::string> kEmpty;
            return data_ ? data_->keys : kEmpty;
        }

        bool Contains(std::string_view key) const {
            if (data_) {
                for (const auto& k : data_->keys) {
                    if (k == key)
                        return true;
                }
            }
            return false;
        }

        EntityView At(std::string_view key) const {
            if (data_) {
                for (size_t i = 0; i < data_->keys.size() && i < data_->values.size(); ++i) {
                    if (data_->keys[i] == key)
                        return EntityView(data_->values[i]);
                }
            }
            return EntityView {};
        }

        const std::string& KeyAt(size_t index) const {
            static const std::string kEmpty;
            return (data_ && index < data_->keys.size()) ? data_->keys[index] : kEmpty;
        }

        EntityView ValueAt(size_t index) const {
            if (data_ && index < data_->values.size())
                return EntityView(data_->values[index]);
            return EntityView {};
        }
    };

    inline MapView EntityView::GetMap(PropertyKey<MapView> key) const {
        if (!data_ || !key.IsValid() || static_cast<size_t>(key.index) >= data_->map_properties.size())
            return MapView {};
        return MapView(data_->map_properties[key.index]);
    }

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

        template <typename T>
            requires SchemaAdaptable<T>
        void Initialize(const std::unique_ptr<T>& schema) {
            //if (schema) SchemaAdapter<T>::BuildCache(*schema, cache_);
        }

        void InitializeFromCache(const PropertyAddressCache& prebuilt_cache) { cache_ = prebuilt_cache; }

        template <VtxScalarType T>
        PropertyKey<T> Get(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == GetExpectedFieldType<T>() &&
                        addr.container_type == VTX::FieldContainerType::None) {
                        return PropertyKey<T> {static_cast<int32_t>(addr.index)};
                    } else {
                        VTX_ERROR("Type mismatch for struct ID {}. Property: {}", structId, propName);
                    }
                }
            }
            return PropertyKey<T> {-1};
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
            return PropertyKey<T> {-1};
        }

        template <VtxScalarType T>
        VtxResult<PropertyKey<T>> TryResolve(int32_t structId, const std::string& propName) const {
            const auto structIt = cache_.structs.find(structId);
            if (structIt == cache_.structs.end()) {
                VtxError error;
                error.code = VtxErrorCode::NotFound;
                error.message = "struct id " + std::to_string(structId) + " not found in schema";
                error.field_path = propName;
                error.source_api = "TryResolve";
                return VtxResult<PropertyKey<T>>::Failure(error);
            }
            const StructSchemaCache& struct_cache = structIt->second;
            const auto propIt = struct_cache.properties.find(propName);
            if (propIt == struct_cache.properties.end()) {
                VtxError error;
                error.code = VtxErrorCode::NotFound;
                error.message = "property '" + propName + "' not found in struct '" + struct_cache.name + "'";
                error.entity_type = struct_cache.name;
                error.field_path = struct_cache.name + "." + propName;
                error.source_api = "TryResolve";
                return VtxResult<PropertyKey<T>>::Failure(error);
            }

            const PropertyAddress& addr = propIt->second;
            const std::string field_path = struct_cache.name + "." + propName;
            if (addr.container_type != VTX::FieldContainerType::None) {
                VtxError error;
                error.code = VtxErrorCode::ContainerMismatch;
                error.message = "property '" + field_path + "' is not a scalar field";
                error.entity_type = struct_cache.name;
                error.field_path = field_path;
                error.expected_container = std::string(VTX::ToString(addr.container_type));
                error.provided_container = "None";
                error.source_api = "TryResolve";
                return VtxResult<PropertyKey<T>>::Failure(error);
            }

            const VTX::FieldType expected = addr.type_id;
            const VTX::FieldType provided = GetExpectedFieldType<T>();
            if (expected != provided) {
                VtxError error;
                error.code = VtxErrorCode::TypeMismatch;
                error.message = "property '" + field_path + "' is " + std::string(VTX::ToString(expected)) +
                                " but was requested as " + std::string(VTX::ToString(provided));
                error.entity_type = struct_cache.name;
                error.field_path = field_path;
                error.expected_type = std::string(VTX::ToString(expected));
                error.provided_type = std::string(VTX::ToString(provided));
                error.expected_container = "None";
                error.provided_container = "None";
                error.source_api = "TryResolve";
                return VtxResult<PropertyKey<T>>::Failure(error);
            }

            return VtxResult<PropertyKey<T>>::Success(PropertyKey<T> {static_cast<int32_t>(addr.index)});
        }

        template <VtxScalarType T>
        VtxResult<PropertyKey<T>> TryResolve(const std::string& structName, const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId == -1) {
                VtxError error;
                error.code = VtxErrorCode::NotFound;
                error.message = "struct '" + structName + "' not found in schema";
                error.entity_type = structName;
                error.field_path = structName + "." + propName;
                error.source_api = "TryResolve";
                return VtxResult<PropertyKey<T>>::Failure(error);
            }
            return TryResolve<T>(structId, propName);
        }

        template <VtxScalarType T, typename EnumType, typename std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
        VtxResult<PropertyKey<T>> TryResolve(EnumType structEnum, const std::string& propName) const {
            return TryResolve<T>(static_cast<int32_t>(structEnum), propName);
        }

        template <VtxArrayType T>
        PropertyKey<T> GetArray(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == GetExpectedFieldType<T>() &&
                        addr.container_type == VTX::FieldContainerType::Array) {
                        return PropertyKey<T> {static_cast<int32_t>(addr.index)};
                    }
                }
            }
            return PropertyKey<T> {-1};
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
            return PropertyKey<T> {-1};
        }

        PropertyKey<VTX::EntityView> GetViewKey(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == VTX::FieldType::Struct &&
                        addr.container_type == VTX::FieldContainerType::None) {
                        return PropertyKey<VTX::EntityView> {static_cast<int32_t>(addr.index)};
                    } else {
                        VTX_ERROR("Type mismatch for struct ID {}. Property: {}", structId, propName);
                    }
                }
            }
            return PropertyKey<VTX::EntityView> {-1};
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
            return PropertyKey<VTX::EntityView> {-1};
        }


        PropertyKey<std::span<const VTX::PropertyContainer>> GetViewArrayKey(int32_t structId,
                                                                             const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == VTX::FieldType::Struct &&
                        addr.container_type == VTX::FieldContainerType::Array) {
                        return PropertyKey<std::span<const VTX::PropertyContainer>> {static_cast<int32_t>(addr.index)};
                    } else {
                        VTX_ERROR("Type mismatch for struct ID {}. Property: {}", structId, propName);
                    }
                }
            }
            return PropertyKey<std::span<const VTX::PropertyContainer>> {-1};
        }

        template <typename EnumType, typename std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
        PropertyKey<std::span<const VTX::PropertyContainer>> GetViewArrayKey(EnumType structEnum,
                                                                             const std::string& propName) const {
            return GetViewArrayKey(static_cast<int32_t>(structEnum), propName);
        }

        PropertyKey<std::span<const VTX::PropertyContainer>> GetViewArrayKey(const std::string& structName,
                                                                             const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return GetViewArrayKey(structId, propName);
            }
            return PropertyKey<std::span<const VTX::PropertyContainer>> {-1};
        }


        PropertyKey<VTX::MapView> GetMapKey(int32_t structId, const std::string& propName) const {
            auto structIt = cache_.structs.find(structId);
            if (structIt != cache_.structs.end()) {
                auto propIt = structIt->second.properties.find(propName);
                if (propIt != structIt->second.properties.end()) {
                    const PropertyAddress& addr = propIt->second;
                    if (addr.type_id == VTX::FieldType::Struct && addr.container_type == VTX::FieldContainerType::Map) {
                        return PropertyKey<VTX::MapView> {static_cast<int32_t>(addr.index)};
                    } else {
                        VTX_ERROR("Type mismatch for struct ID {}. Property: {}", structId, propName);
                    }
                }
            }
            return PropertyKey<VTX::MapView> {-1};
        }

        template <typename EnumType, typename std::enable_if_t<std::is_enum_v<EnumType>, int> = 0>
        PropertyKey<VTX::MapView> GetMapKey(EnumType structEnum, const std::string& propName) const {
            return GetMapKey(static_cast<int32_t>(structEnum), propName);
        }

        PropertyKey<VTX::MapView> GetMapKey(const std::string& structName, const std::string& propName) const {
            const int32_t structId = FindStructId(structName);
            if (structId != -1) {
                return GetMapKey(structId, propName);
            }
            return PropertyKey<VTX::MapView> {-1};
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
            if (it != cache_.structs.end())
                return it->second.properties.contains(propName);
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
} // namespace VTX
