// Tests for VTX::OpenReplayFile + ReaderContext behaviour and lifetime.
//
// Two groups:
//   - ReaderContextHappy -- uses a TEST_F fixture so every test starts with a
//                           freshly-written .vtx and an open ReaderContext.
//   - ReaderContextFailure -- plain TEST for invalid/corrupt file cases where
//                             the fixture setup would actively get in the way.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

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
    cfg.output_filepath  = VtxTest::OutputPath("reader_" + uuid + ".vtx");
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name      = "ReaderContextTest";
    cfg.replay_uuid      = uuid;
    cfg.default_fps      = 60.0f;
    cfg.chunk_max_frames = 10;
    cfg.use_compression  = true;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        for (int i = 0; i < 5; ++i) {
            VTX::Frame f;
            auto& bucket = f.CreateBucket("entity");

            VTX::PropertyContainer pc;
            pc.entity_type_id    = 0;
            pc.string_properties = {"p", "name"};
            pc.int32_properties  = {1, 0, 0};
            pc.float_properties  = {100.0f, 50.0f};
            pc.vector_properties = {VTX::Vector{}, VTX::Vector{}};
            pc.quat_properties   = {VTX::Quat{}};
            pc.bool_properties   = {true};

            bucket.unique_ids.push_back("p");
            bucket.entities.push_back(std::move(pc));

            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(f, t);
        }
        writer->Flush();
        writer->Stop();
    }  // release -- file is fully closed before returning
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
        const std::string uuid =
            std::string(info->test_suite_name()) + "_" + info->name();

        path_ = WriteTinyFlatBuffersFile(uuid);
        ctx_  = VTX::OpenReplayFile(path_);
        ASSERT_TRUE(ctx_) << ctx_.error;
    }

    std::string        path_;
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
