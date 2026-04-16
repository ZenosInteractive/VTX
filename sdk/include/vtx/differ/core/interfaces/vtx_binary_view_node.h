#pragma once

namespace VtxDiff {

    template<typename T>
    concept CBinaryNodeView = requires(const T& View, const FieldDesc& Fd, size_t Index, const std::string& Str)
    {
        { View.IsValid() } -> std::same_as<bool>;
        //{ View.Reset() } -> std::same_as<void>;
        
        { View.EnumerateFields() } -> std::same_as<std::span<const FieldDesc>>;
        { View.GetFieldBytes(Fd) } -> std::same_as<std::span<const std::byte>>;
        { View.GetScalarFieldString(Str) } -> std::same_as<std::string>;
        
        { View.GetArraySize(Fd) } -> std::same_as<size_t>;
        { View.GetArrayElementBytes(Fd, Index) } -> std::same_as<std::span<const std::byte>>;
        { View.GetSubArrayBytes(Fd, Index) } -> std::same_as<std::span<const std::byte>>;
        
        { View.GetMapSize(Fd) } -> std::same_as<size_t>;
        { View.GetMapKey(Fd, Index) } -> std::same_as<std::string>;
        
        { View.GetNestedStruct(Fd) } -> std::same_as<T>;
        { View.GetArrayElementAsStruct(Fd, Index) } -> std::same_as<T>;
        { View.GetMapValueAsStruct(Fd, Index) } -> std::same_as<T>;
        { View.GetFieldByName(Str) } -> std::same_as<T>;
    };
} // namespace VtxDiff
