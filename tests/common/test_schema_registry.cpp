// Tests for VTX::SchemaRegistry -- JSON schema loading + field resolution.
//
// Fixture: tests/fixtures/test_schema.json (copy of the arena schema).
// Player struct: UniqueID, Name, Team, Health, Armor, Position, Rotation,
//                Velocity, IsAlive, Score, Deaths.

#include <gtest/gtest.h>
#include "vtx/common/readers/schema_reader/schema_registry.h"

#include "util/test_fixtures.h"

namespace {
    std::string SchemaPath() {
        return VtxTest::FixturePath("test_schema.json");
    }
} // namespace

// ---------------------------------------------------------------------------
// LoadFromJson
// ---------------------------------------------------------------------------

TEST(SchemaRegistry, LoadFromJsonSucceedsOnValidFile) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    EXPECT_TRUE(schema.GetIsValid());
    EXPECT_FALSE(schema.GetDefinitions().empty());
}

TEST(SchemaRegistry, LoadFromJsonFailsOnMissingFile) {
    VTX::SchemaRegistry schema;
    EXPECT_FALSE(schema.LoadFromJson("/this/does/not/exist.json"));
}

TEST(SchemaRegistry, LoadFromRawStringSucceedsOnMinimalJson) {
    const char* raw = R"({
        "version": "1.0.0",
        "buckets": ["entity"],
        "property_mapping": [
            {
                "struct": "Tiny",
                "values": [
                    {
                        "name": "Score",
                        "structType": "",
                        "typeId": "Int32",
                        "keyId": "None",
                        "containerType": "None",
                        "meta": { "type": "int32", "keyType": "", "category": "Tiny",
                                  "displayName": "Score", "tooltip": "",
                                  "defaultValue": "0", "version": 1, "fixedArrayDim": 1 }
                    }
                ]
            }
        ]
    })";

    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromRawString(raw));
    EXPECT_NE(schema.GetStruct("Tiny"), nullptr);
}

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

TEST(SchemaRegistry, GetStructReturnsDefinitionForKnownName) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    EXPECT_NE(schema.GetStruct("Player"), nullptr);
    EXPECT_NE(schema.GetStruct("Projectile"), nullptr);
    EXPECT_NE(schema.GetStruct("MatchState"), nullptr);
}

TEST(SchemaRegistry, GetStructReturnsNullForUnknownName) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    EXPECT_EQ(schema.GetStruct("NonExistent"), nullptr);
}

TEST(SchemaRegistry, GetFieldResolvesKnownFields) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    EXPECT_NE(schema.GetField("Player", "UniqueID"), nullptr);
    EXPECT_NE(schema.GetField("Player", "Health"), nullptr);
    EXPECT_NE(schema.GetField("Player", "Position"), nullptr);
}

TEST(SchemaRegistry, GetFieldReturnsNullForUnknown) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    EXPECT_EQ(schema.GetField("Player", "DoesNotExist"), nullptr);
    EXPECT_EQ(schema.GetField("Ghost", "Health"), nullptr);
}

TEST(SchemaRegistry, GetStructTypeIdReturnsStableId) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    const int32_t player_id = schema.GetStructTypeId("Player");
    EXPECT_GE(player_id, 0);

    // Same lookup twice = same id.
    EXPECT_EQ(schema.GetStructTypeId("Player"), player_id);

    // Unknown struct = -1.
    EXPECT_EQ(schema.GetStructTypeId("Unknown"), -1);
}

TEST(SchemaRegistry, GetIndexReturnsNonNegativeForKnownField) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    EXPECT_GE(schema.GetIndex("Player", "UniqueID"), 0);
}
