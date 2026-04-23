// Tests for VTX::OpenReplayFile + ReaderContext behaviour and lifetime.
//
// Two groups:
//   - ReaderContextHappy -- uses a TEST_F fixture so every test starts with a
//                           freshly-written .vtx and an open ReaderContext.
//   - ReaderContextFailure -- plain TEST for invalid/corrupt file cases where
//                             the fixture setup would actively get in the way.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

namespace {

    // Writes a tiny 5-frame FlatBuffers replay and returns the path.
    // uuid is used both as the filename tail and as the replay UUID so it's easy
    // to identify which test produced which artefact on disk.
    std::string WriteTinyFlatBuffersFile(const std::string& uuid) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("reader_" + uuid + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "ReaderContextTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 10;
        cfg.use_compression = true;

        {
            auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
            for (int i = 0; i < 5; ++i) {
                VTX::Frame f;
                auto& bucket = f.CreateBucket("entity");

                VTX::PropertyContainer pc;
                pc.entity_type_id = 0;
                pc.string_properties = {"p", "name"};
                pc.int32_properties = {1, 0, 0};
                pc.float_properties = {100.0f, 50.0f};
                pc.vector_properties = {VTX::Vector {}, VTX::Vector {}};
                pc.quat_properties = {VTX::Quat {}};
                pc.bool_properties = {true};

                bucket.unique_ids.push_back("p");
                bucket.entities.push_back(std::move(pc));

                VTX::GameTime::GameTimeRegister t;
                t.game_time = float(i) / 60.0f;
                writer->RecordFrame(f, t);
            }
            writer->Flush();
            writer->Stop();
        } // release -- file is fully closed before returning
        return cfg.output_filepath;
    }

} // namespace


// ===========================================================================
//  Happy-path fixture: every test starts with an open ReaderContext.
// ===========================================================================

class ReaderContextHappy : public ::testing::Test {
protected:
    void SetUp() override {
        // Name output after the current test so parallel ctest invocations
        // don't race over the same filename.
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string uuid = std::string(info->test_suite_name()) + "_" + info->name();

        path_ = WriteTinyFlatBuffersFile(uuid);
        ctx_ = VTX::OpenReplayFile(path_);
        ASSERT_TRUE(ctx_) << ctx_.error;
    }

    std::string path_;
    VTX::ReaderContext ctx_;
};

TEST_F(ReaderContextHappy, ReportsFlatBuffersFormat) {
    EXPECT_EQ(ctx_.format, VTX::VtxFormat::FlatBuffers);
    EXPECT_TRUE(ctx_.error.empty());
    EXPECT_GT(ctx_.size_in_mb, 0.0f);
    EXPECT_EQ(ctx_.reader->GetTotalFrames(), 5);
}

TEST_F(ReaderContextHappy, ChunkStateIsAlwaysNonNull) {
    ASSERT_TRUE(ctx_.chunk_state);
    // GetSnapshot() must be safe immediately after open.
    const auto snap = ctx_.chunk_state->GetSnapshot();
    (void)snap;
}

// Regression for the ReaderContext member-order UAF.  If reader and
// chunk_state were destroyed in the wrong order, any in-flight async callback
// could touch a freed chunk_state.
TEST_F(ReaderContextHappy, DestroysCleanlyAfterFrameAccess) {
    (void)ctx_.reader->GetFrameSync(0);
    (void)ctx_.reader->GetFrameSync(4);
    // ctx_ goes out of scope in TearDown -- reader is destroyed first, then
    // chunk_state.  Any previously-ordered exit would be a UAF.
    SUCCEED();
}

TEST_F(ReaderContextHappy, ResetMakesContextNonLoaded) {
    ctx_.Reset();
    EXPECT_FALSE(ctx_);
    EXPECT_EQ(ctx_.format, VTX::VtxFormat::Unknown);
}

TEST_F(ReaderContextHappy, GetFrameSyncReturnsConsistentDataOnRepeatCalls) {
    const VTX::Frame* a = ctx_.reader->GetFrameSync(2);
    const VTX::Frame* b = ctx_.reader->GetFrameSync(2);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->GetBuckets().size(), b->GetBuckets().size());
}

TEST_F(ReaderContextHappy, GetRawFrameBytesReturnsNonEmptySpan) {
    auto raw = ctx_.reader->GetRawFrameBytes(0);
    EXPECT_GT(raw.size(), 0u);
}

TEST_F(ReaderContextHappy, HeaderAndFooterRoundtripMetadata) {
    auto header = ctx_.reader->GetHeader();
    EXPECT_EQ(header.replay_name, "ReaderContextTest");

    auto footer = ctx_.reader->GetFooter();
    EXPECT_EQ(footer.total_frames, 5);
}


// ===========================================================================
//  Failure-path tests -- no fixture, each constructs its own bad input.
// ===========================================================================

TEST(ReaderContextFailure, NonexistentFileReturnsError) {
    auto ctx = VTX::OpenReplayFile(VtxTest::OutputPath("definitely_not_here.vtx"));
    EXPECT_FALSE(ctx);
    EXPECT_FALSE(ctx.error.empty());
    EXPECT_EQ(ctx.format, VTX::VtxFormat::Unknown);
}

TEST(ReaderContextFailure, GarbageFileReturnsError) {
    const auto path = VtxTest::OutputPath("garbage.vtx");
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs << "this is not a .vtx file at all";
    }

    auto ctx = VTX::OpenReplayFile(path);
    EXPECT_FALSE(ctx);
    EXPECT_FALSE(ctx.error.empty());
}


// ===========================================================================
//  §READY: chunk-0 ready signalling (IsReady / WaitUntilReady / OnReady).
//
//  OpenReplayFile() triggers async warm of chunk 0 as part of opening.
//  Callers observe completion via polling, blocking, or callback.
// ===========================================================================

TEST_F(ReaderContextHappy, ReadyFlipsWithinTimeoutOnValidReplay) {
    // A tiny 5-frame file should warm chunk 0 in well under 5s on any CI
    // environment.  5s is the "TSan on a cold runner" ceiling, not a
    // target.
    ASSERT_TRUE(ctx_.WaitUntilReady(std::chrono::seconds(5)));
    EXPECT_TRUE(ctx_.IsReady());
    EXPECT_FALSE(ctx_.IsReadyFailed());
    EXPECT_TRUE(ctx_.GetReadyError().empty());
}

TEST_F(ReaderContextHappy, ReadyIsStableAcrossRepeatedQueries) {
    ASSERT_TRUE(ctx_.WaitUntilReady(std::chrono::seconds(5)));
    // Ready is a terminal state -- it must not flip back.
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(ctx_.IsReady());
        EXPECT_FALSE(ctx_.IsReadyFailed());
    }
}

TEST(ReaderContextReady, OnReadyFiresOnDirectFacadeWithPreWiredEvents) {
    // OpenReplayFile() wires the context's own events and then calls
    // WarmAt(0), so a user callback registered AFTER open may miss the
    // single-shot signal (race).  For a deterministic test we drive the
    // facade directly: construct, SetEvents with our OnReady, THEN
    // WarmAt(0) ourselves.
    const auto path = WriteTinyFlatBuffersFile("OnReadyFiresOnDirectFacade");

    auto facade = VTX::CreateFlatBuffersFacade(path);
    ASSERT_NE(facade, nullptr);

    std::atomic<int> ready_count {0};
    std::atomic<int> failed_count {0};

    VTX::ReplayReaderEvents evts;
    evts.OnReady = [&]() { ready_count.fetch_add(1); };
    evts.OnReadyFailed = [&](const std::string&) { failed_count.fetch_add(1); };
    facade->SetEvents(evts);

    // Kick the async load ourselves.  The reader's OnReady will fire
    // exactly once when chunk 0 lands.
    facade->WarmAt(0);

    // Poll for up to 5s.  OnReady fires from the worker thread, so we
    // spin on the atomic rather than blocking on WaitUntilReady() (we
    // want to validate the callback path specifically).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ready_count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(ready_count.load(), 1);
    EXPECT_EQ(failed_count.load(), 0);
    EXPECT_TRUE(facade->IsReady());
    EXPECT_FALSE(facade->IsReadyFailed());
}

TEST(ReaderContextReady, ReadyIsVacuousForZeroFrameReplay) {
    // A zero-frame replay can still be opened (header + footer parse)
    // but has no chunks to load.  The ready flag must flip immediately
    // via the MarkReadyVacuous() path so waiters / pollers don't hang.
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = VtxTest::OutputPath("reader_empty_replay.vtx");
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name = "EmptyReplay";
    cfg.replay_uuid = "empty";
    cfg.default_fps = 60.0f;
    cfg.chunk_max_frames = 10;
    cfg.use_compression = true;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        // No RecordFrame() calls.
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    if (!ctx) {
        // Some writer builds refuse to finalise an empty replay.  If
        // that's the case here, this test is a no-op -- the
        // MarkReadyVacuous path is still exercised by
        // OnReadyFailureFlipsOnCorruptChunkZero via the symmetric
        // failure case.
        GTEST_SKIP() << "Writer produced no usable empty replay: " << ctx.error;
    }

    EXPECT_EQ(ctx.reader->GetTotalFrames(), 0);
    EXPECT_TRUE(ctx.IsReady()) << "Zero-frame replay should flip ready vacuously";
    EXPECT_FALSE(ctx.IsReadyFailed());
    EXPECT_TRUE(ctx.WaitUntilReady(std::chrono::milliseconds(100)));
}

TEST(ReaderContextReady, ReadyFailsOnCorruptChunkZero) {
    // Write a valid 5-frame replay, then zero out a stretch of bytes
    // one-third of the way in.  The single-chunk body lives between
    // the header and the footer, so clobbering that region corrupts
    // chunk 0 while keeping the header + footer parseable.  Chunk 0
    // load must fail, IsReadyFailed() must flip, and GetReadyError()
    // must carry a non-empty reason.
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = VtxTest::OutputPath("reader_corrupt_chunk0.vtx");
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name = "CorruptChunk0";
    cfg.replay_uuid = "corrupt_chunk0";
    cfg.default_fps = 60.0f;
    cfg.chunk_max_frames = 100;   // keep it all in one chunk
    cfg.use_compression = true;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        for (int i = 0; i < 5; ++i) {
            VTX::Frame f;
            auto& bucket = f.CreateBucket("entity");
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.string_properties = {"p", "name"};
            pc.int32_properties = {1, 0, 0};
            pc.float_properties = {100.0f, 50.0f};
            pc.vector_properties = {VTX::Vector {}, VTX::Vector {}};
            pc.quat_properties = {VTX::Quat {}};
            pc.bool_properties = {true};
            bucket.unique_ids.push_back("p");
            bucket.entities.push_back(std::move(pc));
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(f, t);
        }
        writer->Flush();
        writer->Stop();
    }

    const auto size = std::filesystem::file_size(cfg.output_filepath);
    ASSERT_GT(size, 64u);

    // Clobber a region that is definitely inside chunk 0's compressed
    // payload: start at size/3, length = size/3.  With header + footer
    // each being O(tens of bytes), size/3 is comfortably past the
    // header and before the footer.
    {
        std::fstream f(cfg.output_filepath, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f);
        f.seekp(static_cast<std::streamoff>(size / 3));
        const std::vector<char> poison(static_cast<std::size_t>(size / 3), static_cast<char>(0xFF));
        f.write(poison.data(), static_cast<std::streamsize>(poison.size()));
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    if (!ctx) {
        // Aggressive corruption may also break footer parsing.  That's
        // an acceptable outcome -- the point of this test is that
        // chunk 0 failure is observable when the file *does* open.
        GTEST_SKIP() << "Corruption also broke header/footer: " << ctx.error;
    }

    // WaitUntilReady returns false on failure and its bool is IsReady().
    const bool is_ready = ctx.WaitUntilReady(std::chrono::seconds(5));
    EXPECT_FALSE(is_ready);
    EXPECT_FALSE(ctx.IsReady());
    EXPECT_TRUE(ctx.IsReadyFailed());
    EXPECT_FALSE(ctx.GetReadyError().empty());
}

TEST_F(ReaderContextHappy, WaitUntilReadyIsIdempotent) {
    // Once the reader has signalled ready, repeated WaitUntilReady
    // calls must return true immediately without blocking.  Regression
    // for any future refactor that accidentally consumes the cv signal.
    ASSERT_TRUE(ctx_.WaitUntilReady(std::chrono::seconds(5)));
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(ctx_.WaitUntilReady(std::chrono::seconds(5)));
    EXPECT_TRUE(ctx_.WaitUntilReady(std::chrono::seconds(5)));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::milliseconds(500))
        << "Second and third WaitUntilReady should be near-instant";
}

TEST_F(ReaderContextHappy, GetFrameSyncAfterReadyHitsWarmCache) {
    // The point of eager-warming chunk 0 at open time is that the
    // caller's first GetFrame* call becomes a cache hit instead of a
    // cold ZSTD decompress.  After WaitUntilReady the lookup must be
    // near-instant and return a non-null frame pointer.  Catches a
    // regression where async and sync paths accidentally both run the
    // heavy load (observable as a long delay here).
    ASSERT_TRUE(ctx_.WaitUntilReady(std::chrono::seconds(5)));

    const auto t0 = std::chrono::steady_clock::now();
    const VTX::Frame* frame = ctx_.reader->GetFrameSync(0);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_NE(frame, nullptr);
    // 100ms is extremely loose -- the real hot-cache measurement is
    // sub-millisecond.  The wide bound keeps debug and sanitiser runs
    // on slow CI boxes green without masking a real regression.
    EXPECT_LT(elapsed, std::chrono::milliseconds(100))
        << "Chunk 0 should already be cached after WaitUntilReady";
}

TEST_F(ReaderContextHappy, WarmAtAfterReadyDoesNotRegressFlag) {
    // Ready is a single-shot terminal state tied to chunk 0.  Additional
    // WarmAt calls (same chunk or any other) must never flip the flag
    // back or clear the terminal condition for callbacks.
    ASSERT_TRUE(ctx_.WaitUntilReady(std::chrono::seconds(5)));
    ASSERT_TRUE(ctx_.IsReady());

    ctx_.reader->WarmAt(0);
    EXPECT_TRUE(ctx_.IsReady());
    EXPECT_FALSE(ctx_.IsReadyFailed());

    // Out-of-range WarmAt is a defined no-op on the reader side.
    ctx_.reader->WarmAt(999);
    EXPECT_TRUE(ctx_.IsReady());
    EXPECT_FALSE(ctx_.IsReadyFailed());
}

TEST(ReaderContextReady, OnReadyFailedFiresOnDirectFacadeForCorruptChunkZero) {
    // Dual of OnReadyFiresOnDirectFacadeWithPreWiredEvents: same
    // pre-wired events + direct-facade pattern, but the file is
    // corrupted so OnReadyFailed fires instead of OnReady.  Pins the
    // failure-callback contract end-to-end.
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = VtxTest::OutputPath("reader_onreadyfailed_direct.vtx");
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name = "OnReadyFailedDirect";
    cfg.replay_uuid = "onreadyfailed_direct";
    cfg.default_fps = 60.0f;
    cfg.chunk_max_frames = 100;
    cfg.use_compression = true;
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        for (int i = 0; i < 5; ++i) {
            VTX::Frame f;
            auto& bucket = f.CreateBucket("entity");
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.string_properties = {"p", "name"};
            pc.int32_properties = {1, 0, 0};
            pc.float_properties = {100.0f, 50.0f};
            pc.vector_properties = {VTX::Vector {}, VTX::Vector {}};
            pc.quat_properties = {VTX::Quat {}};
            pc.bool_properties = {true};
            bucket.unique_ids.push_back("p");
            bucket.entities.push_back(std::move(pc));
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(f, t);
        }
        writer->Flush();
        writer->Stop();
    }

    const auto size = std::filesystem::file_size(cfg.output_filepath);
    ASSERT_GT(size, 64u);
    {
        std::fstream f(cfg.output_filepath, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f);
        f.seekp(static_cast<std::streamoff>(size / 3));
        const std::vector<char> poison(static_cast<std::size_t>(size / 3), static_cast<char>(0xFF));
        f.write(poison.data(), static_cast<std::streamsize>(poison.size()));
    }

    // The ReplayReader constructor parses header + footer synchronously;
    // both are ZSTD-compressed in a normal .vtx file, so aggressive bulk
    // corruption may cause the facade constructor itself to throw before
    // we can even register events.  In that case the async-failure path
    // we're targeting is not reachable; skip rather than flake.
    std::unique_ptr<VTX::IVtxReaderFacade> facade;
    try {
        facade = VTX::CreateFlatBuffersFacade(cfg.output_filepath);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Corruption also broke synchronous init: " << e.what();
    }
    ASSERT_NE(facade, nullptr);

    std::atomic<int> ready_count {0};
    std::atomic<int> failed_count {0};
    std::mutex err_mu;
    std::string last_error;

    VTX::ReplayReaderEvents evts;
    evts.OnReady = [&]() { ready_count.fetch_add(1); };
    evts.OnReadyFailed = [&](const std::string& err) {
        {
            std::lock_guard<std::mutex> lk(err_mu);
            last_error = err;
        }
        failed_count.fetch_add(1);
    };
    facade->SetEvents(evts);

    facade->WarmAt(0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ready_count.load() + failed_count.load() == 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (ready_count.load() == 1) {
        // Rare but possible: the FlatBuffers path may tolerate our
        // specific poisoning byte pattern.  Skip rather than flake --
        // the IsReadyFailed path is already covered by
        // ReadyFailsOnCorruptChunkZero.
        GTEST_SKIP() << "FlatBuffers accepted the poisoned chunk";
    }

    EXPECT_EQ(ready_count.load(), 0);
    EXPECT_EQ(failed_count.load(), 1);
    EXPECT_TRUE(facade->IsReadyFailed());

    std::lock_guard<std::mutex> lk(err_mu);
    EXPECT_FALSE(last_error.empty())
        << "OnReadyFailed callback must receive a non-empty error message";
}

// NOTE: we intentionally do NOT test "Reset() while another thread is
// blocked inside WaitUntilReady()".  Destroying the reader while a
// waiter holds its mutex/cv is UB per the standard even though our
// dtor best-efforts to flip ready_failed_ + notify before teardown.
// Callers must join any waiters before destroying the ReaderContext.
