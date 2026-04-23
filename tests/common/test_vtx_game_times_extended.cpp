// Additional behavioural tests for VTXGameTimes branches not covered by the
// regression-focused suite.

#include <gtest/gtest.h>

#include "vtx/common/vtx_types.h"

using VTX::GameTime::GameTimeRegister;
using VTX::GameTime::VTXGameTimes;

namespace {

    GameTimeRegister MakeBoth(float game_time, int64_t created_utc) {
        GameTimeRegister reg;
        reg.game_time = game_time;
        reg.created_utc_time = created_utc;
        return reg;
    }

} // namespace

TEST(VTXGameTimesExtended, SetupUsesExplicitStartUtcAndImplicitStartUtc) {
    VTXGameTimes explicit_start;
    explicit_start.Setup(60.0f, true, 123456789LL);
    EXPECT_EQ(explicit_start.StartUtc(), 123456789LL);

    VTXGameTimes implicit_start;
    implicit_start.Setup(60.0f, true);
    EXPECT_GT(implicit_start.StartUtc(), 0);
}

TEST(VTXGameTimesExtended, ResolveWithOnlyGameTimeSynthesizesIncreasingUtc) {
    VTXGameTimes times;
    times.Setup(2.0f, true, 1000LL);

    GameTimeRegister first;
    first.game_time = 1.0f;
    ASSERT_TRUE(times.AddTimeRegistry(first));
    ASSERT_TRUE(times.ResolveGameTimes(1));

    ASSERT_EQ(times.GetCreatedUtc().size(), 1u);
    EXPECT_EQ(times.GetCreatedUtc()[0], 10'001'000LL);

    GameTimeRegister second;
    second.game_time = 1.0f;
    ASSERT_TRUE(times.AddTimeRegistry(second));
    ASSERT_TRUE(times.ResolveGameTimes(2));

    ASSERT_EQ(times.GetCreatedUtc().size(), 2u);
    EXPECT_EQ(times.GetCreatedUtc()[1], times.GetCreatedUtc()[0] + 5'000'000LL);
}

TEST(VTXGameTimesExtended, ResolveWithOnlyCreatedUtcSynthesizesRelativeGameTime) {
    VTXGameTimes times;
    times.Setup(60.0f, true, 0);

    GameTimeRegister first;
    first.created_utc_time = 5000LL;
    ASSERT_TRUE(times.AddTimeRegistry(first));
    ASSERT_TRUE(times.ResolveGameTimes(1));

    GameTimeRegister second;
    second.created_utc_time = 7250LL;
    ASSERT_TRUE(times.AddTimeRegistry(second));
    ASSERT_TRUE(times.ResolveGameTimes(2));

    ASSERT_EQ(times.GetGameTime().size(), 2u);
    EXPECT_EQ(times.GetGameTime()[0], 0);
    EXPECT_EQ(times.GetGameTime()[1], 2250LL);
}

TEST(VTXGameTimesExtended, ResolveWithoutInputsSynthesizesBothTimesFromFps) {
    VTXGameTimes times;
    times.Setup(2.0f, true, 1000LL);

    ASSERT_TRUE(times.ResolveGameTimes(1));
    ASSERT_TRUE(times.ResolveGameTimes(2));

    ASSERT_EQ(times.GetGameTime().size(), 2u);
    EXPECT_EQ(times.GetGameTime()[0], 0);
    EXPECT_EQ(times.GetGameTime()[1], 5'000'000LL);

    ASSERT_EQ(times.GetCreatedUtc().size(), 2u);
    EXPECT_EQ(times.GetCreatedUtc()[0], 1000LL);
    EXPECT_EQ(times.GetCreatedUtc()[1], 5'001'000LL);
}

TEST(VTXGameTimesExtended, DetectsTimelineGapWhenUtcJumpExceedsThreshold) {
    VTXGameTimes times;
    times.Setup(60.0f, true, 0);

    ASSERT_TRUE(times.AddTimeRegistry(MakeBoth(0.0f, 1'000'000LL)));
    ASSERT_TRUE(times.ResolveGameTimes(1));

    ASSERT_TRUE(times.AddTimeRegistry(MakeBoth(1.0f / 60.0f, 1'666'664LL)));
    ASSERT_TRUE(times.ResolveGameTimes(2));

    ASSERT_EQ(times.GetTimelineGaps().size(), 1u);
    EXPECT_EQ(times.GetTimelineGaps()[0], 2);
}

TEST(VTXGameTimesExtended, DetectsGameSegmentWhenGameTimeDirectionReverses) {
    VTXGameTimes times;
    times.Setup(60.0f, true, 0);

    ASSERT_TRUE(times.AddTimeRegistry(MakeBoth(1.0f, 10'000'000LL)));
    ASSERT_TRUE(times.ResolveGameTimes(1));

    ASSERT_TRUE(times.AddTimeRegistry(MakeBoth(0.5f, 20'000'000LL)));
    ASSERT_TRUE(times.ResolveGameTimes(2));

    ASSERT_EQ(times.GetGameSegments().size(), 1u);
    EXPECT_EQ(times.GetGameSegments()[0], 2);
}

TEST(VTXGameTimesExtended, CopyFromKeepsOnlyCurrentChunkAndInsertLiveChunkTimesAppends) {
    VTXGameTimes source;
    source.Setup(60.0f, true, 1000LL);

    ASSERT_TRUE(source.AddTimeRegistry(MakeBoth(0.0f, 1000LL)));
    ASSERT_TRUE(source.ResolveGameTimes(1));
    ASSERT_TRUE(source.AddTimeRegistry(MakeBoth(1.0f, 2000LL)));
    ASSERT_TRUE(source.ResolveGameTimes(2));

    source.UpdateChunkStartIndex();

    ASSERT_TRUE(source.AddTimeRegistry(MakeBoth(2.0f, 3000LL)));
    ASSERT_TRUE(source.ResolveGameTimes(3));
    source.ManuallyMarkGameSegmentStart(3);

    VTXGameTimes copy;
    copy.CopyFrom(source);

    ASSERT_EQ(copy.GetGameTime().size(), 1u);
    EXPECT_EQ(copy.GetGameTime()[0], source.GetGameTime().back());
    ASSERT_EQ(copy.GetCreatedUtc().size(), 1u);
    EXPECT_EQ(copy.GetCreatedUtc()[0], 3000LL);
    ASSERT_EQ(copy.GetGameSegments().size(), 1u);
    EXPECT_EQ(copy.GetGameSegments()[0], 3);

    VTXGameTimes appended;
    appended.InsertLiveChunkTimes(copy);
    ASSERT_EQ(appended.GetCreatedUtc().size(), 1u);
    EXPECT_EQ(appended.GetCreatedUtc()[0], 3000LL);
}

TEST(VTXGameTimesExtended, SnapshotRollbackRestoresPreviousState) {
    VTXGameTimes times;
    times.Setup(60.0f, true, 1000LL);

    times.CreateSnapshot();
    ASSERT_TRUE(times.AddTimeRegistry(MakeBoth(0.25f, 2000LL)));
    ASSERT_FALSE(times.IsEmpty());

    times.Rollback();
    EXPECT_TRUE(times.IsEmpty());
    EXPECT_TRUE(times.GetGameTime().empty());
    EXPECT_TRUE(times.GetCreatedUtc().empty());
}

TEST(VTXGameTimesExtended, ResolveRejectsZeroFrameCount) {
    VTXGameTimes times;
    EXPECT_FALSE(times.ResolveGameTimes(0));
}
