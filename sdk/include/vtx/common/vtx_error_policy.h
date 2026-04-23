#pragma once
#include <iostream>
#include <string>
/**
 * @file vtx_error_policy.h
 * @brief Generic error policies for reading operations
 * @author Zenos Interactive
 */
namespace VTX {

    /**
     * @struct StrictErrorPolicy
     * @brief A policy that enforces strict data validation.
     * @details Throws `std::runtime_error` immediately if a mandatory key is missing or types do not match.
     * Use this for critical data where corruption is unacceptable (e.g., loading a save file).
     */
    struct StrictErrorPolicy {
        /**
         * @brief Called when a required JSON key is missing.
         * @param key The name of the missing key.
         * @throw std::runtime_error Always throws.
         */
        static void OnMissingKey(const std::string& key) { throw std::runtime_error("Missing mandatory key : " + key); }

        /**
         * @brief Called when the data type in JSON does not match the C++ target type.
         * @param key The name of the field with the mismatch.
         * @throw std::runtime_error Always throws.
         */
        static void OnTypeMismatch(const std::string& key) {
            throw std::runtime_error("Type doesnt match the key: " + key);
        }
    };

    /**
     * @struct LenientErrorPolicy
     * @brief A policy that logs issues but attempts to continue.
     * @details Prints warnings to `std::cerr` when errors occur. The deserializer will typically
     * leave the C++ variable with its default value. Useful for debugging or loading partial data.
     */
    struct LenientErrorPolicy {
        /**
         * @brief Logs a warning to stderr about a missing key.
         * @param key The name of the missing key.
         */
        static void OnMissingKey(const std::string& key) {
            std::cerr << "[WARNING] Ignored missing key: " << key << std::endl;
        }

        /**
         * @brief Logs a warning to stderr about a type mismatch.
         * @param key The name of the field with the mismatch.
         */
        static void OnTypeMismatch(const std::string& key) {
            std::cerr << "[WARNING] Wrong type_max_indices ignored: " << key << std::endl;
        }
    };

    /**
     * @struct SilentErrorPolicy
     * @brief A high-performance policy that ignores all errors.
     * @details No output is generated, and no exceptions are thrown. This is ideal for
     * production builds where performance is critical and data integrity is guaranteed elsewhere.
     */
    struct SilentErrorPolicy {
        /** @brief No-op implementation. */
        static void OnMissingKey(const std::string& key) {}
        /** @brief No-op implementation. */
        static void OnTypeMismatch(const std::string& key) {}
    };

    // ==========================================================
    // 2. NAMING CONVENTION POLICIES
    // ==========================================================

    /**
     * @struct ExactNamingPolicy
     * @brief A policy that requires exact string matching for keys.
     * @details "PlayerHealth" in C++ will look for "PlayerHealth" in JSON.
     * This is the fastest naming policy as it performs no string manipulation.
     */
    struct ExactNamingPolicy {
        /**
         * @brief Returns the key exactly as provided.
         * @param key The source key name.
         * @return The same key string.
         */
        static std::string Transform(const std::string& key) { return key; }
    };

    /**
     * @struct LowercaseNamingPolicy
     * @brief A policy that performs case-insensitive matching (via lowercase).
     * @details Converts the C++ field name to lowercase before lookup.
     * Example: "PlayerHealth" in C++ will find "playerhealth" in JSON.
     */
    struct LowercaseNamingPolicy {
        /**
         * @brief Transforms the input string to lowercase.
         * @param key The source key name (passed by value to allow modification).
         * @return The lowercase version of the key.
         */
        static std::string Transform(std::string key) {
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            return key;
        }
    };
} // namespace VTX