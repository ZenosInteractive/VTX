// Tests for in-memory schema support: WriterFacadeConfig::schema_json_content
// (raw JSON string) and WriterFacadeConfig::schema_registry (pre-built registry),
// plus their precedence over schema_json_path.

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "util/test_fixtures.h"

namespace {

    std::string ReadFileText(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
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

    void WriteFrames(VTX::IVtxWriterFacade& writer, int count) {
        for (int i = 0; i < count; ++i) {
            auto frame = MakeSimpleFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer.RecordFrame(frame, t);
        }
        writer.Stop();
    }

    VTX::WriterFacadeConfig MakeConfig(const std::string& suffix) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("schema_in_memory_" + suffix + ".vtx");
        cfg.replay_name = "SchemaInMemoryTest";
        cfg.default_fps = 60.0f;
        return cfg; // schema source filled in by each test
    }

} // namespace

// A schema supplied as a raw JSON string (no file path) drives a readable replay.
TEST(SchemaInMemory, SchemaFromContentProducesReadableReplay) {
    const std::string schema_json = ReadFileText(VtxTest::FixturePath("test_schema.json"));
    ASSERT_FALSE(schema_json.empty());

    auto cfg = MakeConfig("content");
    cfg.schema_json_content = schema_json;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_NE(writer, nullptr);
        WriteFrames(*writer, 4);
    } // writer destroyed -> file fully flushed before we read it

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.GetError().message;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 4);
}

// A pre-built SchemaRegistry injected into the config drives a readable replay.
TEST(SchemaInMemory, InjectedRegistryProducesReadableReplay) {
    auto registry = std::make_shared<VTX::SchemaRegistry>();
    ASSERT_TRUE(registry->LoadFromJson(VtxTest::FixturePath("test_schema.json")));

    auto cfg = MakeConfig("registry");
    cfg.schema_registry = registry;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_NE(writer, nullptr);
        WriteFrames(*writer, 3);
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.GetError().message;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 3);
}

// Precedence: an injected registry is used even when schema_json_path is bogus.
TEST(SchemaInMemory, InjectedRegistryWinsOverBogusPath) {
    auto registry = std::make_shared<VTX::SchemaRegistry>();
    ASSERT_TRUE(registry->LoadFromJson(VtxTest::FixturePath("test_schema.json")));

    auto cfg = MakeConfig("registry_wins");
    cfg.schema_json_path = VtxTest::OutputPath("schema_in_memory_does_not_exist.json");
    cfg.schema_registry = registry;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_NE(writer, nullptr);
        WriteFrames(*writer, 2);
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.GetError().message;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 2);
}

// Precedence: in-memory content is used even when schema_json_path is bogus.
TEST(SchemaInMemory, ContentWinsOverBogusPath) {
    const std::string schema_json = ReadFileText(VtxTest::FixturePath("test_schema.json"));
    ASSERT_FALSE(schema_json.empty());

    auto cfg = MakeConfig("content_wins");
    cfg.schema_json_path = VtxTest::OutputPath("schema_in_memory_does_not_exist.json");
    cfg.schema_json_content = schema_json;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_NE(writer, nullptr);
        WriteFrames(*writer, 2);
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.GetError().message;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 2);
}

// Malformed in-memory schema content is rejected: the factory returns nullptr.
TEST(SchemaInMemory, InvalidContentFailsWriterCreation) {
    auto cfg = MakeConfig("bad_content");
    cfg.schema_json_content = "{ this is not valid schema json";

    auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
    EXPECT_EQ(writer, nullptr);
}

// The Protobuf backend honours in-memory schema content as well.
TEST(SchemaInMemory, ProtobufBackendWithContent) {
    const std::string schema_json = ReadFileText(VtxTest::FixturePath("test_schema.json"));
    ASSERT_FALSE(schema_json.empty());

    auto cfg = MakeConfig("proto_content");
    cfg.schema_json_content = schema_json;

    {
        auto writer = VTX::CreateProtobufWriterFacade(cfg);
        ASSERT_NE(writer, nullptr);
        WriteFrames(*writer, 3);
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.GetError().message;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 3);
}
