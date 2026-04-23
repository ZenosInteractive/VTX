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
                                                     : VTX::CreateProtobuffWriterFacade(cfg);
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

INSTANTIATE_TEST_SUITE_P(BothBackends, ReaderApiTest,
                         ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
                         [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
                             return std::string(FormatName(info.param));
                         });
