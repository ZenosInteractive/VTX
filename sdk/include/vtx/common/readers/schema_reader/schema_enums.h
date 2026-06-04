/**
 * @file schema_enums.h
 * @brief Single source of truth for parsing/serializing schema enum strings.
 *
 * @details The schema JSON carries field types, container kinds and map key
 * types as strings (e.g. "Int32", "Array", "Map"). Both the parser
 * (SchemaRegistry) and the validator (SchemaValidator) need to agree on which
 * strings are valid and how they map to the FieldType / FieldContainerType
 * enums. Centralizing that knowledge here keeps the two in lock-step (DRY) and
 * makes "what is a valid type/container/key/default" independently testable.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "game_schema_types.h"

namespace VTX {

    /**
     * @brief Parse a JSON "typeId" string into a FieldType.
     * @details Case-insensitive. Accepts the canonical enum names (Int32, Float,
     * Vector, FloatRange, Enum, ...) plus common aliases (int, uint32, long,
     * range, ...). Covers every FieldType enum value.
     * @return The resolved FieldType, or std::nullopt if the string is unknown.
     */
    std::optional<FieldType> ParseFieldType(std::string_view raw);

    /**
     * @brief Parse a JSON "containerType" string into a FieldContainerType.
     * @details Case-insensitive canonical values: "None", "Array", "Map".
     * An empty string is treated as "None" (scalar field).
     * @return The resolved FieldContainerType, or std::nullopt if unknown.
     */
    std::optional<FieldContainerType> ParseContainerType(std::string_view raw);

    /**
     * @brief Canonical human-readable name for a FieldType.
     */
    std::string_view ToString(FieldType type);

    /**
     * @brief Canonical human-readable name for a FieldContainerType.
     */
    std::string_view ToString(FieldContainerType container);

    /**
     * @brief True if @p key is a type usable as a Map key in VTX serialization.
     * @details Map keys must be hashable scalars: String, Int8, Int32, Int64 or
     * Enum. Floating point, vectors, structs, etc. are rejected.
     */
    bool IsMapCompatibleKeyType(FieldType key);

    /**
     * @brief True if @p default_value is a valid literal for a field of @p type.
     * @details An empty string is always valid (it means "no explicit default").
     * Scalar types must parse fully (Int* -> integer, Float/Double -> real,
     * Bool -> true/false/1/0, String -> anything). Composite types
     * (Vector/Quat/Transform/FloatRange/Struct) only accept an empty default.
     */
    bool IsValidDefaultValue(FieldType type, std::string_view default_value);

} // namespace VTX
