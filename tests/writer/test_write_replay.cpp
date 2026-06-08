// Tests for the one-call VTX::WriteReplay pipeline.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_write_replay.h"

#include "util/test_fixtures.h"

namespace {

    VTX::PropertyContainer MakePlayer(const std::string& id) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {id, "name_" + id};
        pc.int32_properties = {1, 0, 0};
        pc.float_properties = {100.0f, 50.0f};
        return pc;
    }

    VTX::Frame MakeFrame(const std::string& id) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.unique_ids.push_back(id);
        bucket.entities.push_back(MakePlayer(id));
        return f;
    }

    VTX::GameTime::GameTimeRegister Increasing(float t) {
        VTX::GameTime::GameTimeRegister r;
        r.game_time = t;
        r.FrameFilterType = VTX::GameTime::EFilterType::OnlyIncreasing;
        return r;
    }

    class CannedSource : public VTX::IFrameDataSource {
    public:
        void Add(VTX::Frame frame, VTX::GameTime::GameTimeRegister time) {
            items_.emplace_back(std::move(frame), time);
        }
        void FailInitialize() { init_ok_ = false; }

        bool Initialize() override { return init_ok_; }
        bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
            if (cursor_ >= items_.size()) {
                return false;
            }
            out_frame = items_[cursor_].first;
            out_time = items_[cursor_].second;
            ++cursor_;
            return true;
        }
        size_t GetExpectedTotalFrames() const override { return items_.size(); }

    private:
        std::vector<std::pair<VTX::Frame, VTX::GameTime::GameTimeRegister>> items_;
        size_t cursor_ = 0;
        bool init_ok_ = true;
    };

    VTX::WriterFacadeConfig MakeConfig(const std::string& uuid) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("write_replay_" + uuid + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "WriteReplayTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        return cfg;
    }

} // namespace

TEST(WriteReplay, WritesAllFramesAndIsReadable) {
    CannedSource source;
    for (int i = 0; i < 4; ++i) {
        source.Add(MakeFrame("p" + std::to_string(i)), Increasing(static_cast<float>(i)));
    }

    const auto result = VTX::WriteReplay(MakeConfig("happy"), source);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.frames_written, 4u);
    EXPECT_EQ(result.frames_dropped, 0u);
    EXPECT_EQ(result.total_frames, 4);
    EXPECT_FALSE(result.output_path.empty());
    EXPECT_TRUE(result.warnings.empty());

    auto ctx = VTX::OpenReplayFile(result.output_path);
    ASSERT_TRUE(ctx) << ctx.GetError().message;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 4);
}

TEST(WriteReplay, DroppedFramesAreReportedAsWarnings) {
    CannedSource source;
    source.Add(MakeFrame("a"), Increasing(0.0f));
    source.Add(MakeFrame("b"), Increasing(1.0f));
    source.Add(MakeFrame("c"), Increasing(-5.0f)); // decreasing -> rejected by the timer
    source.Add(MakeFrame("d"), Increasing(2.0f));

    const auto result = VTX::WriteReplay(MakeConfig("dropped"), source);
    EXPECT_TRUE(result.ok); // the operation still completes
    EXPECT_EQ(result.frames_written, 3u);
    EXPECT_EQ(result.frames_dropped, 1u);
    ASSERT_EQ(result.warnings.size(), 1u);
    EXPECT_EQ(result.warnings[0].code, VTX::VtxErrorCode::GameTimeRejected);
}

TEST(WriteReplay, InvalidSchemaFailsWithError) {
    CannedSource source;
    source.Add(MakeFrame("a"), Increasing(0.0f));

    auto cfg = MakeConfig("badschema");
    cfg.schema_json_path = VtxTest::OutputPath("write_replay_missing_schema.json");

    const auto result = VTX::WriteReplay(cfg, source);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.frames_written, 0u);
    EXPECT_NE(result.error.code, VTX::VtxErrorCode::None);
    EXPECT_FALSE(result.error.message.empty());
}

TEST(WriteReplay, SourceInitFailureFailsWithError) {
    CannedSource source;
    source.FailInitialize();

    const auto result = VTX::WriteReplay(MakeConfig("initfail"), source);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, VTX::VtxErrorCode::Internal);
    EXPECT_EQ(result.frames_written, 0u);
}

TEST(WriteReplay, ProtobufBackendWritesReadableReplay) {
    CannedSource source;
    for (int i = 0; i < 3; ++i) {
        source.Add(MakeFrame("p" + std::to_string(i)), Increasing(static_cast<float>(i)));
    }

    const auto result = VTX::WriteReplay(MakeConfig("proto"), source, VTX::SerializationFormat::Protobuf);
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.frames_written, 3u);

    auto ctx = VTX::OpenReplayFile(result.output_path);
    ASSERT_TRUE(ctx) << ctx.GetError().message;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 3);
}
