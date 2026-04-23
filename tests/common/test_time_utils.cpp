// Tests for low-level helpers in vtx_types_helpers.h.

#include <gtest/gtest.h>

#include <string>

#include "vtx/common/vtx_types_helpers.h"

namespace {

    size_t FieldIndex(VTX::FieldType type) {
        return static_cast<size_t>(type);
    }

} // namespace

TEST(TimeUtils, PreparePropertyContainerSizesVectorsFromSchema) {
    VTX::SchemaStruct schema;
    schema.type_max_indices.resize(FieldIndex(VTX::FieldType::Struct) + 1, 0);
    schema.type_max_indices[FieldIndex(VTX::FieldType::Bool)] = 2;
    schema.type_max_indices[FieldIndex(VTX::FieldType::Int32)] = 3;
    schema.type_max_indices[FieldIndex(VTX::FieldType::Float)] = 1;
    schema.type_max_indices[FieldIndex(VTX::FieldType::String)] = 2;
    schema.type_max_indices[FieldIndex(VTX::FieldType::Vector)] = 1;
    schema.type_max_indices[FieldIndex(VTX::FieldType::Quat)] = 1;
    schema.type_max_indices[FieldIndex(VTX::FieldType::Transform)] = 1;
    schema.type_max_indices[FieldIndex(VTX::FieldType::FloatRange)] = 1;
    schema.type_max_indices[FieldIndex(VTX::FieldType::Struct)] = 1;

    VTX::PropertyContainer pc;
    VTX::Helpers::PreparePropertyContainer(pc, schema);

    EXPECT_EQ(pc.bool_properties.size(), 2u);
    EXPECT_EQ(pc.int32_properties.size(), 3u);
    EXPECT_EQ(pc.float_properties.size(), 1u);
    EXPECT_EQ(pc.string_properties.size(), 2u);
    EXPECT_EQ(pc.vector_properties.size(), 1u);
    EXPECT_EQ(pc.quat_properties.size(), 1u);
    EXPECT_EQ(pc.transform_properties.size(), 1u);
    EXPECT_EQ(pc.range_properties.size(), 1u);
    EXPECT_EQ(pc.any_struct_properties.size(), 1u);
}

TEST(TimeUtils, ConvertToUeTicksSupportsNumericFormats) {
    using namespace VTX::TimeUtils;

    // Use int64{N} (not N##LL) to pick the int64 overload unambiguously.
    // On Linux x86_64 int64_t == long int, so LL literals are ambiguous
    // between the int64 and float overloads.  On MSVC int64_t == long long
    // so LL matches exactly -- this only bites us cross-platform.
    EXPECT_EQ(ConvertToUeTicks(TimeFormat::UeUTC, int64 {123456789}), 123456789LL);
    EXPECT_EQ(ConvertToUeTicks(TimeFormat::UnixUTC, int64 {0}), TICKS_AT_UNIX_EPOCH);
    EXPECT_EQ(ConvertToUeTicks(TimeFormat::UnixMsUTC, int64 {1}), TICKS_AT_UNIX_EPOCH + TICKS_PER_MILLISECOND);
    EXPECT_EQ(ConvertToUeTicks(TimeFormat::Seconds, 1.5f), 15'000'000LL);

    EXPECT_EQ(ConvertToUeTicks(TimeFormat::Seconds, int64 {7}), 0LL);
}

TEST(TimeUtils, ConvertToUeTicksParsesIso8601AndRejectsInvalidStrings) {
    using namespace VTX::TimeUtils;

    EXPECT_EQ(ConvertToUeTicks(TimeFormat::ISO8601, std::string("1970-01-01T00:00:00")), TICKS_AT_UNIX_EPOCH);
    EXPECT_EQ(ConvertToUeTicks(TimeFormat::ISO8601, std::string("invalid")), 0LL);
    EXPECT_EQ(ConvertToUeTicks(TimeFormat::UnixUTC, std::string("1970-01-01T00:00:00")), 0LL);
}

TEST(TimeUtils, DurationHelpersFormatPredictably) {
    using namespace VTX::TimeUtils;

    const int64 ticks = (3LL * 3600 * TICKS_PER_SECOND) + (2LL * 60 * TICKS_PER_SECOND) + (5LL * TICKS_PER_SECOND) +
                        (123LL * TICKS_PER_MILLISECOND);

    const auto duration = TicksToDuration(ticks);
    EXPECT_EQ(duration.hours, 3);
    EXPECT_EQ(duration.minutes, 2);
    EXPECT_EQ(duration.seconds, 5);
    EXPECT_EQ(duration.milliseconds, 123);

    EXPECT_EQ(FormatDuration(ticks), "3h 2m 5s 123ms");
    EXPECT_NEAR(TicksToSeconds(ticks), 10'925.123, 1e-9);
}

TEST(TimeUtils, FormatUtcTicksAcceptsUnixAndUeEpochTicks) {
    using namespace VTX::TimeUtils;

    const std::string unix_epoch = FormatUtcTicks(0);
    const std::string ue_epoch = FormatUtcTicks(TICKS_AT_UNIX_EPOCH);

    EXPECT_EQ(unix_epoch, ue_epoch);
    EXPECT_NE(unix_epoch.find("1970"), std::string::npos);
    EXPECT_TRUE(unix_epoch.ends_with("00:00:00.000 UTC"));
}

TEST(TimeUtils, CalculateContainerHashIncludesFlatArrayOffsets) {
    VTX::PropertyContainer a;
    a.int32_arrays.AppendSubArray({1, 2});
    a.int32_arrays.AppendSubArray({3});

    VTX::PropertyContainer b;
    b.int32_arrays.AppendSubArray({1});
    b.int32_arrays.AppendSubArray({2, 3});

    EXPECT_NE(VTX::Helpers::CalculateContainerHash(a), VTX::Helpers::CalculateContainerHash(b));
}
