/**
 * @file schema_validator.h
 * @brief Rule-based validator for VTX schema definitions.
 *
 * @details The validator runs a set of independent rules over a schema BEFORE
 * the registry resolves it into SchemaStruct / PropertyContainer indices, so
 * malformed schemas are rejected up front (before any frame is built) instead
 * of silently degrading at runtime.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "schema_validation_result.h"

namespace VTX {

    /**
     * @brief Pre-resolution view of a single field (raw JSON strings preserved).
     */
    struct RawSchemaField {
        std::string name;
        std::string type_id;        ///< Raw "typeId" string.
        std::string container_type; ///< Raw "containerType" string ("" == None).
        std::string key_id;         ///< Raw "keyId" string ("" / "None" == absent).
        std::string struct_type;    ///< Raw "structType" string (for Struct fields).
        std::string default_value;  ///< Raw "meta.defaultValue" string.
        bool has_fixed_array_dim = false;
        int64_t fixed_array_dim = 0; ///< Raw "meta.fixedArrayDim" (valid iff has_fixed_array_dim).
        int field_index = -1;        ///< Position within the struct (for messages).
    };

    /**
     * @brief Pre-resolution view of a single struct.
     */
    struct RawSchemaStruct {
        std::string name;
        std::optional<int64_t> declared_id; ///< Explicit "id" if the schema provides one.
        std::vector<RawSchemaField> fields;
        int struct_index = -1; ///< Position within property_mapping (for messages).
    };

    /**
     * @brief Pre-resolution view of an entire schema document.
     */
    struct RawSchema {
        std::vector<RawSchemaStruct> structs;

        bool has_buckets = false;          ///< Document has a top-level "buckets" key.
        bool buckets_is_array = false;     ///< That key is an array (valid shape).
        std::vector<std::string> buckets;  ///< String entries of "buckets", in order.
        int non_string_bucket_entries = 0; ///< Entries of "buckets" that were not strings.
    };

    /**
     * @brief A single, self-contained schema validation rule.
     */
    class ISchemaValidationRule {
    public:
        virtual ~ISchemaValidationRule() = default;
        virtual std::string Name() const = 0;
        virtual void Validate(const RawSchema& schema, SchemaValidationResult& out) const = 0;
    };

    /**
     * @brief Aggregates and runs a collection of schema validation rules.
     */
    class SchemaValidator {
    public:
        SchemaValidator();

        explicit SchemaValidator(std::vector<std::unique_ptr<ISchemaValidationRule>> rules);

        void AddRule(std::unique_ptr<ISchemaValidationRule> rule);

        SchemaValidationResult Validate(const std::string& raw_json) const;

        SchemaValidationResult Validate(const RawSchema& schema) const;

        static std::vector<std::unique_ptr<ISchemaValidationRule>> DefaultRules();

    private:
        std::vector<std::unique_ptr<ISchemaValidationRule>> rules_;
    };

} // namespace VTX
