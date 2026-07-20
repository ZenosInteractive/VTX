// Reader facade API coverage beyond OpenReplayFile smoke tests.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "util/test_fixtures.h"

namespace {

    const char* FormatName(VTX::VtxFormat format) {
        return format == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf";
    }

    std::string UniqueOutputPath(VTX::VtxFormat format, const std::string& suffix) {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        return VtxTest::OutputPath(VtxTest::SanitizePathComponent(std::string(info->test_suite_name())) + "_" +
                                   VtxTest::SanitizePathComponent(std::string(info->name())) + "_" +
                                   FormatName(format) + "_" + suffix + ".vtx");
    }

    VTX::WriterFacadeConfig MakeConfig(VTX::VtxFormat format, const std::string& suffix, int32_t chunk_max_frames) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = UniqueOutputPath(format, suffix);
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "ReaderApiTest";
        cfg.replay_uuid = "reader-api";
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = chunk_max_frames;
        cfg.use_compression = true;
        return cfg;
    }

    std::unique_ptr<VTX::IVtxWriterFacade> CreateWriter(VTX::VtxFormat format, const VTX::WriterFacadeConfig& cfg) {
        return format == VTX::VtxFormat::FlatBuffers ? VTX::CreateFlatBuffersWriterFacade(cfg)
                                                     : VTX::CreateProtobufWriterFacade(cfg);
    }

    VTX::Frame MakePlayerFrame(int frame_index) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {"player_0", "Alpha"};
        pc.int32_properties = {1, frame_index, 0};
        pc.float_properties = {100.0f - float(frame_index), 50.0f};
        pc.vector_properties = {VTX::Vector {double(frame_index), 0.0, 0.0}, VTX::Vector {1.0, 0.0, 0.0}};
        pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
        pc.bool_properties = {true};

        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

    void WriteReplay(VTX::VtxFormat format, const std::string& path, int frames, int32_t chunk_max_frames) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = path;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "ReaderApiTest";
        cfg.replay_uuid = "reader-api";
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = chunk_max_frames;
        cfg.use_compression = true;

        auto writer = CreateWriter(format, cfg);
        for (int i = 0; i < frames; ++i) {
            auto frame = MakePlayerFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }

    int ReadScore(const VTX::Frame& frame) {
        return frame.GetBuckets()[0].entities[0].int32_properties[1];
    }

    // A frame carrying many entities so a loaded chunk is expensive to free -- used to
    // exercise the off-thread eviction path (freeing on the caller thread would stall).
    VTX::Frame MakeHeavyFrame(int frame_index, int entity_count) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.entities.reserve(entity_count);
        bucket.unique_ids.reserve(entity_count);
        for (int e = 0; e < entity_count; ++e) {
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.string_properties = {"player_" + std::to_string(e), "Alpha"};
            pc.int32_properties = {1, frame_index, e};
            pc.float_properties = {100.0f - float(frame_index), 50.0f};
            pc.vector_properties = {VTX::Vector {double(e), 0.0, 0.0}, VTX::Vector {1.0, 0.0, 0.0}};
            pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
            pc.bool_properties = {true};
            bucket.unique_ids.push_back("player_" + std::to_string(e));
            bucket.entities.push_back(std::move(pc));
        }
        return f;
    }

    void WriteHeavyReplay(VTX::VtxFormat format, const std::string& path, int frames, int32_t chunk_max_frames,
                          int entity_count) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = path;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "ReaderApiTest";
        cfg.replay_uuid = "reader-api";
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = chunk_max_frames;
        cfg.use_compression = true;

        auto writer = CreateWriter(format, cfg);
        for (int i = 0; i < frames; ++i) {
            auto frame = MakeHeavyFrame(i, entity_count);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }

} // namespace

class ReaderApiTest : public ::testing::TestWithParam<VTX::VtxFormat> {};

TEST_P(ReaderApiTest, CreateAccessorReadsSchemaDrivenEntityValues) {
    const auto cfg = MakeConfig(GetParam(), "accessor", 8);
    WriteReplay(GetParam(), cfg.output_filepath, 1, 8);

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;

    const auto accessor = ctx.reader->CreateAccessor();
    const VTX::Frame* frame = ctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    ASSERT_FALSE(frame->GetBuckets().empty());

    const VTX::EntityView entity(frame->GetBuckets()[0].entities[0]);
    const auto name_key = accessor.Get<std::string>("Player", "Name");
    const auto score_key = accessor.Get<int32_t>("Player", "Score");
    const auto health_key = accessor.Get<float>("Player", "Health");
    const auto alive_key = accessor.Get<bool>("Player", "IsAlive");

    EXPECT_EQ(entity.Get(name_key), "Alpha");
    EXPECT_EQ(entity.Get(score_key), 0);
    EXPECT_FLOAT_EQ(entity.Get(health_key), 100.0f);
    EXPECT_TRUE(entity.Get(alive_key));

    EXPECT_TRUE(accessor.HasProperty("Player", "Position"));
    EXPECT_EQ(accessor.GetPropertiesForStruct("Player").size(), 11u);
    EXPECT_FALSE(accessor.Get<float>("Player", "Name").IsValid());
}

TEST_P(ReaderApiTest, FrameRangeAndContextReturnExpectedFrames) {
    const auto cfg = MakeConfig(GetParam(), "range_context", 8);
    WriteReplay(GetParam(), cfg.output_filepath, 5, 8);

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    ASSERT_NE(ctx.reader->GetFrameSync(0), nullptr);

    std::vector<VTX::Frame> range;
    ctx.reader->GetFrameRange(1, 2, range);
    ASSERT_EQ(range.size(), 3u);
    EXPECT_EQ(ReadScore(range[0]), 1);
    EXPECT_EQ(ReadScore(range[1]), 2);
    EXPECT_EQ(ReadScore(range[2]), 3);

    const auto context = ctx.reader->GetFrameContext(2, 1, 1);
    ASSERT_EQ(context.size(), 3u);
    EXPECT_EQ(ReadScore(context[0]), 1);
    EXPECT_EQ(ReadScore(context[1]), 2);
    EXPECT_EQ(ReadScore(context[2]), 3);
}

TEST_P(ReaderApiTest, OutOfBoundsQueriesReturnEmptyResults) {
    const auto cfg = MakeConfig(GetParam(), "oob", 8);
    WriteReplay(GetParam(), cfg.output_filepath, 2, 8);

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;

    EXPECT_EQ(ctx.reader->GetFrameSync(99), nullptr);
    EXPECT_TRUE(ctx.reader->GetRawFrameBytes(99).empty());

    std::vector<VTX::Frame> range;
    ctx.reader->GetFrameRange(99, 2, range);
    EXPECT_TRUE(range.empty());
}

TEST(ReaderApiFlatBuffers, CacheWindowZeroEvictsPreviousChunks) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_CacheWindowZeroEvictsPreviousChunks.vtx");
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 3, 1);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;

    ctx.reader->SetCacheWindow(0, 0);
    EXPECT_EQ(ctx.reader->GetChunkFrameCountSafe(0), 0);

    ASSERT_NE(ctx.reader->GetFrameSync(0), nullptr);
    EXPECT_EQ(ctx.reader->GetChunkFrameCountSafe(0), 1);

    auto snap = ctx.chunk_state->GetSnapshot();
    ASSERT_EQ(snap.loaded_chunks.size(), 1u);
    EXPECT_EQ(snap.loaded_chunks[0], 0);
    EXPECT_TRUE(snap.loading_chunks.empty());

    ASSERT_NE(ctx.reader->GetFrameSync(2), nullptr);
    EXPECT_EQ(ctx.reader->GetChunkFrameCountSafe(0), 0);
    EXPECT_EQ(ctx.reader->GetChunkFrameCountSafe(2), 1);

    snap = ctx.chunk_state->GetSnapshot();
    ASSERT_EQ(snap.loaded_chunks.size(), 1u);
    EXPECT_EQ(snap.loaded_chunks[0], 2);
    EXPECT_TRUE(snap.loading_chunks.empty());
}

// §1.B regression coverage.  Under random access the reader must NOT
// prefetch lateral chunks: the EWMA of chunk-index distance detects
// the pattern and the trigger loop in UpdateCacheWindow is skipped
// once the EWMA exceeds window size.
//
// We can't read the EWMA directly, but we can count
// OnChunkLoadStarted callbacks and compare two runs:
//
//   - Sequential read of N chunks with cache_window=(2,2) fires
//     ~N+4 callbacks (N for the sequential trace plus a trailing
//     window of 4 lateral prefetches that are kept in cache).
//   - Random read of N distant chunks with cache_window=(2,2) would
//     fire ~5*N callbacks pre-fix (every jump repopulates the full
//     window).  Post-fix the EWMA kicks in after ~2 samples and
//     subsequent jumps load just the target.
//
// The assertion is conservative: random loads <= 2*N + small slack
// rather than a tight bound, because the first two samples bootstrap
// the EWMA and still trigger laterals.  This catches a regression of
// the EWMA logic (hitting >= 5*N) while tolerating reasonable
// variations in bootstrap behaviour.
TEST(ReaderApiFlatBuffers, RandomAccessSkipsLateralPrefetches) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_RandomAccessSkipsLateralPrefetches.vtx");
    // 20 chunks of 5 frames each -> 100 frames total.  Small enough
    // to run in milliseconds, big enough that jumps of 10 exceed the
    // window of 2.
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 100, 5);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;

    ctx.reader->SetCacheWindow(2, 2);

    // The indices below map to 10 chunks, each jump distance 10 chunks
    // (well above window=2), guaranteeing the EWMA crosses its
    // threshold after ~2 jumps.
    const std::vector<int32_t> jump_frames {0, 50, 10, 60, 20, 70, 30, 80, 40, 90};

    for (int32_t f : jump_frames) {
        ASSERT_NE(ctx.reader->GetFrameSync(f), nullptr);
    }

    // Under pre-§1.B behaviour this would be ~50 (5 chunks per jump).
    // Under post-§1.B behaviour we expect close to the number of
    // jumps plus the small bootstrap tail of ~2-3 over-prefetched
    // chunks from the first couple of jumps before the EWMA stabilises.
    auto snap = ctx.chunk_state->GetSnapshot();
    const size_t total_seen = snap.loaded_chunks.size() + snap.loading_chunks.size();

    // Conservative bound: no more than 2x the jump count.  Pre-fix
    // this would be ~5x.
    EXPECT_LE(total_seen, jump_frames.size() * 2)
        << "Expected random access to skip lateral prefetches "
        << "but saw " << total_seen << " chunks loaded vs " << jump_frames.size() << " jumps.";
}

// Regression for the "stale-cancelled prefetch blocks re-entry" bug.
//
// Sequence that hits the race:
//   1. GetFrameSync(frame_in_chunk_0) -- triggers chunk 0 (sync) plus
//      lateral prefetches of chunks 1 and 2.  Workers for 1 and 2 are
//      queued on std::async but may not have started yet.
//   2. GetFrameSync(frame_in_chunk_10) -- window shifts away from 0.
//      The UpdateCacheWindow cancel loop calls request_stop() on the
//      PendingLoads for chunks 1 and 2; their entries remain in
//      pending_loads_ until the workers exit and the next reap sweep
//      picks them up.
//   3. GetFrameSync(frame_in_chunk_2) -- chunk 2 is back in the window.
//      Pre-fix: trigger() saw pending_loads_[2] and skipped spawning a
//      new task; worker 2 eventually ran, observed stop_requested(),
//      bailed at its entry check, and the chunk_cache_ write was
//      skipped.  GetFramePtrSync waited on the future, it resolved,
//      cache was empty, returned nullptr.
//   4. Post-fix: trigger() detects pending_loads_[2] has its stop
//      already requested and respawns with a fresh stop_source; the
//      orphaned worker exits on its own, the new worker populates the
//      cache, GetFramePtrSync returns the frame.
//
// The race is timing-dependent, so we run the pattern 50 iterations.
// Under TSan's scheduler overhead a single iteration suffices; under
// stock release it is a flaky single-digit-% race and 50 reps push
// the miss probability below the CI flake floor.
TEST(ReaderApiFlatBuffers, CancelledPrefetchReEntersWindow) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_CancelledPrefetchReEntersWindow.vtx");
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 100, 5); // 20 chunks * 5 frames

    constexpr int kIters = 50;
    for (int iter = 0; iter < kIters; ++iter) {
        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << "iter=" << iter << " " << ctx.error;
        ctx.reader->SetCacheWindow(2, 2);

        // Step 1: prime chunks 0..2 (chunk 0 sync + 1,2 as laterals).
        ASSERT_NE(ctx.reader->GetFrameSync(0), nullptr) << "iter=" << iter << " step=1";
        // Step 2: jump far away -> cancels 1 and 2 before they run.
        ASSERT_NE(ctx.reader->GetFrameSync(50), nullptr) << "iter=" << iter << " step=2";
        // Step 3: jump back to a cancelled chunk.  Pre-fix returns null.
        ASSERT_NE(ctx.reader->GetFrameSync(10), nullptr) << "iter=" << iter << " step=3";
    }
}

// Regression for the "async view path wedges on a cross-range jump" bug.
//
// The inspector UI reads frames through the NON-blocking accessor
// (GetFramePtr): every render tick it calls GetFrame(current) and, on
// null, shows "Loading chunk data...". The load is expected to be
// driven forward by the UpdateCacheWindow call inside GetFramePtr.
//
// Pre-fix sequence that wedged forever:
//   1. GetFrame(chunk 0) primes chunks 0,1,2 as in-flight prefetches
//      (max_concurrent_loads == 3, all slots taken).
//   2. GetFrame(far chunk) shifts the window. UpdateCacheWindow committed
//      current_range_ to the new window, but trigger(target) was skipped
//      because the 3 not-yet-reaped (now stop-requested) prefetch slots
//      were full.  The target was neither cached nor pending.
//   3. Every later GetFrame(far chunk) hit the range-equality guard
//      (range unchanged) and returned early BEFORE re-triggering, so the
//      target chunk was never scheduled again -> null forever.
//
// Post-fix: the viewed chunk is triggered uncapped and ahead of the
// range-equality guard, so the async path self-heals and the frame
// eventually resolves.  Timing-dependent, so run several iterations.
TEST(ReaderApiFlatBuffers, AsyncViewPathResolvesAfterCrossRangeJump) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_AsyncViewPathResolvesAfterCrossRangeJump.vtx");
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 100, 5); // 20 chunks * 5 frames

    constexpr int kIters = 30;
    for (int iter = 0; iter < kIters; ++iter) {
        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << "iter=" << iter << " " << ctx.error;
        ctx.reader->SetCacheWindow(2, 2);

        // Prime chunk 0 and kick its lateral prefetches (fills the load slots).
        (void)ctx.reader->GetFrame(0);

        // Jump far outside the cached window; frame 90 lives in chunk 18.
        const int32_t target = 90;
        const VTX::Frame* frame = nullptr;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            // Re-poll exactly as the UI render loop does; each call re-enters
            // UpdateCacheWindow and must keep the target load progressing.
            frame = ctx.reader->GetFrame(target);
            if (frame) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_NE(frame, nullptr) << "iter=" << iter << " async view path wedged on cross-range jump";
    }
}

// Regression for the coupled "loaded set grows without bound" leak.
//
// A cancelled load (window shifted away before it became resident) used
// to still fire OnChunkLoadFinished, marking the chunk "loaded (RAM)"
// even though it was never written to chunk_cache_.  Eviction only walks
// chunk_cache_, so OnChunkEvicted never fired for that phantom entry and
// the loaded set climbed every time a jump cancelled an in-flight load
// (the "index slowly increasing to infinity" symptom).  Post-fix a
// cancelled load fires OnChunkLoadCancelled instead, which clears the
// loading entry without ever marking it loaded.
TEST(ReaderApiFlatBuffers, AsyncRandomJumpsDoNotLeakLoadedSet) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_AsyncRandomJumpsDoNotLeakLoadedSet.vtx");
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 200, 5); // 40 chunks * 5 frames

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx.reader->SetCacheWindow(2, 2);

    // Alternating far jumps maximise the number of prefetches cancelled
    // mid-flight -- the exact condition that used to leak phantom entries.
    const std::vector<int32_t> jump_frames {0,  190, 10, 180, 20, 170, 30, 160, 40, 150,
                                            50, 140, 60, 130, 70, 120, 80, 110, 90, 100};
    for (int32_t f : jump_frames) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline && ctx.reader->GetFrame(f) == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Let any stop-requested workers drain, then run one more window update
    // so the final reap/eviction sweep settles.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    (void)ctx.reader->GetFrame(jump_frames.back());

    // Resident set is bounded by the cache window (backward 2 + forward 2 +
    // the current chunk = 5) plus a little slack for loads still settling.
    // The phantom leak would push this toward the 40-chunk total.
    auto snap = ctx.chunk_state->GetSnapshot();
    EXPECT_LE(snap.loaded_chunks.size(), 12u)
        << "loaded set leaked phantom (never-resident) chunks: size=" << snap.loaded_chunks.size();
}

// §3.A regression coverage.  WarmAt must trigger an asynchronous load
// of the chunk containing `frame_index` without blocking the caller,
// and without requiring a subsequent GetFrame to fire the load.
TEST(ReaderApiFlatBuffers, WarmAtTriggersAsyncLoadWithoutReading) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_WarmAtTriggersAsyncLoadWithoutReading.vtx");
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 50, 10); // 5 chunks

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;

    // Isolate the effect of WarmAt: no window prefetching around it.
    ctx.reader->SetCacheWindow(0, 0);

    // No chunk loaded yet.
    EXPECT_EQ(ctx.reader->GetChunkFrameCountSafe(3), 0);

    // WarmAt hints the reader that we're about to touch frame 30
    // (which lives in chunk 3 given chunk_max_frames=10).  Returns
    // immediately; the load runs on a worker thread.
    ctx.reader->WarmAt(30);

    // Drain the async load by polling the snapshot.  In practice this
    // finishes in a few ms; cap the wait at a generous 5s to keep the
    // test robust on slow CI runners.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto snap = ctx.chunk_state->GetSnapshot();
        if (std::find(snap.loaded_chunks.begin(), snap.loaded_chunks.end(), 3) != snap.loaded_chunks.end()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(ctx.reader->GetChunkFrameCountSafe(3), 10);

    auto snap = ctx.chunk_state->GetSnapshot();
    ASSERT_FALSE(snap.loaded_chunks.empty());
    EXPECT_NE(std::find(snap.loaded_chunks.begin(), snap.loaded_chunks.end(), 3), snap.loaded_chunks.end());
}

// Regression for the "stale-frame read cancels the target load" wedge.
//
// The inspector reads the current frame every tick (which drives the cache
// window) and, while that frame streams, shows the last-drawn frame from a
// DIFFERENT chunk. A plain GetFrame for that stale read moves the window back
// and cancels the target's load -- every tick -- so the target never becomes
// resident (stuck on "Loading...", pending flashing). GetResidentFrame is a
// side-effect-free read for exactly this case: it never moves the window, so
// the target load runs to completion.
TEST(ReaderApiFlatBuffers, ResidentFramePeekDoesNotCancelTargetLoad) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_ResidentFramePeekDoesNotCancelTargetLoad.vtx");
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 100, 5); // 20 chunks

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx.reader->SetCacheWindow(2, 2);

    (void)ctx.reader->GetFrame(0); // establishes a resident "last drawn" chunk 0

    const int32_t current = 90; // chunk 18, far from chunk 0
    const int32_t stale = 0;    // chunk 0

    const VTX::Frame* f = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        f = ctx.reader->GetFrame(current); // window-driving read (loads the target)
        if (f) {
            break;
        }
        // Stale fallback via the peek: must NOT move the window or cancel the target.
        (void)ctx.reader->GetResidentFrame(stale);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_NE(f, nullptr) << "target frame never became resident -- stale peek is cancelling the load";
}

// GetResidentFrame must never trigger a load: a chunk that has not been brought
// in by a window-driving read stays absent when only peeked.
TEST(ReaderApiFlatBuffers, ResidentFramePeekNeverTriggersLoad) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_ResidentFramePeekNeverTriggersLoad.vtx");
    WriteReplay(VTX::VtxFormat::FlatBuffers, path, 100, 5); // 20 chunks

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx.reader->SetCacheWindow(0, 0);

    // Peeking a never-touched far chunk returns null and schedules nothing.
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(ctx.reader->GetResidentFrame(90), nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto snap = ctx.chunk_state->GetSnapshot();
    EXPECT_TRUE(snap.loading_chunks.empty()) << "peek scheduled a load (loading set non-empty)";
    EXPECT_EQ(std::find(snap.loaded_chunks.begin(), snap.loaded_chunks.end(), 18), snap.loaded_chunks.end())
        << "peek brought chunk 18 resident";
}

// Regression for the "eviction frees on the caller thread" freeze. Freeing a chunk
// destroys every frame's entities (nested heap allocations) and, for real captures,
// cost hundreds of ms to >1s -- a visible UI stall on every cross-range jump once
// chunks actually became resident. Eviction now only unlinks on the caller thread and
// hands the owned data to a background task to destruct, so the jump call returns fast.
//
// Self-calibrating: we first time how long it takes to bring a heavy chunk in
// (GetFrameSync), then assert the cross-range jump call (which evicts a full resident
// window) is a small fraction of that. Synchronous eviction would be on the order of a
// load; off-thread eviction is near-instant.
TEST(ReaderApiFlatBuffers, EvictionDoesNotFreeOnCallerThread) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_EvictionDoesNotFreeOnCallerThread.vtx");
    // 8 chunks x 250 frames x 300 entities -> a resident window is heavy to free.
    WriteHeavyReplay(VTX::VtxFormat::FlatBuffers, path, 2000, 250, 300);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx.reader->SetCacheWindow(3, 3);

    // Baseline: cost to bring one heavy chunk fully resident (I/O + decompress + build).
    const auto load_t0 = std::chrono::steady_clock::now();
    ASSERT_NE(ctx.reader->GetFrameSync(0), nullptr); // chunk 0
    const double load_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_t0).count();

    // Make a full window around chunk 0 resident, then jump far. The jump call evicts
    // the entire resident window; with the fix it must not free on this thread.
    for (int i = 0; i < 200; ++i) {
        (void)ctx.reader->GetFrame(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto jump_t0 = std::chrono::steady_clock::now();
    (void)ctx.reader->GetFrame(1750); // chunk 7, far outside [0-3, 0+3]
    const double jump_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - jump_t0).count();

    // The jump (eviction) must be far cheaper than a load. Synchronous freeing of a
    // multi-chunk window would be comparable to (or exceed) a single load; off-thread
    // eviction is orders of magnitude less. Generous factor to stay CI-robust.
    EXPECT_LT(jump_ms, std::max(50.0, load_ms))
        << "cross-range jump took " << jump_ms << "ms (load baseline " << load_ms
        << "ms) -- eviction appears to be freeing on the caller thread";
}

// Regression for the "closing the app hangs ~10s" freeze. The reader destructor used
// to free the resident chunk cache inline; for real captures that is hundreds of ms to
// seconds. It now hands the resident data to a detached background thread and returns,
// waiting only for load workers (which touch `this`). So destruction is near-instant
// regardless of how much is resident.
TEST(ReaderApiFlatBuffers, DestructionDoesNotBlockOnResidentCache) {
    const auto path = VtxTest::OutputPath("ReaderApiFlatBuffers_DestructionDoesNotBlockOnResidentCache.vtx");
    WriteHeavyReplay(VTX::VtxFormat::FlatBuffers, path, 2000, 250, 300); // 8 heavy chunks

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx.reader->SetCacheWindow(3, 3);

    // Baseline: cost to bring one heavy chunk resident (comparable order to freeing it).
    const auto load_t0 = std::chrono::steady_clock::now();
    ASSERT_NE(ctx.reader->GetFrameSync(1000), nullptr);
    const double load_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_t0).count();

    // Fill a full window, then wait for QUIESCENCE (no loads in flight). The destructor
    // legitimately waits on in-flight load workers because they touch `this`; that is not
    // what we are measuring. We are isolating the cost of freeing the RESIDENT cache.
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        (void)ctx.reader->GetFrame(1000);
        if (ctx.chunk_state->GetSnapshot().loading_chunks.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(ctx.chunk_state->GetSnapshot().loading_chunks.empty()) << "loads never settled";
    ASSERT_FALSE(ctx.chunk_state->GetSnapshot().loaded_chunks.empty()) << "nothing resident to free";

    const auto destroy_t0 = std::chrono::steady_clock::now();
    ctx.Reset(); // destroys the ReplayReader with a heavy resident window
    const double destroy_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - destroy_t0).count();

    EXPECT_LT(destroy_ms, std::max(50.0, load_ms))
        << "reader destruction took " << destroy_ms << "ms (load baseline " << load_ms
        << "ms) -- it appears to be freeing the resident cache inline";
}

INSTANTIATE_TEST_SUITE_P(BothBackends, ReaderApiTest,
                         ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
                         [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
                             return std::string(FormatName(info.param));
                         });
