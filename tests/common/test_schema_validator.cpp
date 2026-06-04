// Tests for VTX::SchemaValidator and the strict schema validation now enforced
// by VTX::SchemaRegistry::LoadFromRawString.
//
// Each rule from the schema-validator spec gets focused coverage, plus the
// canonical-casing normalization, full FieldType enum support, the registry
// rejection path, and the injectable-rules extension point (SOLID/OCP).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "vtx/common/readers/schema_reader/schema_enums.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/readers/schema_reader/schema_validation_result.h"
#include "vtx/common/readers/schema_reader/schema_validator.h"

namespace {

    // Wrap one or more raw field objects into a single-struct schema document.
    std::string SchemaWithFields(const std::string& fields_json, const std::string& struct_name = "S") {
        return R"({"version":"1.0.0","buckets":["entity"],"property_mapping":[{"struct":")" + struct_name +
               R"(","values":[)" + fields_json + R"(]}]})";
    }

    bool HasRuleError(const VTX::SchemaValidationResult& result, const std::string& rule) {
        for (const auto& issue : result.Issues()) {
            if (issue.severity == VTX::SchemaIssueSeverity::Error && issue.rule == rule) {
                return true;
            }
        }
        return false;
    }

    VTX::SchemaValidationResult Validate(const std::string& raw_json) {
        return VTX::SchemaValidator {}.Validate(raw_json);
    }

    // A minimal, fully valid Int32 scalar field.
    constexpr const char* kValidField =
        R"({"name":"Score","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}})";

} // namespace

// ===================================================================
//  Happy path
// ===================================================================

TEST(SchemaValidator, ValidSchemaHasNoIssues) {
    const auto result = Validate(SchemaWithFields(kValidField));
    EXPECT_TRUE(result.IsValid());
    EXPECT_EQ(result.ErrorCount(), 0u);
}

// ===================================================================
//  #1 / #4  FieldType
// ===================================================================

TEST(SchemaValidator, RejectsUnknownTypeId) {
    const char* field =
        R"({"name":"X","typeId":"QuantumFlux","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "FieldType"));
}

TEST(SchemaValidator, RejectsNoneFieldType) {
    const char* field =
        R"({"name":"X","typeId":"None","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "FieldType"));
}

TEST(SchemaValidator, SupportsEveryOfficialFieldType) {
    // FloatRange and Enum were NOT parseable before -- they would silently
    // become None. They must now be first-class, valid field types.
    const char* fields =
        R"({"name":"R","typeId":"FloatRange","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}},)"
        R"({"name":"E","typeId":"Enum","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"2","fixedArrayDim":1}},)"
        R"({"name":"T","typeId":"Transform","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(Validate(SchemaWithFields(fields)).IsValid());

    // And the shared parser knows all 14 enum values.
    EXPECT_TRUE(VTX::ParseFieldType("FloatRange").has_value());
    EXPECT_TRUE(VTX::ParseFieldType("Enum").has_value());
}

// ===================================================================
//  #2 / #3  ContainerType
// ===================================================================

TEST(SchemaValidator, RejectsUnknownContainerType) {
    const char* field =
        R"({"name":"X","typeId":"Int32","containerType":"Set","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "ContainerType"));
}

TEST(SchemaValidator, AcceptsCanonicalAndMixedCasing) {
    // None / Array / Map in any casing, and a mixed-case typeId.
    const char* fields =
        R"({"name":"A","typeId":"iNt32","containerType":"none","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}},)"
        R"({"name":"B","typeId":"Int32","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}},)"
        R"({"name":"C","typeId":"Int32","containerType":"MAP","keyId":"String","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(Validate(SchemaWithFields(fields)).IsValid());

    EXPECT_EQ(VTX::ParseContainerType("array"), VTX::FieldContainerType::Array);
    EXPECT_EQ(VTX::ParseContainerType("Map"), VTX::FieldContainerType::Map);
    EXPECT_EQ(VTX::ParseContainerType(""), VTX::FieldContainerType::None);
}

// ===================================================================
//  #5  Duplicate / empty field names
// ===================================================================

TEST(SchemaValidator, RejectsDuplicateFieldName) {
    const char* fields =
        R"({"name":"Dup","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}},)"
        R"({"name":"Dup","typeId":"Float","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(fields)), "DuplicateFieldName"));
}

TEST(SchemaValidator, RejectsEmptyFieldName) {
    const char* field =
        R"({"name":"","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "DuplicateFieldName"));
}

// ===================================================================
//  #6  Duplicate struct names / ids
// ===================================================================

TEST(SchemaValidator, RejectsDuplicateStructName) {
    const std::string raw = R"({"version":"1.0.0","buckets":["entity"],"property_mapping":[)"
                            R"({"struct":"Player","values":[)" +
                            std::string(kValidField) +
                            R"(]},)"
                            R"({"struct":"Player","values":[)" +
                            std::string(kValidField) + R"(]}]})";
    EXPECT_TRUE(HasRuleError(Validate(raw), "DuplicateStruct"));
}

TEST(SchemaValidator, RejectsDuplicateStructId) {
    const std::string raw = R"({"version":"1.0.0","buckets":["entity"],"property_mapping":[)"
                            R"({"struct":"A","id":7,"values":[)" +
                            std::string(kValidField) +
                            R"(]},)"
                            R"({"struct":"B","id":7,"values":[)" +
                            std::string(kValidField) + R"(]}]})";
    EXPECT_TRUE(HasRuleError(Validate(raw), "DuplicateStruct"));
}

// ===================================================================
//  #7  Struct field reference resolution
// ===================================================================

TEST(SchemaValidator, AcceptsResolvedStructReference) {
    const std::string raw =
        R"({"version":"1.0.0","buckets":["entity"],"property_mapping":[)"
        R"({"struct":"Inner","values":[)" +
        std::string(kValidField) +
        R"(]},)"
        R"({"struct":"Outer","values":[)"
        R"({"name":"In","typeId":"Struct","containerType":"None","keyId":"None","structType":"Inner","meta":{"defaultValue":"","fixedArrayDim":1}})"
        R"(]}]})";
    EXPECT_TRUE(Validate(raw).IsValid());
}

TEST(SchemaValidator, RejectsUnresolvedStructReference) {
    const char* field =
        R"({"name":"In","typeId":"Struct","containerType":"None","keyId":"None","structType":"DoesNotExist","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "StructTypeResolution"));
}

TEST(SchemaValidator, RejectsStructFieldWithoutStructType) {
    const char* field =
        R"({"name":"In","typeId":"Struct","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "StructTypeResolution"));
}

// ===================================================================
//  #8  Map key validity
// ===================================================================

TEST(SchemaValidator, AcceptsMapWithCompatibleKey) {
    const char* field =
        R"({"name":"M","typeId":"Int32","containerType":"Map","keyId":"String","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(Validate(SchemaWithFields(field)).IsValid());
}

TEST(SchemaValidator, RejectsMapWithMissingKey) {
    const char* field =
        R"({"name":"M","typeId":"Int32","containerType":"Map","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "MapKey"));
}

TEST(SchemaValidator, RejectsMapWithIncompatibleKey) {
    const char* field =
        R"({"name":"M","typeId":"Int32","containerType":"Map","keyId":"Float","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "MapKey"));
}

// ===================================================================
//  #9  defaultValue validation
// ===================================================================

TEST(SchemaValidator, RejectsNonNumericIntegerDefault) {
    const char* field =
        R"({"name":"X","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"abc","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "DefaultValue"));
}

TEST(SchemaValidator, RejectsNonBooleanBoolDefault) {
    const char* field =
        R"({"name":"X","typeId":"Bool","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"yes","fixedArrayDim":1}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "DefaultValue"));
}

TEST(SchemaValidator, AcceptsValidScalarDefaults) {
    const char* fields =
        R"({"name":"F","typeId":"Float","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"1.5","fixedArrayDim":1}},)"
        R"({"name":"B","typeId":"Bool","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"true","fixedArrayDim":1}},)"
        R"({"name":"S","typeId":"String","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"anything","fixedArrayDim":1}})";
    EXPECT_TRUE(Validate(SchemaWithFields(fields)).IsValid());
}

// ===================================================================
//  #10  fixedArrayDim validation
// ===================================================================

TEST(SchemaValidator, RejectsNegativeFixedArrayDim) {
    const char* field =
        R"({"name":"X","typeId":"Int32","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":-3}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "FixedArrayDim"));
}

TEST(SchemaValidator, RejectsFixedArrayDimGreaterThanOneOnScalar) {
    const char* field =
        R"({"name":"X","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":4}})";
    EXPECT_TRUE(HasRuleError(Validate(SchemaWithFields(field)), "FixedArrayDim"));
}

TEST(SchemaValidator, AcceptsFixedArrayDimOnArrayField) {
    const char* field =
        R"({"name":"X","typeId":"Int32","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":4}})";
    EXPECT_TRUE(Validate(SchemaWithFields(field)).IsValid());
}

// ===================================================================
//  SchemaRegistry integration -- strict rejection
// ===================================================================

TEST(SchemaRegistryValidation, LoadRejectsInvalidSchema) {
    const char* field =
        R"({"name":"X","typeId":"Bogus","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}})";
    VTX::SchemaRegistry registry;
    EXPECT_FALSE(registry.LoadFromRawString(SchemaWithFields(field)));
    EXPECT_FALSE(registry.GetIsValid());
    EXPECT_TRUE(registry.GetValidationResult().HasErrors());
}

TEST(SchemaRegistryValidation, LoadAcceptsValidSchema) {
    VTX::SchemaRegistry registry;
    EXPECT_TRUE(registry.LoadFromRawString(SchemaWithFields(kValidField)));
    EXPECT_TRUE(registry.GetIsValid());
    EXPECT_TRUE(registry.GetValidationResult().IsValid());
}

TEST(SchemaRegistryValidation, StaticValidateSchemaReportsIssues) {
    const char* field =
        R"({"name":"X","typeId":"Int32","containerType":"Nope","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}})";
    const auto result = VTX::SchemaRegistry::ValidateSchema(SchemaWithFields(field));
    EXPECT_FALSE(result.IsValid());
    EXPECT_TRUE(HasRuleError(result, "ContainerType"));
}

// ===================================================================
//  SOLID -- rules are injectable (Open/Closed + Dependency Inversion)
// ===================================================================

namespace {
    class AlwaysFailsRule final : public VTX::ISchemaValidationRule {
    public:
        std::string Name() const override { return "AlwaysFails"; }
        void Validate(const VTX::RawSchema&, VTX::SchemaValidationResult& out) const override {
            out.AddError(Name(), "", "", "intentional failure");
        }
    };
} // namespace

TEST(SchemaValidator, RunsOnlyInjectedRules) {
    std::vector<std::unique_ptr<VTX::ISchemaValidationRule>> rules;
    rules.push_back(std::make_unique<AlwaysFailsRule>());
    VTX::SchemaValidator validator(std::move(rules));

    // Even a perfectly valid document fails, because only the injected rule runs.
    const auto result = validator.Validate(SchemaWithFields(kValidField));
    ASSERT_EQ(result.Issues().size(), 1u);
    EXPECT_EQ(result.Issues()[0].rule, "AlwaysFails");
}
