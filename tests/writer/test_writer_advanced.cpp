// Additional writer tests covering chunking, sorting, metadata defaults, and
// failure paths.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

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

    VTX::WriterFacadeConfig MakeConfig(VTX::VtxFormat format, const std::string& suffix, int32_t chunk_max_frames,
                                       bool use_compression) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = UniqueOutputPath(format, suffix);
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "WriterAdvancedTest";
        cfg.replay_uuid = "";
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = chunk_max_frames;
        cfg.use_compression = use_compression;
        return cfg;
    }

    std::unique_ptr<VTX::IVtxWriterFacade> CreateWriter(VTX::VtxFormat format, const VTX::WriterFacadeConfig& cfg) {
        return format == VTX::VtxFormat::FlatBuffers ? VTX::CreateFlatBuffersWriterFacade(cfg)
                                                     : VTX::CreateProtobuffWriterFacade(cfg);
    }

    VTX::Frame MakeSimpleFrame(int frame_index) {
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

    VTX::Frame MakeUnsortedTypesFrame() {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer type1_a;
        type1_a.entity_type_id = 1;
        type1_a.string_properties = {"id_b", "Bravo"};

        VTX::PropertyContainer type0;
        type0.entity_type_id = 0;
        type0.string_properties = {"id_a", "Alpha"};

        VTX::PropertyContainer type1_b;
        type1_b.entity_type_id = 1;
        type1_b.string_properties = {"id_c", "Charlie"};

        bucket.unique_ids = {"id_b", "id_a", "id_c"};
        bucket.entities.push_back(type1_a);
        bucket.entities.push_back(type0);
        bucket.entities.push_back(type1_b);
        return f;
    }

} // namespace

class WriterAdvancedTest : public ::testing::TestWithParam<VTX::VtxFormat> {};

TEST_P(WriterAdvancedTest, ChunkMaxFramesProducesExpectedSeekTable) {
    const auto cfg = MakeConfig(GetParam(), "chunking", 1, false);

    {
        auto writer = CreateWriter(GetParam(), cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < 3; ++i) {
            auto frame = MakeSimpleFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    const auto& seek_table = ctx.reader->GetSeekTable();

    ASSERT_EQ(seek_table.size(), 3u);
    EXPECT_EQ(seek_table[0].chunk_index, 0);
    EXPECT_EQ(seek_table[0].start_frame, 0);
    EXPECT_EQ(seek_table[0].end_frame, 0);
    EXPECT_EQ(seek_table[1].chunk_index, 1);
    EXPECT_EQ(seek_table[1].start_frame, 1);
    EXPECT_EQ(seek_table[1].end_frame, 1);
    EXPECT_EQ(seek_table[2].chunk_index, 2);
    EXPECT_EQ(seek_table[2].start_frame, 2);
    EXPECT_EQ(seek_table[2].end_frame, 2);
}

TEST_P(WriterAdvancedTest, SortsEntitiesByTypeAndBuildsTypeRanges) {
    const auto cfg = MakeConfig(GetParam(), "sorted_types", 8, true);

    {
        auto writer = CreateWriter(GetParam(), cfg);
        ASSERT_TRUE(writer);
        auto frame = MakeUnsortedTypesFrame();
        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        writer->RecordFrame(frame, t);
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;

    const VTX::Frame* frame = ctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    ASSERT_FALSE(frame->GetBuckets().empty());

    const auto& bucket = frame->GetBuckets()[0];
    ASSERT_EQ(bucket.entities.size(), 3u);
    ASSERT_EQ(bucket.unique_ids.size(), 3u);
    ASSERT_EQ(bucket.type_ranges.size(), 2u);

    EXPECT_EQ(bucket.entities[0].entity_type_id, 0);
    EXPECT_EQ(bucket.unique_ids[0], "id_a");
    EXPECT_EQ(bucket.entities[1].entity_type_id, 1);
    EXPECT_EQ(bucket.unique_ids[1], "id_b");
    EXPECT_EQ(bucket.entities[2].entity_type_id, 1);
    EXPECT_EQ(bucket.unique_ids[2], "id_c");

    EXPECT_EQ(bucket.type_ranges[0].start_index, 0);
    EXPECT_EQ(bucket.type_ranges[0].count, 1);
    EXPECT_EQ(bucket.type_ranges[1].start_index, 1);
    EXPECT_EQ(bucket.type_ranges[1].count, 2);
}

TEST_P(WriterAdvancedTest, UncompressedFilesRemainReadableAndUseDefaultUuid) {
    const auto cfg = MakeConfig(GetParam(), "uncompressed", 8, false);

    {
        auto writer = CreateWriter(GetParam(), cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < 2; ++i) {
            auto frame = MakeSimpleFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.format, GetParam());

    const auto header = ctx.reader->GetHeader();
    EXPECT_EQ(header.replay_name, "WriterAdvancedTest");
    EXPECT_EQ(header.replay_uuid, "uuid_placeholder");

    const auto footer = ctx.reader->GetFooter();
    EXPECT_EQ(footer.total_frames, 2);
}

TEST_P(WriterAdvancedTest, StopWithoutFramesProducesReadableEmptyReplay) {
    const auto cfg = MakeConfig(GetParam(), "empty", 8, true);

    {
        auto writer = CreateWriter(GetParam(), cfg);
        ASSERT_TRUE(writer);
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 0);
    EXPECT_TRUE(ctx.reader->GetSeekTable().empty());
}

TEST_P(WriterAdvancedTest, ThrowsWhenParentDirectoryDoesNotExist) {
    auto cfg = MakeConfig(GetParam(), "missing_dir", 8, true);
    const std::filesystem::path missing_dir =
        std::filesystem::path(VtxTest::OutputPath("writer_missing_dir")) / "nested";
    std::filesystem::remove_all(missing_dir.parent_path());
    cfg.output_filepath = (missing_dir / "out.vtx").string();

    EXPECT_THROW(
        {
            auto writer = CreateWriter(GetParam(), cfg);
            (void)writer;
        },
        std::runtime_error);
}

INSTANTIATE_TEST_SUITE_P(BothBackends, WriterAdvancedTest,
                         ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
                         [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
                             return std::string(FormatName(info.param));
                         });
