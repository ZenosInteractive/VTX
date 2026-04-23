// Basic writer construction/record/stop smoke tests.

#include <gtest/gtest.h>
#include <filesystem>

#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

namespace {

    VTX::WriterFacadeConfig MakeConfig(const std::string& out_name, const std::string& uuid) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath(out_name);
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "WriterBasicTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 50;
        cfg.use_compression = true;
        return cfg;
    }

    VTX::Frame MakeOneEntityFrame() {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {"id_a", "Alpha"};
        pc.int32_properties = {1, 0, 0};       // Team, Score, Deaths
        pc.float_properties = {100.0f, 50.0f}; // Health, Armor
        pc.vector_properties = {VTX::Vector {0.0, 0.0, 0.0}, VTX::Vector {0.0, 0.0, 0.0}};
        pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
        pc.bool_properties = {true};

        bucket.unique_ids.push_back("id_a");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

} // namespace

TEST(WriterBasic, FlatBuffersFactoryProducesValidWriter) {
    const auto cfg = MakeConfig("writer_basic_fbs.vtx", "uuid-fbs-basic");
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_TRUE(writer);
        writer->Stop();
    } // destructor releases any remaining file handles
    EXPECT_TRUE(std::filesystem::exists(cfg.output_filepath));
    EXPECT_GT(std::filesystem::file_size(cfg.output_filepath), 0u);
}

TEST(WriterBasic, ProtobufFactoryProducesValidWriter) {
    const auto cfg = MakeConfig("writer_basic_proto.vtx", "uuid-proto-basic");
    {
        auto writer = VTX::CreateProtobuffWriterFacade(cfg);
        ASSERT_TRUE(writer);
        writer->Stop();
    }
    EXPECT_TRUE(std::filesystem::exists(cfg.output_filepath));
    EXPECT_GT(std::filesystem::file_size(cfg.output_filepath), 0u);
}

TEST(WriterBasic, RecordAndStopWritesNonEmptyFile) {
    const auto cfg = MakeConfig("writer_basic_record.vtx", "uuid-record");
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_TRUE(writer);

        for (int i = 0; i < 5; ++i) {
            auto frame = MakeOneEntityFrame();
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
    } // release writer so the file is fully closed before we inspect it

    ASSERT_TRUE(std::filesystem::exists(cfg.output_filepath));
    EXPECT_GT(std::filesystem::file_size(cfg.output_filepath), 100u);
}
