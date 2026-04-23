
/**
 * @file json_adapter.h
 * 
 * @brief Adapter for the nlohmann::json library.
 * 
 * @details This class wraps a nlohmann::json node to provide a unified interface
 * for the VTX universal deserialization system. It allows the core logic to read
 * JSON data without depending directly on the specific JSON library API.
 * 
 * @author Zenos Interactive
 */

#pragma once
#include <nlohmann/json.hpp>

namespace VTX {

    /**
     * @class JsonAdapter
     * @brief A lightweight wrapper around a JSON node.
     * @details Implements the "Read Adapter" concept required by UniversalDeserializer.
     * It handles navigation (Child/Element access) and value extraction.
     */
    class JsonAdapter {
        const nlohmann::json* node = nullptr;

    public:
        JsonAdapter() {}
        /**
             * @brief Explicit constructor.
             * @param j Constant reference to the JSON node to wrap.
             */
        JsonAdapter(const nlohmann::json& j)
            : node(&j) {}

        /**
             * @brief Checks if a specific key exists in the current object.
             * @param key The key string to search for.
             * @return true If the node is an object and contains the key.
             * @return false If the node is not an object or the key is missing.
             */
        bool HasKey(const std::string& key) const {
            if (!node->is_object()) {
                return false;
            }
            return node->contains(key);
        }


        /**
             * @brief Extracts a value of the specified type.
             * @tparam T The target type (int, float, std::string, bool, etc.).
             * @return T The converted value from the JSON node.
             * @throws nlohmann::json::type_error If the conversion is invalid.
             */
        template <typename T>
        T GetValue() const {
            return node->get<T>();
        }

        /**
             * @brief Retrieves a child adapter for a specific key.
             * @param key The key of the child node.
             * @return JsonAdapter A new adapter instance pointing to the child node.
             * @throws nlohmann::json::out_of_range If the key does not exist.
             */
        JsonAdapter GetChild(const std::string& key) const {
            if (!IsMap())
                return JsonAdapter();
            if (!node->contains(key))
                return JsonAdapter();
            return JsonAdapter(node->at(key));
        }

        /**
             * @brief Checks if the current node represents an array.
             * @return true If it is an array, false otherwise.
             */
        bool IsArray() const { return node->is_array(); }

        /**
             * @brief Gets the number of elements in the array.
             * @return size_t Number of elements.
             */
        size_t Size() const { return node->size(); }

        /**
             * @brief Retrieves an adapter for a specific element in an array.
             * @param index The zero-based index of the element.
             * @return JsonAdapter Adapter for the element at the given index.
             */
        JsonAdapter GetElement(size_t index) const {
            if (!node || !node->is_array()) {
                throw std::runtime_error("JsonAdapter: GetElement attempted to access a none array object");
            }
            // Usamos .at() que es más seguro (lanza excepción si el índice no existe)
            return JsonAdapter(node->at(index));
        }

        /**
             * @brief Checks if the current node represents a map/object.
             * @return true If it is an object, false otherwise.
             */
        bool IsMap() const { return node->is_object(); }

        /**
             * @brief Retrieves all keys from the current JSON object.
             * @return std::vector<std::string> A list of all keys.
             */
        std::vector<std::string> GetKeys() const {
            std::vector<std::string> keys;
            for (auto& [key, val] : node->items()) {
                keys.push_back(key);
            }
            return keys;
        }

        /**
             * @brief Converts a string key from the JSON map to a specific target type.
             * @details Useful for deserializing `std::map<int, T>` or `std::map<Enum, T>` where
             * JSON only supports string keys.
             * * @tparam KeyType The target type for the key (std::string, int, enum).
             * @param key_str The key as a string.
             * @return KeyType The parsed key value.
             */
        template <typename KeyType>
        KeyType ParseKey(const std::string& key_str) const {
            if constexpr (std::is_same_v<KeyType, std::string>) {
                return key_str;
            } else if constexpr (std::is_integral_v<KeyType>) {
                return static_cast<KeyType>(std::stoll(key_str));
            } else if constexpr (std::is_enum_v<KeyType>) {
                // Attempt to deserialize the enum directly from the string using nlohmann
                return nlohmann::json(key_str).get<KeyType>();
            } else {
                static_assert(std::is_void_v<KeyType>, "Key type_max_indices not supported by JsonReader.");
                return KeyType {};
            }
        }
    };
} // namespace VTX