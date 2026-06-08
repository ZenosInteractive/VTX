/**
 * @file vtx_validation.h
 * @brief Independently callable validation entry points.
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <string>

#include "vtx/common/vtx_diagnostics.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"

namespace VTX {

    class SchemaRegistry;

    /// Where an entity sits, for diagnostic context (all fields optional).
    struct EntityLocation {
        int32_t frame_index = -1;
        std::string bucket;
        std::string unique_id;
    };

    /**
     * @brief Validate a raw schema JSON document.
     * @details Wraps the rule-based SchemaValidator and re-expresses its issues
     * as structured diagnostics. A JSON parse failure is reported as
     * VtxErrorCode::SchemaParseError; rule violations as SchemaInvalid.
     */
    ValidationReport ValidateSchema(const std::string& schema_json);

    /**
     * @brief Validate a single entity against a resolved schema.
     * @details Checks that the entity type resolves to a schema struct and that
     * no per-type property array is larger than the schema declares.
     */
    ValidationReport ValidateEntity(const PropertyContainer& entity, const PropertyAddressCache& schema,
                                    const EntityLocation& where = {});

    /// Convenience overload taking a SchemaRegistry.
    ValidationReport ValidateEntity(const PropertyContainer& entity, const SchemaRegistry& schema,
                                    const EntityLocation& where = {});

    /**
     * @brief Validate every entity in a frame against a resolved schema.
     * @details Per bucket: flags duplicate (non-empty) unique_ids, then runs
     * ValidateEntity on each entity with its (frame, bucket, unique_id) context.
     */
    ValidationReport ValidateFrame(const Frame& frame, const PropertyAddressCache& schema, int32_t frame_index = -1);

    /// Convenience overload taking a SchemaRegistry.
    ValidationReport ValidateFrame(const Frame& frame, const SchemaRegistry& schema, int32_t frame_index = -1);

} // namespace VTX
