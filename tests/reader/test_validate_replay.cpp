// Tests for ValidateReplay (already-open reader) and ValidateReplayFile (path).

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "vtx/common/vtx_diagnostics.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/reader/core/vtx_replay_validation.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "util/test_fixtures.h"

namespace {

    bool HasCode(const VTX::ValidationReport& report, VTX::VtxErrorCode code) {
        for (const auto& d : report.Diagnostics()) {
            if (d.code == code) {
                return true;
            }
        }
        return false;
    }

    VTX::Frame MakePlayerFrame(int frame_index, int players) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        for (int p = 0; p < players; ++p) {
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0; // Player
            pc.string_properties = {"player_" + std::to_string(p), "Name_" + std::to_string(p)};
            pc.int32_properties = {p % 2, frame_index, 0};
            pc.float_properties = {100.0f, 50.0f};
            bucket.unique_ids.push_back("player_" + std::to_string(p));
            bucket.entities.push_back(std::move(pc));
        }
        return f;
    }

    std::string WriteValidReplay(const std::string& uuid) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("validate_replay_" + uuid + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "ValidateReplayTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;

        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        for (int i = 0; i < 4; ++i) {
            auto frame = MakePlayerFrame(i, 3);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = static_cast<float>(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
        return cfg.output_filepath;
    }

} // namespace

TEST(ValidateReplay, CleanReplayHasNoErrors) {
    const std::string path = WriteValidReplay("clean");
    const auto report = VTX::ValidateReplayFile(path);
    EXPECT_TRUE(report.ok()) << report.ToString();
}

TEST(ValidateReplay, MissingFileReportsOpenFailure) {
    const auto report = VTX::ValidateReplayFile(VtxTest::OutputPath("does_not_exist_zzz.vtx"));
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(HasCode(report, VTX::VtxErrorCode::ReplayOpenFailed) ||
                HasCode(report, VTX::VtxErrorCode::ReplayNotReady));
}

TEST(ValidateReplay, SchemaOnlyOptionSkipsFrames) {
    const std::string path = WriteValidReplay("schema_only");
    VTX::ReplayValidationOptions options;
    options.validate_frames = false;
    const auto report = VTX::ValidateReplayFile(path, options);
    EXPECT_TRUE(report.ok()) << report.ToString();
}

// The core overload validates a reader the caller already opened -- no re-open.
TEST(ValidateReplay, OpenReaderOverloadValidatesWithoutReopening) {
    const std::string path = WriteValidReplay("open_reader");

    VTX::ReaderContext ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(static_cast<bool>(ctx)) << ctx.GetError();
    ASSERT_NE(ctx.reader, nullptr);

    const auto report = VTX::ValidateReplay(*ctx.reader);
    EXPECT_TRUE(report.ok()) << report.ToString();
}
