// Tests for VTX::PropertyContainer and VTX::Helpers::CalculateContainerHash.
//
// The hash powers the differ's fast content_hash comparison, so determinism
// and cross-insensitivity to logically-equal state is critical.

#include <gtest/gtest.h>
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include "util/test_fixtures.h"

using VTX::PropertyContainer;
using VTX::Helpers::CalculateContainerHash;

// ---------------------------------------------------------------------------
// content_hash determinism
// ---------------------------------------------------------------------------

TEST(PropertyContainer, HashIsDeterministicAcrossCalls) {
    PropertyContainer a;
    a.entity_type_id = 0;
    a.string_properties = {"player_1"};
    a.float_properties = {100.0f};
    a.vector_properties = {VTX::Vector {1.0, 2.0, 3.0}};

    const uint64_t h1 = CalculateContainerHash(a);
    const uint64_t h2 = CalculateContainerHash(a);
    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, 0u);
}

TEST(PropertyContainer, HashDiffersWhenFloatValueChanges) {
    PropertyContainer a;
    a.entity_type_id = 0;
    a.float_properties = {100.0f};

    PropertyContainer b = a;
    b.float_properties = {101.0f};

    EXPECT_NE(CalculateContainerHash(a), CalculateContainerHash(b));
}

TEST(PropertyContainer, HashDiffersWhenStringChanges) {
    PropertyContainer a;
    a.string_properties = {"alpha"};
    PropertyContainer b;
    b.string_properties = {"bravo"};
    EXPECT_NE(CalculateContainerHash(a), CalculateContainerHash(b));
}

TEST(PropertyContainer, HashDiffersWhenTypeIdChanges) {
    PropertyContainer a;
    a.entity_type_id = 0;
    a.float_properties = {1.0f};

    PropertyContainer b = a;
    b.entity_type_id = 1;

    EXPECT_NE(CalculateContainerHash(a), CalculateContainerHash(b));
}

TEST(PropertyContainer, HashMatchesForEquivalentContainers) {
    PropertyContainer a = VtxTest::MakeSimpleEntity(0, 75.0f, {1.0, 2.0, 3.0}, "foo");
    PropertyContainer b = VtxTest::MakeSimpleEntity(0, 75.0f, {1.0, 2.0, 3.0}, "foo");
    EXPECT_EQ(a.content_hash, b.content_hash);
}

TEST(PropertyContainer, HashIncludesNestedAnyStruct) {
    PropertyContainer inner;
    inner.int32_properties = {42};

    PropertyContainer outer_a;
    outer_a.any_struct_properties = {inner};

    PropertyContainer outer_b;
    PropertyContainer inner_b;
    inner_b.int32_properties = {43};
    outer_b.any_struct_properties = {inner_b};

    EXPECT_NE(CalculateContainerHash(outer_a), CalculateContainerHash(outer_b));
}

// ---------------------------------------------------------------------------
// Default-state invariants
// ---------------------------------------------------------------------------

TEST(PropertyContainer, DefaultConstructedHasExpectedDefaults) {
    PropertyContainer pc;
    EXPECT_EQ(pc.entity_type_id, -1);
    EXPECT_EQ(pc.content_hash, 0u);
    EXPECT_TRUE(pc.bool_properties.empty());
    EXPECT_TRUE(pc.float_properties.empty());
    EXPECT_TRUE(pc.string_properties.empty());
    EXPECT_TRUE(pc.vector_properties.empty());
    EXPECT_TRUE(pc.quat_properties.empty());
    EXPECT_TRUE(pc.any_struct_properties.empty());
}
