// End-to-end round-trip tests: writer -> reader -> verify.
//
// The key correctness property of the SDK: what you write is what you read.
// Runs as a TEST_P parametrized over VTX::VtxFormat so every scenario covers
// both the FlatBuffers and Protobuf backends without copy-pasted bodies.

#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <string>

#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

namespace {

constexpr int   kTotalFrames    = 120;     // enough to force multiple chunks
constexpr int   kChunkMaxFrames = 40;      // = 3 chunks
constexpr float kFps            = 60.0f;

const char* FormatName(VTX::VtxFormat f) {
    return f == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf";
}

VTX::Frame BuildFrame(int frame_index) {
    VTX::Frame f;
    auto& bucket = f.CreateBucket("entity");

    VTX::PropertyContainer pc;
    pc.entity_type_id    = 0;
    pc.string_properties = { "player_0", "Alpha" };
    pc.int32_properties  = { 1, frame_index, 0 };                // Team, Score(=frame), Deaths
    pc.float_properties  = { 100.0f - float(frame_index), 50.0f };
    pc.vector_properties = {
        VTX::Vector{ double(frame_index), 0.0, 0.0 },
        VTX::Vector{ 1.0, 0.0, 0.0 }
    };
    pc.quat_properties = { VTX::Quat{0.0f, 0.0f, 0.0f, 1.0f} };
    pc.bool_properties = { true };

    bucket.unique_ids.push_back("player_0");
    bucket.entities.push_back(std::move(pc));
    return f;
}

} // namespace


// ===========================================================================
//  Parametrized fixture -- one body, runs against both backends.
// ===========================================================================

class RoundtripTest : public ::testing::TestWithParam<VTX::VtxFormat> {
protected:
    VTX::WriterFacadeConfig MakeConfig(const std::string& suffix,
                                       const std::string& uuid) const
    {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath  = VtxTest::OutputPath(
            std::string("roundtrip_") + FormatName(GetParam()) + "_" + suffix + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name      = "RoundtripTest";
        cfg.replay_uuid      = uuid;
        cfg.default_fps      = kFps;
        cfg.chunk_max_frames = kChunkMaxFrames;
        cfg.use_compression  = true;
        return cfg;
    }

    std::unique_ptr<VTX::IVtxWriterFacade> CreateWriter(
        const VTX::WriterFacadeConfig& cfg) const
    {
        return GetParam() == VTX::VtxFormat::FlatBuffers
            ? VTX::CreateFlatBuffersWriterFacade(cfg)
            : VTX::CreateProtobuffWriterFacade(cfg);
    }
};

// ---------------------------------------------------------------------------
// Every recorded value must round-trip identically back out of the reader.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, PreservesFrameData) {
    auto cfg = MakeConfig("preserves", "uuid-rt-preserves");
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < kTotalFrames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.format, GetParam());

    auto* reader = ctx.reader.get();
    EXPECT_EQ(reader->GetTotalFrames(), kTotalFrames);

    // Seek table reflects the chunking.
    const auto& seek_table = reader->GetSeekTable();
    EXPECT_GE(seek_table.size(), size_t(kTotalFrames / kChunkMaxFrames));

    // Header + footer fidelity.
    auto header = reader->GetHeader();
    EXPECT_EQ(header.replay_name, "RoundtripTest");

    auto footer = reader->GetFooter();
    EXPECT_EQ(footer.total_frames, kTotalFrames);

    // Spot-check boundary and middle frames.
    for (int frame_index : {0, kTotalFrames / 2, kTotalFrames - 1}) {
        const VTX::Frame* f = reader->GetFrameSync(frame_index);
        ASSERT_NE(f, nullptr) << "frame " << frame_index;
        ASSERT_EQ(f->GetBuckets().size(), 1u);
        const auto& entities = f->GetBuckets()[0].entities;
        ASSERT_EQ(entities.size(), 1u);

        const auto& e = entities[0];
        ASSERT_GE(e.int32_properties.size(), 2u);
        EXPECT_EQ(e.int32_properties[1], frame_index);

        ASSERT_GE(e.float_properties.size(), 1u);
        EXPECT_FLOAT_EQ(e.float_properties[0], 100.0f - float(frame_index));

        ASSERT_GE(e.vector_properties.size(), 1u);
        EXPECT_DOUBLE_EQ(e.vector_properties[0].x, double(frame_index));
    }
}

// ---------------------------------------------------------------------------
// Writer must accept historical (pre-"now") UTC timestamps -- regression for
// the VTXGameTimes fix of 2026-04-17.  Same contract on both backends.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, AcceptsHistoricalUtc) {
    auto cfg = MakeConfig("historical_utc", "uuid-rt-hist");
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);

        const int64_t base_utc = 1'745'000'000LL * 10'000'000LL;  // 2025-04-19
        for (int i = 0; i < 20; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time        = float(i) / kFps;
            t.created_utc_time = base_utc + int64_t(i) * 166'666LL;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 20);
}

// ---------------------------------------------------------------------------
// Backend instantiation -- produces:
//   BothBackends/RoundtripTest.PreservesFrameData/FlatBuffers
//   BothBackends/RoundtripTest.PreservesFrameData/Protobuf
//   BothBackends/RoundtripTest.AcceptsHistoricalUtc/FlatBuffers
//   BothBackends/RoundtripTest.AcceptsHistoricalUtc/Protobuf
// ---------------------------------------------------------------------------

INSTANTIATE_TEST_SUITE_P(
    BothBackends,
    RoundtripTest,
    ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
    [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
        return std::string(FormatName(info.param));
    }
);
