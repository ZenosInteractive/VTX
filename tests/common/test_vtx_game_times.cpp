// Regression + behaviour tests for VTX::GameTime::VTXGameTimes.
//
// Several tests here pin fixes that landed on 2026-04-17 (see CHANGELOG):
//
//   Bug 1: AddTimeRegistry rejected the first UTC by comparing against
//          start_utc_ (system_clock::now at construction).
//   Bug 2: VTXGameTimes() constructor seeded start_utc_ to NOW, which made
//          any historical replay fail.
//   Bug 4: OnlyIncreasing/OnlyDecreasing filters rejected the first frame
//          whose game_time was 0 because the default "last" was also 0.
//
// These tests would have caught every one of them.

#include <gtest/gtest.h>
#include "vtx/common/vtx_types.h"

using VTX::GameTime::EFilterType;
using VTX::GameTime::GameTimeRegister;
using VTX::GameTime::VTXGameTimes;

// ---------------------------------------------------------------------------
// Regression: historical UTC must be accepted
// ---------------------------------------------------------------------------
//
// Pre-fix: VTXGameTimes() set start_utc_ = GetUtcNowTicks(), and
// AddTimeRegistry compared new UTC <= LastCreatedUtc() (which fell back to
// start_utc_ when empty).  Any historical timestamp -> rejected.
TEST(VTXGameTimes, AcceptsHistoricalUtcOnFirstFrame_Regression) {
    VTXGameTimes times;

    // A timestamp from April 2025 -- definitely < system_clock::now().
    const int64_t historical_utc = 1'745'000'000LL * 10'000'000LL;

    GameTimeRegister reg;
    reg.game_time = 0.0f;
    reg.created_utc_time = historical_utc;
    reg.FrameFilterType = EFilterType::OnlyIncreasing;

    EXPECT_TRUE(times.AddTimeRegistry(reg));
    EXPECT_EQ(times.LastCreatedUtc(), historical_utc);
}

// Same as above but with a "future" timestamp, to confirm the fix doesn't
// only accept the past -- anything is valid on frame 0.
TEST(VTXGameTimes, AcceptsFutureUtcOnFirstFrame) {
    VTXGameTimes times;
    const int64_t future_utc = VTXGameTimes::GetUtcNowTicks() + 1'000'000'000LL;

    GameTimeRegister reg;
    reg.game_time = 0.0f;
    reg.created_utc_time = future_utc;

    EXPECT_TRUE(times.AddTimeRegistry(reg));
}

// The whole point of the rejection -- once we have a prior frame, a new UTC
// that regresses must still be rejected.
TEST(VTXGameTimes, RejectsUtcRegressionAfterFirstFrame) {
    VTXGameTimes times;

    GameTimeRegister first;
    first.game_time = 0.0f;
    first.created_utc_time = 1'745'000'000'000'000'0LL; // 2025
    ASSERT_TRUE(times.AddTimeRegistry(first));

    // Simulate the "per-frame" finalize so add_used_ clears.
    times.ResolveGameTimes(1);

    GameTimeRegister regressing;
    regressing.game_time = 0.016f;
    regressing.created_utc_time = *first.created_utc_time - 1; // goes BACKWARD

    EXPECT_FALSE(times.AddTimeRegistry(regressing));
}

// ---------------------------------------------------------------------------
// Regression: OnlyIncreasing filter on game_time = 0 at frame 0
// ---------------------------------------------------------------------------
//
// Pre-fix: comparison was `new <= GetLastGameTimeSeconds()`, which returned 0
// when empty.  First frame with game_time=0 was therefore rejected.
TEST(VTXGameTimes, OnlyIncreasingAcceptsGameTimeZeroOnFirstFrame_Regression) {
    VTXGameTimes times;

    GameTimeRegister reg;
    reg.game_time = 0.0f;
    reg.FrameFilterType = EFilterType::OnlyIncreasing;

    EXPECT_TRUE(times.AddTimeRegistry(reg));
}

TEST(VTXGameTimes, OnlyDecreasingAcceptsGameTimeZeroOnFirstFrame_Regression) {
    VTXGameTimes times;

    GameTimeRegister reg;
    reg.game_time = 0.0f;
    reg.FrameFilterType = EFilterType::OnlyDecreasing;

    EXPECT_TRUE(times.AddTimeRegistry(reg));
}

// The filter itself must still enforce the policy on subsequent frames.
TEST(VTXGameTimes, OnlyIncreasingRejectsRegressionOnLaterFrame) {
    VTXGameTimes times;

    GameTimeRegister a;
    a.game_time = 1.0f;
    a.FrameFilterType = EFilterType::OnlyIncreasing;
    ASSERT_TRUE(times.AddTimeRegistry(a));
    times.ResolveGameTimes(1);

    GameTimeRegister b;
    b.game_time = 0.5f; // less than previous
    b.FrameFilterType = EFilterType::OnlyIncreasing;

    EXPECT_FALSE(times.AddTimeRegistry(b));
}

TEST(VTXGameTimes, OnlyDecreasingRejectsAscentOnLaterFrame) {
    VTXGameTimes times;

    GameTimeRegister a;
    a.game_time = 10.0f;
    a.FrameFilterType = EFilterType::OnlyDecreasing;
    ASSERT_TRUE(times.AddTimeRegistry(a));
    times.ResolveGameTimes(1);

    GameTimeRegister b;
    b.game_time = 20.0f; // greater than previous
    b.FrameFilterType = EFilterType::OnlyDecreasing;

    EXPECT_FALSE(times.AddTimeRegistry(b));
}

// ---------------------------------------------------------------------------
// Constructor state
// ---------------------------------------------------------------------------

TEST(VTXGameTimes, DefaultConstructedStartUtcIsZero_Regression) {
    // The old code seeded start_utc_ with GetUtcNowTicks().
    VTXGameTimes times;
    EXPECT_EQ(times.StartUtc(), 0);
    EXPECT_TRUE(times.IsEmpty());
}

TEST(VTXGameTimes, ClearResetsStartUtcToZero_Regression) {
    VTXGameTimes times;

    GameTimeRegister reg;
    reg.game_time = 0.0f;
    reg.created_utc_time = 1'745'000'000'000'000'0LL;
    ASSERT_TRUE(times.AddTimeRegistry(reg));

    times.Clear();
    EXPECT_TRUE(times.IsEmpty());
    EXPECT_EQ(times.StartUtc(), 0);
}

// ---------------------------------------------------------------------------
// Add+Resolve behaviour across multiple frames
// ---------------------------------------------------------------------------

TEST(VTXGameTimes, AcceptsManyMonotonicFrames) {
    VTXGameTimes times;
    const int64_t base_utc = 1'745'000'000LL * 10'000'000LL;
    constexpr int kFrames = 10;

    for (int i = 0; i < kFrames; ++i) {
        GameTimeRegister reg;
        reg.game_time = float(i) / 60.0f;
        reg.created_utc_time = base_utc + int64_t(i) * 166'666LL;
        reg.FrameFilterType = EFilterType::OnlyIncreasing;

        EXPECT_TRUE(times.AddTimeRegistry(reg)) << "frame " << i;
        times.ResolveGameTimes(i + 1);
    }

    EXPECT_EQ(times.GetCreatedUtc().size(), static_cast<size_t>(kFrames));
    EXPECT_EQ(times.LastCreatedUtc(), base_utc + int64_t(kFrames - 1) * 166'666LL);
}

TEST(VTXGameTimes, RejectsSecondAddInSameFrame) {
    VTXGameTimes times;

    GameTimeRegister a;
    a.game_time = 0.016f;
    ASSERT_TRUE(times.AddTimeRegistry(a));

    GameTimeRegister b;
    b.game_time = 0.032f;
    // No ResolveGameTimes() call yet -> add_used_ still set -> rejected.
    EXPECT_FALSE(times.AddTimeRegistry(b));
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

TEST(VTXGameTimes, SecondsToTicksMatchesConventional) {
    EXPECT_EQ(VTXGameTimes::SecondsToTicks(0.0f), 0);
    EXPECT_EQ(VTXGameTimes::SecondsToTicks(1.0f), 10'000'000);
    EXPECT_EQ(VTXGameTimes::SecondsToTicks(0.5f), 5'000'000);
}

TEST(VTXGameTimes, GetUtcNowTicksIsMonotonic) {
    const int64_t a = VTXGameTimes::GetUtcNowTicks();
    const int64_t b = VTXGameTimes::GetUtcNowTicks();
    EXPECT_GE(b, a);
    EXPECT_GT(a, 0);
}
