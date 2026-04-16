/**
 * @file type_traits.h
 * @brief Metaprogramming utilities (SFINAE/Concepts) for type detection.
 * @details This file defines type traits used by the UniversalDeserializer to detect
 * container types (std::vector, std::map) and check if a custom struct has registered
 * mapping metadata (JsonMapping or FlatBufferMapping).
 */
#pragma once
#include <vector>
#include <map>
#include <type_traits>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace VTX {


    /** * @brief Base struct for std::vector detection (false case).
     * @tparam T The type to check.
     */
    template<typename T>
    struct is_vector : std::false_type {};

    /** * @brief Specialization for std::vector detection (true case).
     * @tparam T The element type.
     * @tparam A The allocator type.
     */
    template<typename T, typename A>
    struct is_vector<std::vector<T, A>> : std::true_type {};

    /** * @brief Helper variable template. True if T is a std::vector.
     * @tparam T The type to check.
     */
    template<typename T>
    inline constexpr bool is_vector_v = is_vector<T>::value;

    /** * @brief Base struct for std::map detection (false case).
     */
    template<typename T> struct is_map : std::false_type {};

    /** * @brief Specialization for std::map detection (true case).
     */
    template<typename K, typename V, typename C, typename A>
    struct is_map<std::map<K, V, C, A>> : std::true_type {};

    /** * @brief Helper variable template. True if T is a std::map.
     */
    template<typename T> inline constexpr bool is_map_v = is_map<T>::value;


    /**
     * @brief Base template for JSON mappings.
     * @details Users must specialize this struct for their custom types to enable deserialization.
     * The specialization must provide a static `GetFields()` method returning a tuple of Fields.
     * @tparam T The type to map.
     */
    template <typename T>
    struct JsonMapping;

    /**
     * @brief SFINAE Meta-function to detect if a type has a defined JsonMapping.
     * @details Checks if `VTX::JsonMapping<T>::GetFields()` is a valid expression.
     * @tparam T The type to inspect.
     * @tparam void Placeholder for SFINAE.
     */
    template<typename T, typename = void>
    struct HasJsonMapping : std::false_type {};

    /**
     * @brief Specialization for types that HAVE a valid JsonMapping.
     */
    template<typename T>
    struct HasJsonMapping<T, std::void_t<decltype(VTX::JsonMapping<T>::GetFields())>> : std::true_type {};

    /** * @brief Helper variable. True if T has a registered JSON mapping.
     */
    template<typename T> inline constexpr bool has_mapping_v = HasJsonMapping<T>::value;

    /**
     * @brief Base template for FlatBuffer mappings.
     * @details Users must specialize this struct to link C++ types with FlatBuffer tables.
     */
    template <typename T> struct FlatBufferMapping;

    /**
     * @brief SFINAE Meta-function to detect if a type has a defined FlatBufferMapping.
     * @details Checks if `VTX::FlatBufferMapping<T>::GetFields()` is a valid expression.
     */
    template<typename T, typename = void>
    struct HasFbMapping : std::false_type {};

    template<typename T>
    struct HasFbMapping<T, std::void_t<decltype(FlatBufferMapping<T>::GetFields())>> : std::true_type {};

    /** 
    * @brief Helper variable. True if T has a registered FlatBuffer mapping.
    */
    template<typename T> inline constexpr bool has_fb_mapping_v = HasFbMapping<T>::value;
}