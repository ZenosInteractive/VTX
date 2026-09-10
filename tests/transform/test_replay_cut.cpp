// Replay cut tests: CutReplayFile() writes a copy of a replay holding a sub-range
// of its frames. Whole chunks are copied verbatim (same checksum), edge chunks are
// re-serialized with only the kept frames, frames are renumbered from 0, the time
// table is sliced with absolute tick values, and the seek table describes the bytes
// on disk. Runs as a TEST_P over both backends.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/transform/vtx_replay_cut.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "util/replay_snapshot.h"
#include "util/test_fixtures.h"

namespace {

    constexpr int32_t kFrames = 100; // 3 chunks: [0,39] [40,79] [80,99]
    constexpr int32_t kChunkFrames = 40;
    constexpr float kFps = 60.0f;
    constexpr int64_t kBaseUtc = 17'000'000'000'000'000LL; // unix-relative 100 ns ticks
    constexpr int64_t kFrameTicks = 166'666;

    const char* FormatName(VTX::VtxFormat f) {
        return f == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf";
    }

    // Fixture schema (tests/fixtures/test_schema.json), Player (type 0): strings
    // [UniqueID, Name], int32 [Team, Score, Deaths], float [Health, Armor], ...
    VTX::Frame BuildFrame(int32_t frame) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        for (int32_t p = 0; p < 2; ++p) {
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            const std::string id = "player_" + std::to_string(p);
            pc.string_properties = {id, p == 0 ? "Alpha" : "Bravo"};
            pc.int32_properties = {p + 1, frame, frame / 10}; // Score carries the frame index
            pc.float_properties = {100.0f - float(frame % 50), 25.0f + float(p)};
            pc.vector_properties = {VTX::Vector {double(frame), double(p), 0.0}, VTX::Vector {1.0, 0.0, 0.0}};
            pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
            pc.bool_properties = {frame % 7 != 0};
            bucket.unique_ids.push_back(id);
            bucket.entities.push_back(std::move(pc));
        }
        return f;
    }

    // Output frame k must equal source frame (first + k), entity by entity.
    void ExpectFramesAreSourceRange(const VtxTest::ReplaySnapshot& source, const VtxTest::ReplaySnapshot& output,
                                    int32_t first, int32_t last) {
        ASSERT_EQ(output.frames.size(), static_cast<size_t>(last - first + 1));
        for (size_t k = 0; k < output.frames.size(); ++k) {
            const VTX::Frame& s = source.frames[static_cast<size_t>(first) + k];
            const VTX::Frame& o = output.frames[k];
            ASSERT_EQ(o.GetBuckets().size(), s.GetBuckets().size()) << "frame " << k;
            for (size_t b = 0; b < s.GetBuckets().size(); ++b) {
                const VTX::Bucket& sb = s.GetBuckets()[b];
                const VTX::Bucket& ob = o.GetBuckets()[b];
                ASSERT_EQ(ob.unique_ids, sb.unique_ids) << "frame " << k;
                ASSERT_EQ(ob.entities.size(), sb.entities.size()) << "frame " << k;
                for (size_t i = 0; i < sb.entities.size(); ++i) {
                    VtxTest::ExpectEntityEqual(sb.entities[i], ob.entities[i],
                                               "frame " + std::to_string(k) + " entity " + sb.unique_ids[i]);
                }
            }
        }
    }

    // The footer's time table is the source table sliced to [first, last], values untouched.
    void ExpectTimeTableSliced(const VtxTest::ReplaySnapshot& source, const VtxTest::ReplaySnapshot& output,
                               int32_t first, int32_t last) {
        const size_t n = static_cast<size_t>(last - first + 1);
        ASSERT_EQ(output.footer.times.created_utc.size(), n);
        ASSERT_EQ(output.footer.times.game_time.size(), n);
        for (size_t k = 0; k < n; ++k) {
            EXPECT_EQ(output.footer.times.created_utc[k],
                      source.footer.times.created_utc[static_cast<size_t>(first) + k])
                << k;
            EXPECT_EQ(output.footer.times.game_time[k], source.footer.times.game_time[static_cast<size_t>(first) + k])
                << k;
        }
        const double expected_duration = double(source.footer.times.created_utc[static_cast<size_t>(last)] -
                                                source.footer.times.created_utc[static_cast<size_t>(first)]) /
                                         double(VTX::GameTime::TICKS_PER_SECOND);
        EXPECT_NEAR(output.footer.duration_seconds, expected_duration, 1e-3);
        EXPECT_EQ(output.footer.total_frames, last - first + 1);
    }

} // namespace

class ReplayCutTest : public ::testing::TestWithParam<VTX::VtxFormat> {
protected:
    std::string Path(const std::string& suffix) const {
        return VtxTest::OutputPath(std::string("replay_cut_") + FormatName(GetParam()) + "_" + suffix + ".vtx");
    }

    std::string WriteSource(const std::string& suffix) const {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = Path(suffix + "_src");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "ReplayCutTest";
        cfg.replay_uuid = "uuid-cut-" + suffix;
        cfg.default_fps = kFps;
        cfg.chunk_max_frames = kChunkFrames;
        cfg.use_compression = true;

        auto writer = GetParam() == VTX::VtxFormat::FlatBuffers ? VTX::CreateFlatBuffersWriterFacade(cfg)
                                                                : VTX::CreateProtobufWriterFacade(cfg);
        EXPECT_TRUE(writer);
        if (!writer) {
            return {};
        }
        for (int32_t i = 0; i < kFrames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            t.created_utc_time = kBaseUtc + int64_t(i) * kFrameTicks;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
        return cfg.output_filepath;
    }
};

// ---------------------------------------------------------------------------
// A whole-chunk cut copies the chunk verbatim: same bytes (same checksum), frames
// renumbered from 0, time table sliced, header identical.
// ---------------------------------------------------------------------------
TEST_P(ReplayCutTest, WholeChunkCutCopiesVerbatim) {
    const std::string src = WriteSource("wholechunk");
    ASSERT_FALSE(src.empty());
    const VtxTest::ReplaySnapshot source = VtxTest::ReadReplay(src);
    ASSERT_TRUE(source.ok);
    ASSERT_EQ(source.footer.chunk_index.size(), 3u);

    const VTX::ReplayCutPlan plan = VTX::PlanCutChunks(source.footer, 1, 1);
    ASSERT_TRUE(plan.valid) << plan.error;
    EXPECT_EQ(plan.first_frame, 40);
    EXPECT_EQ(plan.last_frame, 79);
    EXPECT_FALSE(plan.trims_head);
    EXPECT_FALSE(plan.trims_tail);
    EXPECT_EQ(plan.FrameCount(), 40);
    EXPECT_EQ(plan.chunk_bytes, source.footer.chunk_index[1].chunk_size_bytes);

    const std::string dst = Path("wholechunk_out");
    const VTX::ReplayCutResult r = VTX::CutReplayFile(src, source.footer, plan, dst);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.total_frames, 40);
    EXPECT_EQ(r.chunks_written, 1);
    EXPECT_EQ(r.chunks_rewritten, 0);
    EXPECT_GT(r.output_bytes, 0u);

    const VtxTest::ReplaySnapshot output = VtxTest::ReadReplay(dst);
    ASSERT_TRUE(output.ok);
    EXPECT_EQ(output.format, source.format);
    EXPECT_EQ(output.header.replay_uuid, source.header.replay_uuid);
    EXPECT_EQ(output.header.recorded_utc_timestamp, source.header.recorded_utc_timestamp);
    EXPECT_EQ(output.schema.property_mapping, source.schema.property_mapping);

    ASSERT_EQ(output.footer.chunk_index.size(), 1u);
    EXPECT_EQ(output.footer.chunk_index[0].chunk_index, 0);
    EXPECT_EQ(output.footer.chunk_index[0].start_frame, 0);
    EXPECT_EQ(output.footer.chunk_index[0].end_frame, 39);
    EXPECT_EQ(output.footer.chunk_index[0].checksum, source.footer.chunk_index[1].checksum); // verbatim copy
    EXPECT_EQ(output.footer.chunk_index[0].chunk_size_bytes, source.footer.chunk_index[1].chunk_size_bytes);

    ExpectFramesAreSourceRange(source, output, 40, 79);
    ExpectTimeTableSliced(source, output, 40, 79);
    VtxTest::ExpectSeekTableMatchesFile(dst, output.footer);
}

// ---------------------------------------------------------------------------
// An exact-frame cut rewrites both edge chunks and keeps the middle one verbatim.
// ---------------------------------------------------------------------------
TEST_P(ReplayCutTest, ExactFrameCutRewritesEdges) {
    const std::string src = WriteSource("exact");
    ASSERT_FALSE(src.empty());
    const VtxTest::ReplaySnapshot source = VtxTest::ReadReplay(src);
    ASSERT_TRUE(source.ok);

    const VTX::ReplayCutPlan plan = VTX::PlanCutFrames(source.footer, 30, 85);
    ASSERT_TRUE(plan.valid) << plan.error;
    EXPECT_EQ(plan.first_chunk, 0);
    EXPECT_EQ(plan.last_chunk, 2);
    EXPECT_EQ(plan.first_frame, 30);
    EXPECT_EQ(plan.last_frame, 85);
    EXPECT_TRUE(plan.trims_head);
    EXPECT_TRUE(plan.trims_tail);
    EXPECT_EQ(plan.FrameCount(), 56);

    const std::string dst = Path("exact_out");
    const VTX::ReplayCutResult r = VTX::CutReplayFile(src, source.footer, plan, dst);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.total_frames, 56);
    EXPECT_EQ(r.chunks_written, 3);
    EXPECT_EQ(r.chunks_rewritten, 2);

    const VtxTest::ReplaySnapshot output = VtxTest::ReadReplay(dst);
    ASSERT_TRUE(output.ok);
    ASSERT_EQ(output.footer.chunk_index.size(), 3u);
    EXPECT_EQ(output.footer.chunk_index[0].start_frame, 0);
    EXPECT_EQ(output.footer.chunk_index[0].end_frame, 9); // source 30..39
    EXPECT_EQ(output.footer.chunk_index[1].start_frame, 10);
    EXPECT_EQ(output.footer.chunk_index[1].end_frame, 49); // source 40..79, verbatim
    EXPECT_EQ(output.footer.chunk_index[1].checksum, source.footer.chunk_index[1].checksum);
    EXPECT_EQ(output.footer.chunk_index[2].start_frame, 50);
    EXPECT_EQ(output.footer.chunk_index[2].end_frame, 55);                                   // source 80..85
    EXPECT_NE(output.footer.chunk_index[0].checksum, source.footer.chunk_index[0].checksum); // rewritten
    EXPECT_NE(output.footer.chunk_index[2].checksum, source.footer.chunk_index[2].checksum);

    ExpectFramesAreSourceRange(source, output, 30, 85);
    ExpectTimeTableSliced(source, output, 30, 85);
    VtxTest::ExpectSeekTableMatchesFile(dst, output.footer);
}

// Bounds are clamped to the replay and a head cut alone rewrites one chunk.
TEST_P(ReplayCutTest, ClampsBoundsAndTrimsOnlyOneEdge) {
    const std::string src = WriteSource("clamp");
    ASSERT_FALSE(src.empty());
    const VtxTest::ReplaySnapshot source = VtxTest::ReadReplay(src);
    ASSERT_TRUE(source.ok);

    const VTX::ReplayCutPlan plan = VTX::PlanCutFrames(source.footer, 95, 100'000);
    ASSERT_TRUE(plan.valid) << plan.error;
    EXPECT_EQ(plan.first_frame, 95);
    EXPECT_EQ(plan.last_frame, kFrames - 1);
    EXPECT_TRUE(plan.trims_head);
    EXPECT_FALSE(plan.trims_tail);

    const std::string dst = Path("clamp_out");
    const VTX::ReplayCutResult r = VTX::CutReplayFile(src, source.footer, plan, dst);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.total_frames, 5);
    EXPECT_EQ(r.chunks_written, 1);
    EXPECT_EQ(r.chunks_rewritten, 1);

    const VtxTest::ReplaySnapshot output = VtxTest::ReadReplay(dst);
    ASSERT_TRUE(output.ok);
    ExpectFramesAreSourceRange(source, output, 95, kFrames - 1);
    VtxTest::ExpectSeekTableMatchesFile(dst, output.footer);
}

// ---------------------------------------------------------------------------
// Failure paths report an error and never leave a destination behind.
// ---------------------------------------------------------------------------
TEST_P(ReplayCutTest, ErrorsAreReported) {
    namespace fs = std::filesystem;
    const std::string src = WriteSource("errors");
    ASSERT_FALSE(src.empty());
    const VtxTest::ReplaySnapshot source = VtxTest::ReadReplay(src);
    ASSERT_TRUE(source.ok);
    const std::string dst = Path("errors_out");

    {
        VTX::ReplayCutPlan invalid; // valid == false
        const auto r = VTX::CutReplayFile(src, source.footer, invalid, dst);
        EXPECT_FALSE(r.ok());
        EXPECT_FALSE(fs::exists(dst));
    }
    {
        const VTX::ReplayCutPlan plan = VTX::PlanCutChunks(source.footer, 0, 0);
        ASSERT_TRUE(plan.valid);
        const auto r = VTX::CutReplayFile(src, source.footer, plan, src);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("different file"), std::string::npos) << r.error;
        EXPECT_TRUE(fs::exists(src));
    }
    {
        const VTX::ReplayCutPlan plan = VTX::PlanCutChunks(source.footer, 0, 0);
        const auto r = VTX::CutReplayFile(Path("does_not_exist"), source.footer, plan, dst);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("does not exist"), std::string::npos) << r.error;
        EXPECT_FALSE(fs::exists(dst));
    }
    {
        // A plan built against a different footer must not index past this one.
        VTX::ReplayCutPlan stale = VTX::PlanCutChunks(source.footer, 0, 0);
        stale.last_chunk = 42;
        const auto r = VTX::CutReplayFile(src, source.footer, stale, dst);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("chunk index"), std::string::npos) << r.error;
        EXPECT_FALSE(fs::exists(dst));
    }
}

INSTANTIATE_TEST_SUITE_P(Formats, ReplayCutTest,
                         ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
                         [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) { return FormatName(info.param); });

// ===========================================================================
//  Plan resolution on a synthetic footer, no file involved.
// ===========================================================================
TEST(ReplayCutPlan, ResolvesRangesAgainstTheChunkIndex) {
    VTX::FileFooter footer;
    footer.total_frames = 100;
    for (int32_t i = 0; i < 3; ++i) {
        VTX::ChunkIndexEntry e;
        e.chunk_index = i;
        e.start_frame = i * 40;
        e.end_frame = std::min(i * 40 + 39, 99);
        e.chunk_size_bytes = 1000 + static_cast<uint32_t>(i);
        footer.chunk_index.push_back(e);
    }

    const auto whole = VTX::PlanCutChunks(footer, 0, 2);
    ASSERT_TRUE(whole.valid);
    EXPECT_EQ(whole.first_frame, 0);
    EXPECT_EQ(whole.last_frame, 99);
    EXPECT_FALSE(whole.trims_head || whole.trims_tail);
    EXPECT_EQ(whole.chunk_bytes, 3003u);

    const auto exact = VTX::PlanCutFrames(footer, 45, 45);
    ASSERT_TRUE(exact.valid);
    EXPECT_EQ(exact.first_chunk, 1);
    EXPECT_EQ(exact.last_chunk, 1);
    EXPECT_TRUE(exact.trims_head);
    EXPECT_TRUE(exact.trims_tail);
    EXPECT_EQ(exact.FrameCount(), 1);

    const auto boundary = VTX::PlanCutFrames(footer, 40, 79);
    ASSERT_TRUE(boundary.valid);
    EXPECT_FALSE(boundary.trims_head || boundary.trims_tail); // exactly chunk 1

    EXPECT_FALSE(VTX::PlanCutFrames(footer, 50, 10).valid);
    EXPECT_FALSE(VTX::PlanCutChunks(footer, 2, 1).valid);
    EXPECT_FALSE(VTX::PlanCutFrames(VTX::FileFooter {}, 0, 0).valid);
    EXPECT_EQ(VTX::ReplayCutPlan {}.FrameCount(), 0);
}
