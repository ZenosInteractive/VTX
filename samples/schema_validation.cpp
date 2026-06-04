// schema_validation.cpp -- Demonstrates VTX::SchemaValidator.
//
// The schema validator rejects malformed schemas BEFORE the registry resolves
// them into PropertyContainer indices (and long before a frame is built):
//   * unknown typeId / containerType            (no silent fallback to None)
//   * duplicate field names / struct names / ids
//   * unresolved Struct references
//   * Map fields with a missing / incompatible keyId
//   * defaultValue that does not match the field type
//   * insane fixedArrayDim
// plus canonical-casing normalization and full FieldType enum support.
//
// This sample is self-contained (schemas are inline) and self-checking: it
// returns 0 only when every expectation holds, so it doubles as a smoke test.
//
// Build: link against vtx_common.

#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/readers/schema_reader/schema_validation_result.h"
#include "vtx/common/readers/schema_reader/schema_validator.h"
#include "vtx/common/vtx_logger.h"

#include <memory>
#include <string>
#include <vector>

namespace {

    // A well-formed schema: scalar fields, valid types/defaults, mixed casing
    // on containerType/typeId (which the validator normalizes, not rejects).
    const char* kValidSchema = R"({
        "version": "1.0.0",
        "buckets": ["entity"],
        "property_mapping": [
            {
                "struct": "Player",
                "values": [
                    {"name":"UniqueID","typeId":"String","containerType":"none","keyId":"None","structType":"",
                     "meta":{"defaultValue":"","fixedArrayDim":1}},
                    {"name":"Health","typeId":"Float","containerType":"None","keyId":"None","structType":"",
                     "meta":{"defaultValue":"100","fixedArrayDim":1}},
                    {"name":"Tags","typeId":"String","containerType":"Array","keyId":"None","structType":"",
                     "meta":{"defaultValue":"","fixedArrayDim":4}}
                ]
            }
        ]
    })";

    // A schema that violates several rules at once, to show the full report:
    //   - typeId "QuantumFlux"        -> unknown type
    //   - field "A" declared twice    -> duplicate field name
    //   - Int32 default "oops"        -> invalid defaultValue
    //   - Map field keyId "Float"     -> incompatible map key
    //   - Struct field -> "Ghost"     -> unresolved struct reference
    const char* kInvalidSchema = R"({
        "version": "1.0.0",
        "buckets": ["entity"],
        "property_mapping": [
            {
                "struct": "Broken",
                "values": [
                    {"name":"A","typeId":"QuantumFlux","containerType":"None","keyId":"None","structType":"",
                     "meta":{"defaultValue":"","fixedArrayDim":1}},
                    {"name":"A","typeId":"Int32","containerType":"None","keyId":"None","structType":"",
                     "meta":{"defaultValue":"oops","fixedArrayDim":1}},
                    {"name":"M","typeId":"Int32","containerType":"Map","keyId":"Float","structType":"",
                     "meta":{"defaultValue":"","fixedArrayDim":1}},
                    {"name":"Nested","typeId":"Struct","containerType":"None","keyId":"None","structType":"Ghost",
                     "meta":{"defaultValue":"","fixedArrayDim":1}}
                ]
            }
        ]
    })";

    // A custom rule: schemas must declare at least one struct. Demonstrates the
    // Open/Closed extension point -- new checks plug in without touching the
    // built-in rules or the validator.
    class RequireAtLeastOneStructRule final : public VTX::ISchemaValidationRule {
    public:
        std::string Name() const override { return "RequireAtLeastOneStruct"; }
        void Validate(const VTX::RawSchema& schema, VTX::SchemaValidationResult& out) const override {
            if (schema.structs.empty()) {
                out.AddError(Name(), "", "", "schema declares no structs.");
            }
        }
    };

} // namespace

int main() {
    bool ok = true;

    // 1) A valid schema validates clean.
    {
        const auto result = VTX::SchemaRegistry::ValidateSchema(kValidSchema);
        VTX_INFO("[valid]   {} error(s), {} warning(s)", result.ErrorCount(), result.WarningCount());
        if (!result.IsValid()) {
            VTX_ERROR("expected the valid schema to pass, but it reported errors:\n{}", result.ToString());
            ok = false;
        }
    }

    // 2) A malformed schema is rejected, and the report explains why.
    {
        const auto result = VTX::SchemaRegistry::ValidateSchema(kInvalidSchema);
        VTX_INFO("[invalid] {} error(s):\n{}", result.ErrorCount(), result.ToString());
        if (result.IsValid()) {
            VTX_ERROR("expected the invalid schema to be rejected.");
            ok = false;
        }
    }

    // 3) The registry refuses to LOAD an invalid schema (strict by default).
    {
        VTX::SchemaRegistry good;
        VTX::SchemaRegistry bad;
        const bool loaded_good = good.LoadFromRawString(kValidSchema);
        const bool loaded_bad = bad.LoadFromRawString(kInvalidSchema);
        VTX_INFO("[load]    valid -> {}, invalid -> {}", loaded_good, loaded_bad);
        if (!loaded_good || loaded_bad) {
            VTX_ERROR("registry strict-load contract violated.");
            ok = false;
        }
    }

    // 4) Custom rules plug in alongside (or instead of) the defaults.
    {
        std::vector<std::unique_ptr<VTX::ISchemaValidationRule>> rules = VTX::SchemaValidator::DefaultRules();
        rules.push_back(std::make_unique<RequireAtLeastOneStructRule>());
        VTX::SchemaValidator validator(std::move(rules));

        const char* empty_schema = R"({"version":"1.0.0","buckets":["entity"],"property_mapping":[]})";
        const auto result = validator.Validate(empty_schema);
        VTX_INFO("[custom]  empty schema -> {} error(s)", result.ErrorCount());
        if (result.IsValid()) {
            VTX_ERROR("custom rule did not fire on an empty schema.");
            ok = false;
        }
    }

    if (!ok) {
        VTX_ERROR("schema_validation sample: FAILED expectations.");
        return 1;
    }
    VTX_INFO("schema_validation sample: all checks passed.");
    return 0;
}
