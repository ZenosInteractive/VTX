// AsyncSinkAdapter tests: the async decorator must move chunk/journal I/O to a worker
// thread WITHOUT changing what lands on disk or the guarantees callers rely on.
//
// Coverage:
//   * Order equivalence  -- against an instrumented fake inner sink, the async call
//                           sequence to the inner sink equals the synchronous one.
//   * Byte equivalence   -- through the REAL sink, an async recording is bit-for-bit
//                           identical to a synchronous one for the same inputs.
//   * Backpressure       -- a bounded queue + a slow inner sink blocks the caller but
//                           never drops or reorders a frame.
//   * Failure protocol   -- a latched inner I/O failure surfaces as SinkFailed on the
//                           next TryRecordFrame and through Drain()/GetLastError, aborts
//                           the recording, and unblocks any producer waiting on the queue.
//   * Drain()            -- the durability barrier drains the queue in order.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vtx_schema_generated.h" // complete fbsvtx/cppvtx types before the policy headers
#include "vtx_schema.pb.h"

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/reader/core/vtx_replay_validation.h"
#include "vtx/writer/core/vtx_replay_recovery.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/core/writer.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"
#include "vtx/writer/policies/sinks/async_sink_adapter.h"
#include "vtx/writer/policies/sinks/file_sink.h"

#include "util/test_fixtures.h"

namespace {

    VTX::Frame BuildFrame(int i) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {"player_0", "Alpha"};
        pc.int32_properties = {1, i, 0}; // Team, Score(=i), Deaths
        pc.float_properties = {100.0f, 50.0f};
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

    // --- Instrumented fake inner sink -------------------------------------------
    //
    // Records the exact ordered sequence of data operations (journal frame, save chunk,
    // close, abort) reaching the inner sink, WITHOUT touching disk. Shared with the test
    // through a shared_ptr so it survives being moved into the writer/adapter. It reuses
    // the real FlatBuffers serializer policy so it plugs straight into ReplayWriter.

    struct FakeSinkState {
        std::mutex mu;
        std::vector<std::string> ops; // normalized op log ("J<idx>", "C<start>-<total>", ...)
        int save_chunk_calls = 0;
        int fail_after_chunks = -1;               // >=0: latch a failure on that many SaveChunk calls
        std::atomic<bool> good {true};            // mirrors DurableFile::Good()
        bool journal_active = true;               // mirrors an open recovery journal
        std::chrono::milliseconds work_delay {0}; // artificial per-op slowness for backpressure

        void Log(std::string s) {
            std::lock_guard<std::mutex> lk(mu);
            ops.push_back(std::move(s));
        }
        std::vector<std::string> Ops() {
            std::lock_guard<std::mutex> lk(mu);
            return ops;
        }
    };

    struct FakeSink {
        using SerializerPolicy = VTX::FlatBuffersVtxPolicy;
        using FrameType = VTX::Frame;
        using SchemaType = SerializerPolicy::SchemaType;
        using HeaderType = SerializerPolicy::HeaderType;

        struct Config {
            std::shared_ptr<FakeSinkState> state;
        };

        explicit FakeSink(Config c)
            : state_(std::move(c.state)) {}

        void JournalTiming(float, bool) {}
        void OnSessionStart(const SchemaType&) { state_->Log("OnSessionStart"); }

        void JournalFrame(const FrameType&, int32_t idx, int64_t, int64_t) {
            Delay();
            state_->Log("J" + std::to_string(idx));
        }
        // The async worker calls this instead; it must log identically (sync coalescing aside)
        // so the two op sequences line up.
        void JournalFrameBatched(const FrameType&, int32_t idx, int64_t, int64_t) {
            Delay();
            state_->Log("J" + std::to_string(idx));
        }
        void SyncJournal() {} // coalescing of J-syncs is allowed; not logged
        bool IsJournalActive() const { return state_->journal_active; }

        void SaveChunk(std::vector<std::unique_ptr<FrameType>>&, const std::vector<int64_t>&, int32_t start,
                       int32_t total) {
            Delay();
            state_->Log("C" + std::to_string(start) + "-" + std::to_string(total));
            const int n = ++state_->save_chunk_calls;
            if (state_->fail_after_chunks >= 0 && n >= state_->fail_after_chunks)
                state_->good.store(false, std::memory_order_release); // latch, as a short write would
        }

        void Close(const VTX::SessionFooter& f) { state_->Log("Close" + std::to_string(f.total_frames)); }
        bool Good() const { return state_->good.load(std::memory_order_acquire); }
        void AbortClose() { state_->Log("AbortClose"); }

    private:
        void Delay() {
            if (state_->work_delay.count() > 0)
                std::this_thread::sleep_for(state_->work_delay);
        }
        std::shared_ptr<FakeSinkState> state_;
    };

    using SyncFake = VTX::ReplayWriter<FakeSink>;
    using AsyncFake = VTX::ReplayWriter<VTX::AsyncSinkAdapter<FakeSink>>;

    std::unique_ptr<SyncFake> MakeSyncFake(std::shared_ptr<FakeSinkState> state, int chunk_max) {
        SyncFake::Config cfg;
        cfg.sink_config.state = std::move(state);
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.default_fps = 60.0f;
        cfg.chunker_config.max_frames = chunk_max;
        return std::make_unique<SyncFake>(cfg);
    }

    std::unique_ptr<AsyncFake> MakeAsyncFake(std::shared_ptr<FakeSinkState> state, int chunk_max, size_t queue_cap) {
        AsyncFake::Config cfg;
        cfg.sink_config.inner.state = std::move(state);
        cfg.sink_config.async_max_queue_frames = queue_cap;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.default_fps = 60.0f;
        cfg.chunker_config.max_frames = chunk_max;
        return std::make_unique<AsyncFake>(cfg);
    }

    template <typename Writer>
    void RecordFrames(Writer& w, int count) {
        for (int i = 0; i < count; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            t.created_utc_time = 1'000'000'000'000 + int64_t(i) * 166'667;
            w.TryRecordFrame(frame, t);
        }
    }

} // namespace

// The inner sink sees the exact same ordered sequence of data operations whether the
// writer is synchronous or async: one FIFO, one worker preserves order by construction.
TEST(AsyncSink, OrderEquivalenceMatchesSync) {
    auto sync_state = std::make_shared<FakeSinkState>();
    auto async_state = std::make_shared<FakeSinkState>();

    {
        auto w = MakeSyncFake(sync_state, /*chunk_max=*/3);
        RecordFrames(*w, 10);
        w->Stop();
    }
    {
        auto w = MakeAsyncFake(async_state, /*chunk_max=*/3, /*queue_cap=*/4);
        RecordFrames(*w, 10);
        w->Stop(); // drains + closes + joins the worker
    }

    EXPECT_EQ(async_state->Ops(), sync_state->Ops());
    ASSERT_FALSE(sync_state->Ops().empty());
    EXPECT_EQ(sync_state->Ops().front(), "OnSessionStart");
    EXPECT_EQ(sync_state->Ops().back(), "Close10");
}

// A bounded queue with a slow inner sink blocks the caller (never returns early, never
// drops) -- every recorded frame reaches the inner sink, in order.
TEST(AsyncSink, BackpressureBlocksButLosesNothing) {
    auto state = std::make_shared<FakeSinkState>();
    state->work_delay = std::chrono::milliseconds(2); // make the worker the bottleneck

    {
        auto w = MakeAsyncFake(state, /*chunk_max=*/2, /*queue_cap=*/2);
        RecordFrames(*w, 20);
        w->Stop();
    }

    const auto ops = state->Ops();
    int journaled = 0;
    for (const auto& op : ops)
        if (!op.empty() && op[0] == 'J')
            ++journaled;
    EXPECT_EQ(journaled, 20); // no frame lost under backpressure
    EXPECT_EQ(ops.front(), "OnSessionStart");
    EXPECT_EQ(ops.back(), "Close20");
}

// A latched inner I/O failure aborts the recording: the next TryRecordFrame is rejected
// with SinkFailed, the queue is discarded, and the inner handles are released (AbortClose).
TEST(AsyncSink, FailureSurfacesAsSinkFailed) {
    auto state = std::make_shared<FakeSinkState>();
    state->fail_after_chunks = 1; // fail on the first chunk flush

    auto w = MakeAsyncFake(state, /*chunk_max=*/2, /*queue_cap=*/8);

    // Drive frames until the worker has flushed a chunk and latched the failure, which the
    // writer then reports synchronously. Bounded loop so a regression cannot hang the suite.
    bool saw_sink_failed = false;
    for (int i = 0; i < 200 && !saw_sink_failed; ++i) {
        auto frame = BuildFrame(i);
        VTX::GameTime::GameTimeRegister t;
        t.game_time = float(i) / 60.0f;
        t.created_utc_time = 1'000'000'000'000 + int64_t(i) * 166'667;
        const auto res = w->TryRecordFrame(frame, t);
        if (!res.written && res.error.code == VTX::VtxErrorCode::SinkFailed)
            saw_sink_failed = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(saw_sink_failed);
    EXPECT_TRUE(w->HasSinkFailed());
    EXPECT_EQ(w->GetLastError().code, VTX::VtxErrorCode::SinkFailed);

    // Drain() on a failed sink returns the latched error promptly (no hang).
    EXPECT_EQ(w->Drain().code, VTX::VtxErrorCode::SinkFailed);

    w->Stop(); // must not hang; worker already exited

    const auto ops = state->Ops();
    bool aborted = false;
    for (const auto& op : ops)
        if (op == "AbortClose")
            aborted = true;
    EXPECT_TRUE(aborted); // handles released -> file becomes repair-ready immediately
}

// Drain() is the durability barrier: it blocks until every frame enqueued before it has
// been handed to the inner sink, then the queue is empty.
TEST(AsyncSink, DrainFlushesQueueInOrder) {
    auto state = std::make_shared<FakeSinkState>();
    state->work_delay = std::chrono::milliseconds(1);

    auto w = MakeAsyncFake(state, /*chunk_max=*/4, /*queue_cap=*/64);
    RecordFrames(*w, 12);

    const auto err = w->Drain();
    EXPECT_EQ(err.code, VTX::VtxErrorCode::None);
    EXPECT_EQ(w->GetQueueDepth(), 0u); // everything caught up

    int journaled = 0;
    for (const auto& op : state->Ops())
        if (!op.empty() && op[0] == 'J')
            ++journaled;
    EXPECT_EQ(journaled, 12); // all 12 frames journaled before Drain returned

    w->Stop();
}

// Recording (or draining) after Stop() is caller misuse, but it must never HANG. The worker
// is gone, so an enqueue that would wait for room has nobody to free it: the adapter reports
// the worker's exit and returns immediately instead of blocking forever. Reaching the end of
// this test IS the assertion -- a regression deadlocks here.
TEST(AsyncSink, EnqueueAfterStopDoesNotHang) {
    auto state = std::make_shared<FakeSinkState>();
    auto w = MakeAsyncFake(state, /*chunk_max=*/2, /*queue_cap=*/2);
    RecordFrames(*w, 4);
    w->Stop();

    RecordFrames(*w, 50);                                // far more items than the queue cap
    EXPECT_EQ(w->Drain().code, VTX::VtxErrorCode::None); // barrier with no worker: returns, not hangs
    EXPECT_NO_FATAL_FAILURE(w->Stop());                  // a second Stop is also a no-op

    // Nothing after the close reached the sink: the recording ended at Stop().
    const auto ops = state->Ops();
    ASSERT_FALSE(ops.empty());
    EXPECT_EQ(ops.back(), "Close4");
}

// --- Byte equivalence through the REAL sink ------------------------------------
//
// The async .vtx must be bit-for-bit identical to a synchronous one for the same inputs:
// only WHEN I/O happens changes, never WHAT is written. Explicit per-frame times remove
// any wall-clock nondeterminism so the comparison is exact.

namespace {

    void WriteRecording(const std::string& path, bool async, bool protobuf, int frames, int chunk_max) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = path;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "AsyncByteEquiv";
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = chunk_max;
        cfg.use_compression = true;
        cfg.async_io = async;

        auto w = protobuf ? VTX::CreateProtobufWriterFacade(cfg) : VTX::CreateFlatBuffersWriterFacade(cfg);
        EXPECT_NE(w, nullptr);
        for (int i = 0; i < frames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            t.created_utc_time = 1'000'000'000'000 + int64_t(i) * 166'667;
            w->RecordFrame(frame, t);
        }
        w->Stop();
        w.reset(); // release the handle before reading the bytes back
    }

    int64_t NowSecond() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    void RunByteEquivalence(bool protobuf, const std::string& tag) {
        const std::string sync_path = VtxTest::OutputPath("async_equiv_sync_" + tag + ".vtx");
        const std::string async_path = VtxTest::OutputPath("async_equiv_async_" + tag + ".vtx");

        // The file header embeds a wall-clock second (FileHeader.timestamp = now()), so two
        // independent recordings differ there whenever they straddle a second boundary -- which
        // then cascades through zstd and the seek-table offsets. Pair the writes inside a single
        // wall-clock second (retrying on the rare boundary crossing) so the ONLY remaining source
        // of difference is the async path itself, and full-file byte identity is a valid check.
        std::vector<std::byte> sync_bytes;
        std::vector<std::byte> async_bytes;
        bool same_second = false;
        for (int attempt = 0; attempt < 8 && !same_second; ++attempt) {
            const int64_t before = NowSecond();
            WriteRecording(sync_path, /*async=*/false, protobuf, /*frames=*/17, /*chunk_max=*/4);
            WriteRecording(async_path, /*async=*/true, protobuf, /*frames=*/17, /*chunk_max=*/4);
            const int64_t after = NowSecond();
            if (before == after) {
                same_second = true;
                sync_bytes = VtxTest::ReadAllBytes(sync_path);
                async_bytes = VtxTest::ReadAllBytes(async_path);
            }
        }
        ASSERT_TRUE(same_second) << "could not pair the writes within one wall-clock second";

        ASSERT_FALSE(sync_bytes.empty());
        EXPECT_EQ(async_bytes, sync_bytes) << "async .vtx diverged from sync for " << tag;

        // And the async file is a valid, readable recording with the expected frame count.
        auto ctx = VTX::OpenReplayFile(async_path);
        ASSERT_TRUE(ctx) << ctx.error;
        ctx->WaitUntilReady();
        EXPECT_EQ(ctx->GetTotalFrames(), 17);
    }

} // namespace

TEST(AsyncSink, ByteEquivalenceFlatBuffers) {
    RunByteEquivalence(/*protobuf=*/false, "fb");
}

TEST(AsyncSink, ByteEquivalenceProtobuf) {
    RunByteEquivalence(/*protobuf=*/true, "pb");
}

// --- Integrity: no frame and no chunk may be lost ------------------------------

namespace {

    struct MatrixCfg {
        bool durable;
        bool compression;
        bool journal;
        int chunk_max;
        size_t queue_cap;
        const char* tag;
    };

    void WriteMatrix(const std::string& path, bool async, const MatrixCfg& c, int frames) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = path;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = c.chunk_max;
        cfg.use_compression = c.compression;
        cfg.durable_writes = c.durable;
        cfg.enable_recovery_journal = c.journal;
        cfg.async_io = async;
        cfg.async_max_queue_frames = c.queue_cap;

        auto w = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_NE(w, nullptr);
        for (int i = 0; i < frames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            t.created_utc_time = 1'000'000'000'000 + int64_t(i) * 166'667;
            w->RecordFrame(frame, t);
        }
        w->Stop();
    }

} // namespace

// Across the sink's whole configuration matrix -- durability, compression, journal on/off,
// chunk sizes, and queue caps down to 1 (permanent backpressure) -- an async recording must
// contain EXACTLY what the synchronous one does: same frame count, same CHUNK count, and the
// right frame with the right content at every index. A dropped, duplicated, or reordered
// queue item would break one of these.
TEST(AsyncSink, NoFrameOrChunkLossAcrossConfigMatrix) {
    const MatrixCfg configs[] = {
        {true, true, true, 4, 0, "d1c1j1_q0"},          // defaults, queue cap auto (2*chunk)
        {true, false, true, 4, 2, "d1c0j1_q2"},         // uncompressed, tight queue
        {false, true, true, 1, 1, "d0c1j1_chunk1_q1"},  // a chunk per frame, cap 1: max contention
        {true, true, false, 8, 3, "d1c1j0_q3"},         // recovery journal OFF (no journal items)
        {false, false, true, 16, 64, "d0c0j1_q16_q64"}, // flush-only, roomy queue
    };
    constexpr int kFrames = 137; // deliberately not a multiple of any chunk size

    for (const auto& c : configs) {
        SCOPED_TRACE(c.tag);
        const std::string sync_path = VtxTest::OutputPath(std::string("mtx_sync_") + c.tag + ".vtx");
        const std::string async_path = VtxTest::OutputPath(std::string("mtx_async_") + c.tag + ".vtx");

        WriteMatrix(sync_path, /*async=*/false, c, kFrames);
        WriteMatrix(async_path, /*async=*/true, c, kFrames);
        if (::testing::Test::HasFatalFailure())
            return;

        auto sync_ctx = VTX::OpenReplayFile(sync_path);
        ASSERT_TRUE(sync_ctx) << sync_ctx.error;
        sync_ctx->WaitUntilReady();
        auto async_ctx = VTX::OpenReplayFile(async_path);
        ASSERT_TRUE(async_ctx) << async_ctx.error;
        async_ctx->WaitUntilReady();

        ASSERT_EQ(async_ctx->GetTotalFrames(), kFrames) << "async lost frames";
        ASSERT_EQ(sync_ctx->GetTotalFrames(), kFrames);
        EXPECT_EQ(async_ctx->GetSeekTable().size(), sync_ctx->GetSeekTable().size()) << "chunk count diverged";

        for (int i = 0; i < kFrames; ++i) {
            const VTX::Frame* a = async_ctx->GetFrameSync(i);
            const VTX::Frame* s = sync_ctx->GetFrameSync(i);
            ASSERT_NE(a, nullptr) << "async frame " << i << " missing";
            ASSERT_NE(s, nullptr) << "sync frame " << i << " missing";
            // The right frame landed at the right index (a reorder would shift the score)...
            EXPECT_EQ(a->GetBuckets()[0].entities[0].int32_properties[1], i) << "frame " << i;
            // ...and its full content is identical to the synchronous recording's.
            EXPECT_EQ(a->GetBuckets()[0].entities[0].content_hash, s->GetBuckets()[0].entities[0].content_hash)
                << "content mismatch at frame " << i;
        }
    }
}

// A cleanly-stopped async recording must pass the SDK's OWN whole-replay validation --
// embedded schema plus every frame -- not merely read back with the right frame count.
TEST(AsyncSink, CleanAsyncRecordingPassesReplayValidation) {
    const std::string path = VtxTest::OutputPath("async_validated.vtx");
    const MatrixCfg c {true, true, true, 5, 4, "validate"};
    WriteMatrix(path, /*async=*/true, c, /*frames=*/64);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    const VTX::ValidationReport report = VTX::ValidateReplayFile(path);
    EXPECT_FALSE(report.HasErrors()) << report.ToString();
}

// Sustained volume through a ONE-item queue: thousands of hand-offs between the recording
// thread and the I/O worker, every one of them contended. A lost, duplicated or reordered
// item surfaces as the wrong frame at a known index.
TEST(AsyncSink, HighVolumeContendedStreamStaysIntact) {
    const std::string path = VtxTest::OutputPath("async_stress.vtx");
    const MatrixCfg c {false, true, true, 7, 1, "stress"}; // flush-only for speed, queue cap 1
    constexpr int kFrames = 2000;
    WriteMatrix(path, /*async=*/true, c, kFrames);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    ASSERT_EQ(ctx->GetTotalFrames(), kFrames);
    for (int i : {0, 1, 999, 1000, 1777, kFrames - 1}) {
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr) << "frame " << i;
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i) << "frame " << i;
    }
}

// Worker start/join lifecycle under churn: many short recordings back to back. A leaked,
// double-joined, or never-started worker (or a hang in Close) shows up here.
TEST(AsyncSink, RapidLifecycleCyclesAreClean) {
    constexpr int kRounds = 40;
    for (int round = 0; round < kRounds; ++round) {
        const std::string path = VtxTest::OutputPath("async_cycle_" + std::to_string(round) + ".vtx");
        const MatrixCfg c {true, true, true, 3, 4, "cycle"};
        WriteMatrix(path, /*async=*/true, c, /*frames=*/5);
        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_FALSE(VTX::ReplayNeedsRecovery(path)) << "round " << round; // clean Stop deleted the journal
        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error;
        ctx->WaitUntilReady();
        ASSERT_EQ(ctx->GetTotalFrames(), 5) << "round " << round;
    }
}

// The facade destructor finalizes an async recording that the caller never Stop()ped: the
// adapter must drain the queue, write the footer and join the worker -- with items still
// queued at destruction time (queue cap 2) and never hanging. Repeated to shake out any
// ordering hazard in that teardown path.
TEST(AsyncSink, FacadeDestructorFinalizesAsyncRecording) {
    constexpr int kRounds = 15;
    constexpr int kFrames = 9;
    for (int round = 0; round < kRounds; ++round) {
        const std::string path = VtxTest::OutputPath("async_drop_" + std::to_string(round) + ".vtx");
        std::filesystem::remove(path);
        std::filesystem::remove(VTX::RecoveryJournalPath(path));
        {
            VTX::WriterFacadeConfig cfg;
            cfg.output_filepath = path;
            cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
            cfg.default_fps = 60.0f;
            cfg.chunk_max_frames = 4;
            cfg.async_io = true;
            cfg.async_max_queue_frames = 2; // force backpressure, so items are still queued at drop
            auto w = VTX::CreateFlatBuffersWriterFacade(cfg);
            ASSERT_NE(w, nullptr);
            for (int i = 0; i < kFrames; ++i) {
                auto frame = BuildFrame(i);
                VTX::GameTime::GameTimeRegister t;
                t.game_time = float(i) / 60.0f;
                t.created_utc_time = 1'000'000'000'000 + int64_t(i) * 166'667;
                w->RecordFrame(frame, t);
            }
            // Deliberately NO Stop(): the facade destructor must finalize the recording.
        }
        EXPECT_FALSE(VTX::ReplayNeedsRecovery(path)) << "round " << round; // footer written, journal removed
        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error << " (round " << round << ")";
        ctx->WaitUntilReady();
        ASSERT_EQ(ctx->GetTotalFrames(), kFrames) << "round " << round;
    }
}

// Drain() interleaved with recording under permanent backpressure: the barrier must never
// deadlock against a blocked producer, and nothing may be lost around it.
TEST(AsyncSink, DrainInterleavedWithRecordingLosesNothing) {
    const std::string path = VtxTest::OutputPath("async_drain_interleaved.vtx");
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = path;
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.default_fps = 60.0f;
    cfg.chunk_max_frames = 3;
    cfg.async_io = true;
    cfg.async_max_queue_frames = 1; // every enqueue contends with the worker
    auto w = VTX::CreateFlatBuffersWriterFacade(cfg);
    ASSERT_NE(w, nullptr);

    constexpr int kFrames = 80;
    for (int i = 0; i < kFrames; ++i) {
        auto frame = BuildFrame(i);
        VTX::GameTime::GameTimeRegister t;
        t.game_time = float(i) / 60.0f;
        t.created_utc_time = 1'000'000'000'000 + int64_t(i) * 166'667;
        w->RecordFrame(frame, t);
        if (i % 7 == 0) {
            const VTX::VtxError err = w->Drain(); // barrier mid-stream, under contention
            ASSERT_EQ(err.code, VTX::VtxErrorCode::None) << "Drain failed at frame " << i;
            EXPECT_EQ(w->GetQueueDepth(), 0u);
        }
    }
    w->Stop();
    w.reset();

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    ASSERT_EQ(ctx->GetTotalFrames(), kFrames);
    for (int i : {0, 13, 41, kFrames - 1}) {
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr) << "frame " << i;
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i);
    }
}

// A clean async recording leaves no recovery sidecar behind (Stop() finalized the footer).
TEST(AsyncSink, CleanStopRemovesJournal) {
    const std::string path = VtxTest::OutputPath("async_clean.vtx");
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = path;
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.default_fps = 60.0f;
    cfg.chunk_max_frames = 3;
    cfg.async_io = true;

    auto w = VTX::CreateFlatBuffersWriterFacade(cfg);
    ASSERT_NE(w, nullptr);
    for (int i = 0; i < 7; ++i) {
        auto frame = BuildFrame(i);
        VTX::GameTime::GameTimeRegister t;
        t.game_time = float(i) / 60.0f;
        t.created_utc_time = 1'000'000'000'000 + int64_t(i) * 166'667;
        w->RecordFrame(frame, t);
    }
    w->Stop();
    w.reset();

    EXPECT_FALSE(VTX::ReplayNeedsRecovery(path));
}
