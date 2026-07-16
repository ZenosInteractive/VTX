// Tests for VTX::SchemaRegistry -- JSON schema loading + field resolution.
//
// Fixture: tests/fixtures/test_schema.json (copy of the arena schema).
// Player struct: UniqueID, Name, Team, Health, Armor, Position, Rotation,
//                Velocity, IsAlive, Score, Deaths.

#include <gtest/gtest.h>
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

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

// ---------------------------------------------------------------------------
// Bucket names ("buckets" array)
// ---------------------------------------------------------------------------

namespace {
    // Minimal valid property_mapping to append after a custom "buckets" value.
    constexpr const char* kTinyMapping = R"("property_mapping": [
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
    ])";
} // namespace

TEST(SchemaRegistry, GetBucketNamesParsesDeclaredBuckets) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    ASSERT_EQ(schema.GetBucketNames().size(), 1u);
    EXPECT_EQ(schema.GetBucketNames()[0], "entity");

    // The property cache carries the same names (used by the reader).
    EXPECT_EQ(schema.GetPropertyCache().bucket_names, schema.GetBucketNames());
}

TEST(SchemaRegistry, GetBucketNamesPreservesDeclarationOrder) {
    const std::string raw =
        std::string(R"({ "version": "1.0.0", "buckets": ["entity", "bone_data", "economy"], )") + kTinyMapping + "}";

    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromRawString(raw));

    const auto& names = schema.GetBucketNames();
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "entity");
    EXPECT_EQ(names[1], "bone_data");
    EXPECT_EQ(names[2], "economy");
}

TEST(SchemaRegistry, GetBucketNamesEmptyWhenKeyMissing) {
    const std::string raw = std::string(R"({ "version": "1.0.0", )") + kTinyMapping + "}";

    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromRawString(raw));
    EXPECT_TRUE(schema.GetBucketNames().empty());
    EXPECT_TRUE(schema.GetPropertyCache().bucket_names.empty());
}

TEST(SchemaRegistry, ReloadReplacesBucketNames) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    ASSERT_FALSE(schema.GetBucketNames().empty());

    const std::string raw = std::string(R"({ "version": "1.0.0", )") + kTinyMapping + "}";
    ASSERT_TRUE(schema.LoadFromRawString(raw));
    EXPECT_TRUE(schema.GetBucketNames().empty());
}

// ---------------------------------------------------------------------------
// Array / Map pre-sizing (symmetric with scalar pre-sizing)
// ---------------------------------------------------------------------------

namespace {
    // Player declares: 2 int32 scalars, 1 float array, 2 string arrays,
    // 1 struct array (Inventory of Item) and 1 struct map (AmmoByWeapon of Ammo).
    constexpr const char* kArrayMapSchema = R"({
        "version": "1.0.0",
        "buckets": ["entity"],
        "property_mapping": [
            { "struct": "Item", "values": [
                {"name":"Id","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}}
            ]},
            { "struct": "Ammo", "values": [
                {"name":"Count","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}}
            ]},
            { "struct": "Player", "values": [
                {"name":"Score","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}},
                {"name":"Level","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}},
                {"name":"Cooldowns","typeId":"Float","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":0}},
                {"name":"Tags","typeId":"String","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":0}},
                {"name":"Names","typeId":"String","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":0}},
                {"name":"Inventory","typeId":"Struct","containerType":"Array","keyId":"None","structType":"Item","meta":{"defaultValue":"","fixedArrayDim":0}},
                {"name":"AmmoByWeapon","typeId":"Struct","containerType":"Map","keyId":"String","structType":"Ammo","meta":{"defaultValue":"","fixedArrayDim":1}}
            ]}
        ]
    })";
} // namespace

TEST(SchemaRegistry, PreSizesArraysAndMapsFromSchema) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromRawString(kArrayMapSchema));

    const VTX::SchemaStruct* player = schema.GetStruct("Player");
    ASSERT_NE(player, nullptr);

    VTX::PropertyContainer c;
    VTX::Helpers::PreparePropertyContainer(c, *player);

    // Scalars: unchanged (two int32 fields).
    EXPECT_EQ(c.int32_properties.size(), 2u);

    // Arrays: one empty subarray per declared array field, grouped per type.
    EXPECT_EQ(c.float_arrays.SubArrayCount(), 1u);
    EXPECT_EQ(c.string_arrays.SubArrayCount(), 2u);
    EXPECT_EQ(c.any_struct_arrays.SubArrayCount(), 1u);
    EXPECT_TRUE(c.float_arrays.GetSubArray(0).empty());
    EXPECT_TRUE(c.string_arrays.GetSubArray(1).empty());

    // Array types with no declared fields stay untouched.
    EXPECT_EQ(c.int32_arrays.SubArrayCount(), 0u);
    EXPECT_EQ(c.vector_arrays.SubArrayCount(), 0u);

    // Map: one empty, Struct-valued map slot.
    ASSERT_EQ(c.map_properties.size(), 1u);
    EXPECT_TRUE(c.map_properties[0].keys.empty());
    EXPECT_TRUE(c.map_properties[0].values.empty());
}

TEST(SchemaRegistry, ArrayAndMapSizingLandInPropertyCache) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromRawString(kArrayMapSchema));

    const int32_t player_id = schema.GetStructTypeId("Player");
    ASSERT_GE(player_id, 0);

    const auto& sc = schema.GetPropertyCache().structs.at(player_id);
    const auto string_idx = static_cast<size_t>(VTX::FieldType::String);
    const auto float_idx = static_cast<size_t>(VTX::FieldType::Float);
    ASSERT_GT(sc.array_max_indices.size(), string_idx);
    EXPECT_EQ(sc.array_max_indices[string_idx], 2);
    EXPECT_EQ(sc.array_max_indices[float_idx], 1);
    EXPECT_EQ(sc.map_max_index, 1);
}

// A schema without array/map fields leaves those sizes empty/zero.
TEST(SchemaRegistry, ScalarOnlySchemaHasNoArrayOrMapSizing) {
    const std::string raw = std::string(R"({ "version": "1.0.0", )") + kTinyMapping + "}";
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromRawString(raw));

    const int32_t id = schema.GetStructTypeId("Tiny");
    ASSERT_GE(id, 0);
    const auto& sc = schema.GetPropertyCache().structs.at(id);
    EXPECT_TRUE(sc.array_max_indices.empty());
    EXPECT_EQ(sc.map_max_index, 0);
}
