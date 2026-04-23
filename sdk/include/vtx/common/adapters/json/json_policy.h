/*
* @file json_frame_policy.h
* 
* @brief Definitions for mapping JSON fields to C++ class members.
* 
* @details This file provides the `Field` structure used to create static reflection tables
* for C++ structs, allowing them to be deserialized from JSON data automatically.
* 
* @author Zenos Interactive
*/

#pragma once

namespace VTX {

    /**
     * @struct Field
     * @brief Represents a single mapped field linking a JSON key to a C++ member variable.
     * @tparam Class The class or struct type that owns the member.
     * @tparam Type The data type of the member variable.
     */
    template <typename Class, typename Type>
    struct Field {
        const char* json_key;
        Type Class::*member_ptr;
    };


    /**
     * @brief Helper function to create a Field object with automatic type deduction.
     * @details Simplifies the syntax for defining mappings.
     * Usage: `MakeField("health", &Player::health)`
     * * @tparam Class Automatically deduced class type.
     * @tparam Type Automatically deduced member type.
     * @param name The JSON key name.
     * @param ptr The pointer to the member variable.
     * @return Field<Class, Type> A initialized Field structure.
     */
    template <typename Class, typename Type>
    constexpr auto MakeField(const char* name, Type Class::*ptr) {
        return Field<Class, Type> {name, ptr};
    }
} // namespace VTX