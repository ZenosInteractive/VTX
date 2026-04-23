// Tests for VTX::PropertyAddressCache -- O(1) name -> (index, type, container)
// lookup produced by SchemaRegistry.

#include <gtest/gtest.h>
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"

#include "util/test_fixtures.h"

namespace {
    std::string SchemaPath() {
        return VtxTest::FixturePath("test_schema.json");
    }
} // namespace

TEST(PropertyAddressCache, PopulatedAfterSchemaLoad) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    const auto& cache = schema.GetPropertyCache();
    EXPECT_FALSE(cache.structs.empty());
    EXPECT_FALSE(cache.name_to_id.empty());
}

TEST(PropertyAddressCache, NameToIdContainsAllStructs) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    const auto& cache = schema.GetPropertyCache();

    ASSERT_TRUE(cache.name_to_id.contains("Player"));
    ASSERT_TRUE(cache.name_to_id.contains("Projectile"));
    ASSERT_TRUE(cache.name_to_id.contains("MatchState"));

    // Each id must index a valid StructSchemaCache.
    for (const auto& [name, id] : cache.name_to_id) {
        ASSERT_TRUE(cache.structs.contains(id)) << name;
    }
}

TEST(PropertyAddressCache, PlayerPropertiesResolveToValidAddresses) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    const auto& cache = schema.GetPropertyCache();

    const int32_t player_id = cache.name_to_id.at("Player");
    const auto& player = cache.structs.at(player_id);
    EXPECT_EQ(player.name, "Player");

    for (const char* field : {"UniqueID", "Name", "Team", "Health", "Armor", "Position", "Rotation", "Velocity",
                              "IsAlive", "Score", "Deaths"}) {
        ASSERT_TRUE(player.properties.contains(field)) << field;
        const auto& addr = player.properties.at(field);
        EXPECT_TRUE(addr.IsValid()) << field;
    }
}

TEST(PropertyAddressCache, PropertyOrderMatchesSchemaFieldOrder) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));
    const auto& cache = schema.GetPropertyCache();

    const int32_t player_id = cache.name_to_id.at("Player");
    const auto& player = cache.structs.at(player_id);
    auto ordered = player.GetPropertiesInOrder();
    ASSERT_EQ(ordered.size(), 11u);
    EXPECT_EQ(ordered[0].name, "UniqueID");
    EXPECT_EQ(ordered[10].name, "Deaths");
}

TEST(PropertyAddressCache, MakeLookupKeyIsDeterministic) {
    using VTX::FieldContainerType;
    using VTX::FieldType;
    using VTX::MakePropertyLookupKey;
    EXPECT_EQ(MakePropertyLookupKey(0, FieldType::Float, FieldContainerType::None),
              MakePropertyLookupKey(0, FieldType::Float, FieldContainerType::None));
    EXPECT_NE(MakePropertyLookupKey(0, FieldType::Float, FieldContainerType::None),
              MakePropertyLookupKey(1, FieldType::Float, FieldContainerType::None));
    EXPECT_NE(MakePropertyLookupKey(0, FieldType::Float, FieldContainerType::None),
              MakePropertyLookupKey(0, FieldType::Int32, FieldContainerType::None));
}

TEST(PropertyAddressCache, ClearRemovesEverything) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    // Cache is owned by the registry -- copying into a local lets us clear.
    VTX::PropertyAddressCache cache = schema.GetPropertyCache();
    ASSERT_FALSE(cache.structs.empty());

    cache.Clear();
    EXPECT_TRUE(cache.structs.empty());
    EXPECT_TRUE(cache.name_to_id.empty());
}
