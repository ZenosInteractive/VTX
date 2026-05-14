// Tests for VTX::GenericNativeLoader + VTX::StructMapping<T> and the
// ADL `to_vtx_value` hook (declared in loader_base.h).
//
// Coverage:
//   - Load<T>() walks StructMapping<T>::GetFields() and lands every member
//     in the correct PropertyContainer slot.
//   - The ADL hook converts a client's custom math type into VTX::Vector
//     transparently inside LoadField, without StructMapping<> ever knowing
//     about the conversion.

#include <gtest/gtest.h>

#include "vtx/common/adapters/native/struct_mapping.h"
#include "vtx/common/readers/frame_reader/native_loader.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

#include <string>

namespace vtx_native_loader_test {

    // -----------------------------------------------------------------
    //  Setup 1: a player struct that already uses VTX::Vector / VTX::Quat
    // -----------------------------------------------------------------

    struct PlainPlayer {
        std::string unique_id;
        std::string name;
        int team = 0;
        float health = 100.0f;
        float armor = 50.0f;
        VTX::Vector position;
        VTX::Quat rotation;
        VTX::Vector velocity;
        bool is_alive = true;
        int score = 0;
        int deaths = 0;
    };

    // -----------------------------------------------------------------
    //  Setup 2: a player struct that uses a CUSTOM math type which the
    //  client converts to VTX::Vector via the ADL hook.
    // -----------------------------------------------------------------

    struct CustomVec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // ADL hook -- defined in the SAME namespace as the custom type so that
    // unqualified `to_vtx_value(v)` inside GenericLoaderBase::LoadField
    // (which lives in namespace VTX) finds it via argument-dependent lookup.
    inline VTX::Vector to_vtx_value(const CustomVec3& v) {
        return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
    }

    struct PlayerWithCustomVec {
        std::string unique_id;
        std::string name;
        int team = 0;
        float health = 100.0f;
        float armor = 50.0f;
        CustomVec3 position; // <-- custom type
        VTX::Quat rotation;
        CustomVec3 velocity; // <-- custom type
        bool is_alive = true;
        int score = 0;
        int deaths = 0;
    };

} // namespace vtx_native_loader_test

// StructMapping specializations live in namespace VTX (qualified form).

template <>
struct VTX::StructMapping<vtx_native_loader_test::PlainPlayer> {
    static constexpr auto GetFields() {
        using P = vtx_native_loader_test::PlainPlayer;
        return std::make_tuple(MakeStructField("UniqueID", &P::unique_id), MakeStructField("Name", &P::name),
                               MakeStructField("Team", &P::team), MakeStructField("Health", &P::health),
                               MakeStructField("Armor", &P::armor), MakeStructField("Position", &P::position),
                               MakeStructField("Rotation", &P::rotation), MakeStructField("Velocity", &P::velocity),
                               MakeStructField("IsAlive", &P::is_alive), MakeStructField("Score", &P::score),
                               MakeStructField("Deaths", &P::deaths));
    }
};

template <>
struct VTX::StructMapping<vtx_native_loader_test::PlayerWithCustomVec> {
    static constexpr auto GetFields() {
        using P = vtx_native_loader_test::PlayerWithCustomVec;
        return std::make_tuple(MakeStructField("UniqueID", &P::unique_id), MakeStructField("Name", &P::name),
                               MakeStructField("Team", &P::team), MakeStructField("Health", &P::health),
                               MakeStructField("Armor", &P::armor), MakeStructField("Position", &P::position),
                               MakeStructField("Rotation", &P::rotation), MakeStructField("Velocity", &P::velocity),
                               MakeStructField("IsAlive", &P::is_alive), MakeStructField("Score", &P::score),
                               MakeStructField("Deaths", &P::deaths));
    }
};

namespace {
    std::string SchemaPath() {
        return VtxTest::FixturePath("test_schema.json");
    }
} // namespace

// ===================================================================
//  Tests
// ===================================================================

TEST(NativeLoader, LoadsAllSlotsFromMappedStruct) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    VTX::GenericNativeLoader loader(schema.GetPropertyCache());

    vtx_native_loader_test::PlainPlayer p {};
    p.unique_id = "player_42";
    p.name = "Alice";
    p.team = 1;
    p.health = 87.5f;
    p.armor = 30.0f;
    p.position = {1.0, 2.0, 3.0};
    p.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    p.velocity = {-0.5, 0.0, 0.5};
    p.is_alive = true;
    p.score = 5;
    p.deaths = 2;

    VTX::PropertyContainer dest;
    loader.Load(p, dest, "Player");

    EXPECT_GE(dest.entity_type_id, 0);

    ASSERT_EQ(dest.string_properties.size(), 2u);
    EXPECT_EQ(dest.string_properties[0], "player_42");
    EXPECT_EQ(dest.string_properties[1], "Alice");

    ASSERT_EQ(dest.int32_properties.size(), 3u);
    EXPECT_EQ(dest.int32_properties[0], 1); // Team
    EXPECT_EQ(dest.int32_properties[1], 5); // Score
    EXPECT_EQ(dest.int32_properties[2], 2); // Deaths

    ASSERT_EQ(dest.float_properties.size(), 2u);
    EXPECT_FLOAT_EQ(dest.float_properties[0], 87.5f);
    EXPECT_FLOAT_EQ(dest.float_properties[1], 30.0f);

    ASSERT_EQ(dest.vector_properties.size(), 2u);
    EXPECT_DOUBLE_EQ(dest.vector_properties[0].x, 1.0);
    EXPECT_DOUBLE_EQ(dest.vector_properties[0].y, 2.0);
    EXPECT_DOUBLE_EQ(dest.vector_properties[0].z, 3.0);
    EXPECT_DOUBLE_EQ(dest.vector_properties[1].x, -0.5);
    EXPECT_DOUBLE_EQ(dest.vector_properties[1].z, 0.5);

    ASSERT_EQ(dest.quat_properties.size(), 1u);
    EXPECT_FLOAT_EQ(dest.quat_properties[0].w, 1.0f);

    ASSERT_EQ(dest.bool_properties.size(), 1u);
    EXPECT_TRUE(dest.bool_properties[0]);

    EXPECT_NE(dest.content_hash, 0u);
}

TEST(NativeLoader, ADLConversionFromCustomMathType) {
    VTX::SchemaRegistry schema;
    ASSERT_TRUE(schema.LoadFromJson(SchemaPath()));

    VTX::GenericNativeLoader loader(schema.GetPropertyCache());

    vtx_native_loader_test::PlayerWithCustomVec p {};
    p.unique_id = "custom_player";
    p.team = 2;
    p.health = 50.0f;
    p.position = {1.5f, 2.5f, 3.5f};  // CustomVec3 -> ADL -> VTX::Vector
    p.velocity = {-1.0f, 0.0f, 2.0f}; // CustomVec3 -> ADL -> VTX::Vector

    VTX::PropertyContainer dest;
    loader.Load(p, dest, "Player");

    ASSERT_EQ(dest.vector_properties.size(), 2u);

    // Position: client's CustomVec3 was converted to VTX::Vector via ADL.
    EXPECT_DOUBLE_EQ(dest.vector_properties[0].x, 1.5);
    EXPECT_DOUBLE_EQ(dest.vector_properties[0].y, 2.5);
    EXPECT_DOUBLE_EQ(dest.vector_properties[0].z, 3.5);

    // Velocity: same path, same result.
    EXPECT_DOUBLE_EQ(dest.vector_properties[1].x, -1.0);
    EXPECT_DOUBLE_EQ(dest.vector_properties[1].y, 0.0);
    EXPECT_DOUBLE_EQ(dest.vector_properties[1].z, 2.0);

    // Non-ADL slots remain unaffected.
    EXPECT_EQ(dest.int32_properties[0], 2);
    EXPECT_FLOAT_EQ(dest.float_properties[0], 50.0f);
}

TEST(NativeLoader, HasVtxConvertConceptDetectsAdlOverload) {
    // The concept must see the user's hook via ADL.
    static_assert(VTX::HasVtxConvert<vtx_native_loader_test::CustomVec3>,
                  "ADL must find to_vtx_value(CustomVec3) in its declaring namespace.");

    // Built-in / VTX-native types must NOT satisfy the concept (nobody defined
    // to_vtx_value for them, so LoadField goes through the normal switch).
    static_assert(!VTX::HasVtxConvert<int>);
    static_assert(!VTX::HasVtxConvert<float>);
    static_assert(!VTX::HasVtxConvert<std::string>);
    static_assert(!VTX::HasVtxConvert<VTX::Vector>);
    static_assert(!VTX::HasVtxConvert<VTX::Quat>);

    SUCCEED();
}
