// State-machine tests for VTX::GameTime::VTXGameTimes.
//
// Complements test_vtx_game_times.cpp by exercising combinations / edge
// transitions between Setup / Clear / AddTimeRegistry / ResolveGameTimes /
// CreateSnapshot / Rollback.

#include <gtest/gtest.h>
#include "vtx/common/vtx_types.h"

using VTX::GameTime::VTXGameTimes;
using VTX::GameTime::GameTimeRegister;
using VTX::GameTime::EFilterType;

namespace {

GameTimeRegister MakeReg(float game_time, int64_t utc = -1) {
    GameTimeRegister r;
    r.game_time = game_time;
    if (utc >= 0) r.created_utc_time = utc;
    r.FrameFilterType = EFilterType::OnlyIncreasing;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Rollback without prior CreateSnapshot
// ---------------------------------------------------------------------------

TEST(VTXGameTimesState, RollbackWithoutPriorSnapshotIsSafe) {
    // Calling Rollback() without ever calling CreateSnapshot() must not
    // crash or corrupt state.  It should effectively be a no-op (possibly
    // rolling back to the default-constructed state).
    VTXGameTimes t;

    ASSERT_TRUE(t.AddTimeRegistry(MakeReg(0.0f)));
    t.ResolveGameTimes(1);
    ASSERT_TRUE(t.AddTimeRegistry(MakeReg(0.1f)));
    t.ResolveGameTimes(2);

    t.Rollback();  // no snapshot -- must not crash
    SUCCEED();

    // After Rollback with no snapshot, we must still be able to Clear safely.
    t.Clear();
    EXPECT_TRUE(t.IsEmpty());
}

// ---------------------------------------------------------------------------
// Resolve with zero frames
// ---------------------------------------------------------------------------

TEST(VTXGameTimesState, ResolveGameTimesZeroFramesIsSafe) {
    VTXGameTimes t;
    // No frames added; Resolve(0) must not crash or corrupt.
    t.ResolveGameTimes(0);
    EXPECT_TRUE(t.IsEmpty());

    // Subsequent use must still work.
    EXPECT_TRUE(t.AddTimeRegistry(MakeReg(0.0f)));
    t.ResolveGameTimes(1);
    EXPECT_FALSE(t.IsEmpty());
}

// ---------------------------------------------------------------------------
// Setup after data has been added
// ---------------------------------------------------------------------------

TEST(VTXGameTimesState, SetupAfterDataAdded_DocumentsBehavior) {
    // Setup() is typically called before any AddTimeRegistry.  If someone
    // calls it AFTER adding data, we document the observed behaviour here so
    // a future change doesn't silently alter the contract.
    VTXGameTimes t;
    const int64_t base_utc = 1'745'000'000LL * 10'000'000LL;

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(t.AddTimeRegistry(MakeReg(float(i) * 0.016f, base_utc + i * 166'666LL)));
        t.ResolveGameTimes(i + 1);
    }
    ASSERT_FALSE(t.IsEmpty());

    // Call Setup AFTER data exists.  Observed contract: Setup changes config
    // (FPS, is_increasing, start_utc) but does NOT clear existing samples.
    // If this ever changes, this test will surface the regression.
    t.Setup(120.0f, true, 0);

    // The data from before must still be queryable.
    EXPECT_FALSE(t.IsEmpty());
    EXPECT_EQ(t.GetCreatedUtc().size(), 3u);
}

// ---------------------------------------------------------------------------
// Clear then reuse
// ---------------------------------------------------------------------------

TEST(VTXGameTimesState, ClearThenReuseReturnsToEmptyState) {
    VTXGameTimes t;
    ASSERT_TRUE(t.AddTimeRegistry(MakeReg(0.0f, 1000)));
    t.ResolveGameTimes(1);
    ASSERT_TRUE(t.AddTimeRegistry(MakeReg(0.016f, 2000)));
    t.ResolveGameTimes(2);

    t.Clear();
    EXPECT_TRUE(t.IsEmpty());
    EXPECT_EQ(t.StartUtc(), 0);   // regression for the 2026-04-17 fix

    // Fresh state must accept the same data again, including the first frame
    // being t=0 without triggering the monotonicity filter.
    EXPECT_TRUE(t.AddTimeRegistry(MakeReg(0.0f, 5000)));
    t.ResolveGameTimes(1);
    EXPECT_FALSE(t.IsEmpty());
    EXPECT_EQ(t.LastCreatedUtc(), 5000);
}

// ---------------------------------------------------------------------------
// CreateSnapshot + Rollback roundtrip
// ---------------------------------------------------------------------------

TEST(VTXGameTimesState, CreateSnapshotThenRollbackRestoresState) {
    VTXGameTimes t;
    ASSERT_TRUE(t.AddTimeRegistry(MakeReg(0.0f, 1000)));
    t.ResolveGameTimes(1);

    const size_t before_size = t.GetCreatedUtc().size();
    t.CreateSnapshot();

    // Add more data after the snapshot
    ASSERT_TRUE(t.AddTimeRegistry(MakeReg(0.016f, 2000)));
    t.ResolveGameTimes(2);
    ASSERT_TRUE(t.AddTimeRegistry(MakeReg(0.032f, 3000)));
    t.ResolveGameTimes(3);
    EXPECT_GT(t.GetCreatedUtc().size(), before_size);

    // Rollback must discard the post-snapshot entries.
    t.Rollback();
    EXPECT_EQ(t.GetCreatedUtc().size(), before_size);

    // After rollback we can resume adding data from the snapshot point.
    EXPECT_TRUE(t.AddTimeRegistry(MakeReg(0.016f, 2000)));
    t.ResolveGameTimes(2);
}

// ---------------------------------------------------------------------------
// InsertLiveChunkTimes with coherent chunks
// ---------------------------------------------------------------------------

TEST(VTXGameTimesState, InsertLiveChunkTimesPreservesMonotonicOrder) {
    // Build a first chunk (frames 0..4), then a second chunk (frames 5..9).
    // Insert the second into the first and confirm the combined timeline is
    // monotonic and has the right size.
    VTXGameTimes first;
    const int64_t base = 1'745'000'000LL * 10'000'000LL;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(first.AddTimeRegistry(MakeReg(float(i) / 60.0f, base + i * 166'666LL)));
        first.ResolveGameTimes(i + 1);
    }

    VTXGameTimes second;
    for (int i = 5; i < 10; ++i) {
        ASSERT_TRUE(second.AddTimeRegistry(MakeReg(float(i) / 60.0f, base + i * 166'666LL)));
        second.ResolveGameTimes(i - 4);
    }

    first.InsertLiveChunkTimes(second);
    EXPECT_EQ(first.GetCreatedUtc().size(), 10u);

    // Monotonic UTC throughout.
    const auto& utc = first.GetCreatedUtc();
    for (size_t i = 1; i < utc.size(); ++i) {
        EXPECT_GE(utc[i], utc[i - 1]) << "regression at index " << i;
    }
}
