// Edge-case tests for VTX::IVtxWriterFacade.
//
// Covers boundary conditions that basic_write / roundtrip don't exercise:
// zero frames, single frame, one-chunk-per-frame, post-Stop API calls, and
// oversized frames relative to chunk_max_bytes.

#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <string>

#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

namespace {

    VTX::WriterFacadeConfig MakeBaseConfig(const std::string& out_name, const std::string& uuid,
                                           int32_t chunk_max_frames = 500, size_t chunk_max_bytes = 10 * 1024 * 1024) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath(out_name);
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "WriterEdgeTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = chunk_max_frames;
        cfg.chunk_max_bytes = chunk_max_bytes;
        cfg.use_compression = true;
        return cfg;
    }

    VTX::Frame MakeTrivialFrame(int frame_index = 0) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {"p", "player"};
        pc.int32_properties = {1, frame_index, 0};
        pc.float_properties = {100.0f, 50.0f};
        pc.vector_properties = {VTX::Vector {}, VTX::Vector {}};
        pc.quat_properties = {VTX::Quat {}};
        pc.bool_properties = {true};

        bucket.unique_ids.push_back("p");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

} // namespace

// ---------------------------------------------------------------------------
// Zero frames
// ---------------------------------------------------------------------------

TEST(WriterEdges, WriteZeroFramesThenStop) {
    const auto cfg = MakeBaseConfig("edge_zero_frames.vtx", "edge-zero");
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_TRUE(writer);
        writer->Stop(); // no RecordFrame calls
    }

    ASSERT_TRUE(std::filesystem::exists(cfg.output_filepath));

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 0);

    // Header + footer must still be readable on an empty replay.
    auto header = ctx.reader->GetHeader();
    EXPECT_EQ(header.replay_name, "WriterEdgeTest");
    auto footer = ctx.reader->GetFooter();
    EXPECT_EQ(footer.total_frames, 0);
}

// ---------------------------------------------------------------------------
// Exactly one frame
// ---------------------------------------------------------------------------

TEST(WriterEdges, WriteSingleFrame) {
    const auto cfg = MakeBaseConfig("edge_single_frame.vtx", "edge-single");
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        auto frame = MakeTrivialFrame(0);
        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        writer->RecordFrame(frame, t);
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx);
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 1);

    const auto& seek = ctx.reader->GetSeekTable();
    EXPECT_EQ(seek.size(), 1u);
    EXPECT_EQ(seek.front().start_frame, 0);
    EXPECT_EQ(seek.front().end_frame, 0);

    const VTX::Frame* f = ctx.reader->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets().size(), 1u);
}

// ---------------------------------------------------------------------------
// One chunk per frame (stresses chunk flushing path)
// ---------------------------------------------------------------------------

TEST(WriterEdges, ChunkMaxFramesIsOne) {
    const auto cfg = MakeBaseConfig("edge_chunk_one.vtx", "edge-chunk-one",
                                    /*chunk_max_frames=*/1);
    constexpr int kFrames = 10;
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        for (int i = 0; i < kFrames; ++i) {
            auto frame = MakeTrivialFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx);
    EXPECT_EQ(ctx.reader->GetTotalFrames(), kFrames);

    // With chunk_max_frames=1 we expect exactly one seek-table entry per frame.
    const auto& seek = ctx.reader->GetSeekTable();
    EXPECT_EQ(seek.size(), static_cast<size_t>(kFrames));
    for (size_t i = 0; i < seek.size(); ++i) {
        EXPECT_EQ(seek[i].start_frame, static_cast<int32_t>(i));
        EXPECT_EQ(seek[i].end_frame, static_cast<int32_t>(i));
    }
}

// ---------------------------------------------------------------------------
// Post-Stop API calls must not crash
// ---------------------------------------------------------------------------

TEST(WriterEdges, RecordFrameAfterStopIsSafe) {
    const auto cfg = MakeBaseConfig("edge_record_after_stop.vtx", "edge-post-stop");
    auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
    ASSERT_TRUE(writer);

    auto f1 = MakeTrivialFrame(0);
    VTX::GameTime::GameTimeRegister t;
    t.game_time = 0.0f;
    writer->RecordFrame(f1, t);
    writer->Flush();
    writer->Stop();

    // These calls happen after Stop() -- the contract doesn't promise they
    // record anything, but they must not crash, deadlock, or corrupt state.
    auto f2 = MakeTrivialFrame(1);
    t.game_time = 1.0f / 60.0f;
    writer->RecordFrame(f2, t);
    writer->Flush();

    writer.reset(); // release before inspecting

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx);
    // Whatever total_frames ends up being, the file should be readable.
    const int total = ctx.reader->GetTotalFrames();
    EXPECT_GE(total, 1); // at least the frame recorded before Stop
}

TEST(WriterEdges, DoubleStopIsIdempotent) {
    const auto cfg = MakeBaseConfig("edge_double_stop.vtx", "edge-double-stop");
    auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
    ASSERT_TRUE(writer);

    auto f = MakeTrivialFrame(0);
    VTX::GameTime::GameTimeRegister t;
    t.game_time = 0.0f;
    writer->RecordFrame(f, t);
    writer->Flush();

    writer->Stop();
    writer->Stop(); // second call must be a safe no-op
    writer->Stop(); // third for good measure

    writer.reset();

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx);
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 1);
}

// ---------------------------------------------------------------------------
// Frame that exceeds chunk_max_bytes
// ---------------------------------------------------------------------------

TEST(WriterEdges, GiantFrameLargerThanChunkMaxBytes) {
    // chunk_max_bytes = 1 KB deliberately -- a single frame with a 64 KB
    // string_properties entry is larger than that.  The writer has to
    // emit a chunk containing the oversize frame (it can't split a frame).
    const auto cfg = MakeBaseConfig("edge_giant_frame.vtx", "edge-giant",
                                    /*chunk_max_frames=*/1000,
                                    /*chunk_max_bytes=*/1024);
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_TRUE(writer);

        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {"giant", std::string(64 * 1024, 'X')};
        pc.int32_properties = {1, 0, 0};
        pc.float_properties = {100.0f, 50.0f};
        pc.vector_properties = {VTX::Vector {}, VTX::Vector {}};
        pc.quat_properties = {VTX::Quat {}};
        pc.bool_properties = {true};
        bucket.unique_ids.push_back("giant_id");
        bucket.entities.push_back(std::move(pc));

        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        writer->RecordFrame(f, t);
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 1);

    // Read the frame back -- the giant string must round-trip intact.
    const VTX::Frame* f = ctx.reader->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->GetBuckets().size(), 1u);
    const auto& entities = f->GetBuckets()[0].entities;
    ASSERT_EQ(entities.size(), 1u);
    ASSERT_GE(entities[0].string_properties.size(), 2u);
    EXPECT_EQ(entities[0].string_properties[1].size(), 64u * 1024u);
}
