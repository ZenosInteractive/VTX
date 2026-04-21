// Reader facade API coverage beyond OpenReplayFile smoke tests.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "util/test_fixtures.h"

namespace {

const char* FormatName(VTX::VtxFormat format)
{
    return format == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf";
}

std::string UniqueOutputPath(VTX::VtxFormat format, const std::string& suffix)
{
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return VtxTest::OutputPath(
        VtxTest::SanitizePathComponent(std::string(info->test_suite_name())) + "_" +
        VtxTest::SanitizePathComponent(std::string(info->name())) + "_" +
        FormatName(format) + "_" + suffix + ".vtx");
}

VTX::WriterFacadeConfig MakeConfig(VTX::VtxFormat format,
                                   const std::string& suffix,
                                   int32_t chunk_max_frames)
{
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

std::unique_ptr<VTX::IVtxWriterFacade> CreateWriter(VTX::VtxFormat format,
                                                    const VTX::WriterFacadeConfig& cfg)
{
    return format == VTX::VtxFormat::FlatBuffers
        ? VTX::CreateFlatBuffersWriterFacade(cfg)
        : VTX::CreateProtobuffWriterFacade(cfg);
}

VTX::Frame MakePlayerFrame(int frame_index)
{
    VTX::Frame f;
    auto& bucket = f.CreateBucket("entity");

    VTX::PropertyContainer pc;
    pc.entity_type_id = 0;
    pc.string_properties = {"player_0", "Alpha"};
    pc.int32_properties = {1, frame_index, 0};
    pc.float_properties = {100.0f - float(frame_index), 50.0f};
    pc.vector_properties = {
        VTX::Vector{double(frame_index), 0.0, 0.0},
        VTX::Vector{1.0, 0.0, 0.0}
    };
    pc.quat_properties = {VTX::Quat{0.0f, 0.0f, 0.0f, 1.0f}};
    pc.bool_properties = {true};

    bucket.unique_ids.push_back("player_0");
    bucket.entities.push_back(std::move(pc));
    return f;
}

void WriteReplay(VTX::VtxFormat format,
                 const std::string& path,
                 int frames,
                 int32_t chunk_max_frames)
{
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

int ReadScore(const VTX::Frame& frame)
{
    return frame.GetBuckets()[0].entities[0].int32_properties[1];
}

} // namespace

class ReaderApiTest : public ::testing::TestWithParam<VTX::VtxFormat> {};

TEST_P(ReaderApiTest, CreateAccessorReadsSchemaDrivenEntityValues)
{
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

TEST_P(ReaderApiTest, FrameRangeAndContextReturnExpectedFrames)
{
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

TEST_P(ReaderApiTest, OutOfBoundsQueriesReturnEmptyResults)
{
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

TEST(ReaderApiFlatBuffers, CacheWindowZeroEvictsPreviousChunks)
{
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

INSTANTIATE_TEST_SUITE_P(
    BothBackends,
    ReaderApiTest,
    ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
    [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
        return std::string(FormatName(info.param));
    });
