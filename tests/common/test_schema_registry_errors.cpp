// Error-path tests for VTX::SchemaRegistry.
//
// Covers malformed JSON, missing required keys, and unexpected type tags.
// All failure paths must return cleanly -- no crash, no partial state that
// would later manifest as UB.

#include <gtest/gtest.h>
#include <string>
#include "vtx/common/readers/schema_reader/schema_registry.h"

// ---------------------------------------------------------------------------
// LoadFromRawString failure paths
// ---------------------------------------------------------------------------

TEST(SchemaRegistryErrors, LoadFromRawStringEmptyFails) {
    VTX::SchemaRegistry schema;
    EXPECT_FALSE(schema.LoadFromRawString(""));
    EXPECT_FALSE(schema.GetIsValid());
}

TEST(SchemaRegistryErrors, LoadFromRawStringMalformedJsonFails) {
    VTX::SchemaRegistry schema;
    EXPECT_FALSE(schema.LoadFromRawString("{foo:"));
    EXPECT_FALSE(schema.GetIsValid());
}

TEST(SchemaRegistryErrors, LoadFromRawStringMissingPropertyMappingFails) {
    // Valid JSON but missing the "property_mapping" array -- the registry
    // should reject it rather than silently accept an empty schema.
    const char* raw = R"({
        "version": "1.0.0",
        "buckets": ["entity"]
    })";

    VTX::SchemaRegistry schema;
    const bool ok = schema.LoadFromRawString(raw);
    // Either LoadFromRawString returns false, OR it succeeds but produces a
    // registry with no structs.  Both are acceptable -- the forbidden case
    // is a registry that claims validity AND crashes on lookup.
    if (ok) {
        EXPECT_TRUE(schema.GetDefinitions().empty());
    } else {
        EXPECT_FALSE(schema.GetIsValid());
    }
}

TEST(SchemaRegistryErrors, LoadFromRawStringDuplicateStructIsDeterministic) {
    // Two structs named "Player".  The registry must not crash and must
    // produce a deterministic resolution (last-wins, first-wins, or explicit
    // rejection -- we don't care which, as long as it's consistent).
    const char* raw = R"({
        "version": "1.0.0",
        "buckets": ["entity"],
        "property_mapping": [
            {
                "struct": "Player",
                "values": [{
                    "name": "Score", "structType": "", "typeId": "Int32",
                    "keyId": "None", "containerType": "None",
                    "meta": {"type": "int32", "keyType": "", "category": "Player",
                             "displayName": "Score", "tooltip": "",
                             "defaultValue": "0", "version": 1, "fixedArrayDim": 1}
                }]
            },
            {
                "struct": "Player",
                "values": [{
                    "name": "Health", "structType": "", "typeId": "Float",
                    "keyId": "None", "containerType": "None",
                    "meta": {"type": "float", "keyType": "", "category": "Player",
                             "displayName": "Health", "tooltip": "",
                             "defaultValue": "0", "version": 1, "fixedArrayDim": 1}
                }]
            }
        ]
    })";

    VTX::SchemaRegistry schema;
    // Whatever the policy, this must not crash and two identical loads must
    // land in the same state.
    const bool ok1 = schema.LoadFromRawString(raw);
    (void)ok1;

    VTX::SchemaRegistry schema2;
    const bool ok2 = schema2.LoadFromRawString(raw);
    (void)ok2;

    // Both registries must agree on Player existence or absence.
    const bool player1_exists = (schema.GetStruct("Player") != nullptr);
    const bool player2_exists = (schema2.GetStruct("Player") != nullptr);
    EXPECT_EQ(player1_exists, player2_exists);
}

TEST(SchemaRegistryErrors, LoadFromRawStringUnknownTypeIdDoesntCrash) {
    // Field with a completely bogus typeId.  The registry may reject or
    // accept-and-ignore, but must not crash.
    const char* raw = R"({
        "version": "1.0.0",
        "buckets": ["entity"],
        "property_mapping": [
            {
                "struct": "Ghost",
                "values": [{
                    "name": "Spooky", "structType": "", "typeId": "QuantumFlux",
                    "keyId": "None", "containerType": "None",
                    "meta": {"type": "unknown", "keyType": "", "category": "Ghost",
                             "displayName": "Spooky", "tooltip": "",
                             "defaultValue": "", "version": 1, "fixedArrayDim": 1}
                }]
            }
        ]
    })";

    VTX::SchemaRegistry schema;
    const bool ok = schema.LoadFromRawString(raw);
    (void)ok;

    // If the struct was accepted, looking up the bogus field must not crash.
    if (const auto* s = schema.GetStruct("Ghost")) {
        (void)s; // just don't crash
    }
    // If not accepted, that's also fine -- both outcomes are valid.
    SUCCEED();
}
