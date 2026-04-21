// Parametrized differ coverage to exercise both binary backends.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "vtx/differ/core/vtx_differ_facade.h"
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

std::unique_ptr<VTX::IVtxWriterFacade> CreateWriter(VTX::VtxFormat format,
                                                    const VTX::WriterFacadeConfig& cfg)
{
    return format == VTX::VtxFormat::FlatBuffers
        ? VTX::CreateFlatBuffersWriterFacade(cfg)
        : VTX::CreateProtobuffWriterFacade(cfg);
}

VTX::PropertyContainer MakePlayer(float health)
{
    VTX::PropertyContainer pc;
    pc.entity_type_id = 0;
    pc.string_properties = {"player_0", "Alpha"};
    pc.int32_properties = {1, 0, 0};
    pc.float_properties = {health, 50.0f};
    pc.vector_properties = {VTX::Vector{0.0, 0.0, 0.0}, VTX::Vector{1.0, 0.0, 0.0}};
    pc.quat_properties = {VTX::Quat{0.0f, 0.0f, 0.0f, 1.0f}};
    pc.bool_properties = {true};
    return pc;
}

template <typename F0, typename F1>
std::string WriteTwoFrameReplay(VTX::VtxFormat format,
                                const std::string& path,
                                F0 build0,
                                F1 build1)
{
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = path;
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name = "DiffParametrized";
    cfg.replay_uuid = "diff-param";
    cfg.default_fps = 60.0f;
    cfg.chunk_max_frames = 8;
    cfg.use_compression = true;

    auto writer = CreateWriter(format, cfg);
    {
        auto frame = build0();
        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        writer->RecordFrame(frame, t);
    }
    {
        auto frame = build1();
        VTX::GameTime::GameTimeRegister t;
        t.game_time = 1.0f / 60.0f;
        writer->RecordFrame(frame, t);
    }
    writer->Stop();
    return path;
}

VtxDiff::PatchIndex DiffFrames(VTX::VtxFormat format, const std::string& path)
{
    auto ctx = VTX::OpenReplayFile(path);
    if (!ctx) {
        ADD_FAILURE() << ctx.error;
        return {};
    }
    EXPECT_EQ(ctx.format, format);

    auto differ = VtxDiff::CreateDifferFacade(format);
    if (!differ) {
        ADD_FAILURE() << "CreateDifferFacade returned nullptr";
        return {};
    }

    auto raw_a = ctx.reader->GetRawFrameBytes(0);
    std::vector<std::byte> bytes_a(raw_a.begin(), raw_a.end());
    const auto raw_b = ctx.reader->GetRawFrameBytes(1);
    return differ->DiffRawFrames(bytes_a, raw_b);
}

} // namespace

class DifferParametrizedTest : public ::testing::TestWithParam<VTX::VtxFormat> {};

TEST_P(DifferParametrizedTest, IdenticalFramesProduceZeroOpsOnBothBackends)
{
    const auto build = [] {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(MakePlayer(100.0f));
        return f;
    };

    const auto path = WriteTwoFrameReplay(GetParam(), UniqueOutputPath(GetParam(), "identical"), build, build);
    const auto patch = DiffFrames(GetParam(), path);
    EXPECT_TRUE(patch.operations.empty());
}

TEST_P(DifferParametrizedTest, FloatReplaceIsDetectedOnBothBackends)
{
    const auto build0 = [] {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(MakePlayer(100.0f));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(MakePlayer(75.0f));
        return f;
    };

    const auto path = WriteTwoFrameReplay(GetParam(), UniqueOutputPath(GetParam(), "replace"), build0, build1);
    const auto patch = DiffFrames(GetParam(), path);

    bool saw_float_replace = false;
    for (const auto& op : patch.operations) {
        if (op.ContainerType == VtxDiff::EVTXContainerType::FloatProperties &&
            (op.Operation == VtxDiff::DiffOperation::Replace ||
             op.Operation == VtxDiff::DiffOperation::ReplaceRange)) {
            saw_float_replace = true;
            EXPECT_EQ(op.ActorId, "player_0");
        }
    }
    EXPECT_TRUE(saw_float_replace);
}

TEST_P(DifferParametrizedTest, EmptyInputsReturnEmptyPatch)
{
    auto differ = VtxDiff::CreateDifferFacade(GetParam());
    ASSERT_TRUE(differ);

    const std::vector<std::byte> empty_a;
    const std::vector<std::byte> empty_b;
    const auto patch = differ->DiffRawFrames(empty_a, empty_b);
    EXPECT_TRUE(patch.operations.empty());
}

INSTANTIATE_TEST_SUITE_P(
    BothBackends,
    DifferParametrizedTest,
    ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
    [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
        return std::string(FormatName(info.param));
    });
