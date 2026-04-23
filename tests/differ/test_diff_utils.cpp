// Tests for header-only differ helpers and path utilities.

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string>

#include "vtx/differ/core/vtx_diff_types.h"

namespace {

    template <typename T>
    std::span<const std::byte> BytesOf(const T& value) {
        return std::as_bytes(std::span(&value, 1));
    }

    template <typename T, size_t N>
    std::span<const std::byte> BytesOf(const std::array<T, N>& value) {
        return std::as_bytes(std::span(value));
    }

} // namespace

TEST(DiffUtils, TypeToFieldNameReturnsExpectedNames) {
    EXPECT_EQ(VtxDiff::TypeToFieldName(VtxDiff::EVTXContainerType::FloatProperties), "float_properties");
    EXPECT_EQ(VtxDiff::TypeToFieldName(VtxDiff::EVTXContainerType::AnyStructArrays), "any_struct_arrays");
    EXPECT_EQ(VtxDiff::TypeToFieldName(VtxDiff::EVTXContainerType::Unknown), "Unknown");
    EXPECT_EQ(VtxDiff::TypeToFieldName(static_cast<VtxDiff::EVTXContainerType>(255)), "");
}

TEST(DiffUtils, PathHelpersBuildCanonicalRepresentations) {
    VtxDiff::PathS path("Root");
    EXPECT_EQ(path.Append("Field").Index(3).Key("player_0").ToString(), "Root.Field[3]{player_0}");

    EXPECT_EQ(VtxDiff::MakeFieldPath("", "Field"), "Field");
    EXPECT_EQ(VtxDiff::MakeFieldPath("Root", "Field"), "Root.Field");
    EXPECT_EQ(VtxDiff::MakeIndexPath("Root", 2), "Root[2]");
    EXPECT_EQ(VtxDiff::MakeMapKeyPath("Root", "Key"), "Root{Key}");
    EXPECT_TRUE(VtxDiff::EndsWith("Root.Field", ".Field"));
    EXPECT_FALSE(VtxDiff::EndsWith("Root.Field", ".Other"));
}

TEST(DiffUtils, IsArraysTypeRecognizesArrayContainers) {
    EXPECT_TRUE(VtxDiff::IsArraysType(VtxDiff::EVTXContainerType::Int32Arrays));
    EXPECT_TRUE(VtxDiff::IsArraysType(VtxDiff::EVTXContainerType::AnyStructArrays));
    EXPECT_FALSE(VtxDiff::IsArraysType(VtxDiff::EVTXContainerType::Int32Properties));
    EXPECT_FALSE(VtxDiff::IsArraysType(VtxDiff::EVTXContainerType::Unknown));
}

TEST(DiffUtils, AreScalarsEqualUsesEpsilonAndFallsBackToByteComparison) {
    VtxDiff::DiffOptions relaxed;
    relaxed.compare_floats_with_epsilon = true;
    relaxed.float_epsilon = 1e-3f;

    const float fa = 1.0f;
    const float fb = 1.0005f;
    EXPECT_TRUE(
        VtxDiff::AreScalarsEqual(VtxDiff::EVTXContainerType::FloatProperties, BytesOf(fa), BytesOf(fb), relaxed));

    std::array<float, 3> vec_a = {1.0f, 2.0f, 3.0f};
    std::array<float, 3> vec_b = {1.0f, 2.0005f, 3.0f};
    EXPECT_TRUE(VtxDiff::AreScalarsEqual(VtxDiff::EVTXContainerType::VectorProperties, BytesOf(vec_a), BytesOf(vec_b),
                                         relaxed));

    std::array<float, 4> quat_a = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> quat_b = {0.0f, 0.0f, 0.0f, 1.0005f};
    EXPECT_TRUE(VtxDiff::AreScalarsEqual(VtxDiff::EVTXContainerType::QuatProperties, BytesOf(quat_a), BytesOf(quat_b),
                                         relaxed));

    VtxDiff::DiffOptions strict;
    strict.compare_floats_with_epsilon = false;
    EXPECT_FALSE(
        VtxDiff::AreScalarsEqual(VtxDiff::EVTXContainerType::FloatProperties, BytesOf(fa), BytesOf(fb), strict));

    const int32_t ia = 7;
    const int32_t ib = 8;
    EXPECT_TRUE(
        VtxDiff::AreScalarsEqual(VtxDiff::EVTXContainerType::Int32Properties, BytesOf(ia), BytesOf(ia), relaxed));
    EXPECT_FALSE(
        VtxDiff::AreScalarsEqual(VtxDiff::EVTXContainerType::Int32Properties, BytesOf(ia), BytesOf(ib), relaxed));
}
