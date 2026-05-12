#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/core/vtx_frame_post_processor.h"
#include "vtx/writer/core/vtx_frame_mutation_view.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "util/test_fixtures.h"

namespace {

    constexpr int kPlayersPerFrame = 3;

    VTX::Frame MakePlayerFrame(int frame_index) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        for (int p = 0; p < kPlayersPerFrame; ++p) {
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.string_properties = {"player_" + std::to_string(p), "Alpha_" + std::to_string(p)};
            pc.int32_properties = {p % 2 == 0 ? 1 : 2, frame_index * 10 + p, 0};
            pc.float_properties = {100.0f - float(frame_index), 50.0f};
            pc.vector_properties = {VTX::Vector {double(frame_index), double(p), 0.0}, VTX::Vector {}};
            pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
            pc.bool_properties = {true};

            bucket.unique_ids.push_back("player_" + std::to_string(p));
            bucket.entities.push_back(std::move(pc));
        }
        return f;
    }

    std::string OutPath(const std::string& uuid) {
        return VtxTest::OutputPath("writer_post_processor_" + uuid + ".vtx");
    }

    VTX::WriterFacadeConfig MakeConfig(const std::string& uuid) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = OutPath(uuid);
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "WriterPostProcessorTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 8;
        cfg.use_compression = true;
        return cfg;
    }

    void RunWriter(VTX::IVtxWriterFacade& writer, int frames) {
        for (int i = 0; i < frames; ++i) {
            auto frame = MakePlayerFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer.RecordFrame(frame, t);
        }
        writer.Flush();
        writer.Stop();
    }


    class DoubleHealthProcessor : public VTX::IFramePostProcessor {
    public:
        void Init(const VTX::FramePostProcessorInitContext& ctx) override {
            init_called_ = true;
            ASSERT_NE(ctx.frame_accessor, nullptr);
            health_key_ = ctx.frame_accessor->Get<float>("Player", "Health");
        }
        void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext& ctx) override {
            ++process_calls_;
            last_global_frame_ = ctx.global_frame_index;
            if (!health_key_.IsValid())
                return;
            auto bucket = view.GetBucket("entity");
            for (auto entity : bucket) {
                entity.Set(health_key_, entity.Get(health_key_) * 2.0f);
            }
        }
        void Clear() override { clear_called_ = true; }

        bool init_called_ = false;
        bool clear_called_ = false;
        int process_calls_ = 0;
        int32_t last_global_frame_ = -1;
        VTX::PropertyKey<float> health_key_ {-1};
    };

    class ScoreSetter : public VTX::IFramePostProcessor {
    public:
        explicit ScoreSetter(int32_t tag)
            : tag_(tag) {}
        void Init(const VTX::FramePostProcessorInitContext& ctx) override {
            score_key_ = ctx.frame_accessor->Get<int32_t>("Player", "Score");
        }
        void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext&) override {
            if (!score_key_.IsValid())
                return;
            auto bucket = view.GetBucket("entity");
            for (auto entity : bucket) {
                entity.Set(score_key_, tag_);
            }
        }
        int32_t tag_;
        VTX::PropertyKey<int32_t> score_key_ {-1};
    };

    class GhostInjector : public VTX::IFramePostProcessor {
    public:
        void Init(const VTX::FramePostProcessorInitContext& ctx) override {
            team_key_ = ctx.frame_accessor->Get<int32_t>("Player", "Team");
            health_key_ = ctx.frame_accessor->Get<float>("Player", "Health");
        }
        void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext&) override {
            auto bucket = view.GetBucket("entity");
            auto ghost = bucket.AddEntity();
            if (auto* raw = ghost.raw()) {
                raw->entity_type_id = 0;
                raw->int32_properties.resize(3, 0);
                raw->float_properties.resize(2, 0.0f);
            }
            if (team_key_.IsValid())
                ghost.Set(team_key_, 99);
            if (health_key_.IsValid())
                ghost.Set(health_key_, 1.0f);
        }
        VTX::PropertyKey<int32_t> team_key_ {-1};
        VTX::PropertyKey<float> health_key_ {-1};
    };

    class TeamTwoFilter : public VTX::IFramePostProcessor {
    public:
        void Init(const VTX::FramePostProcessorInitContext& ctx) override {
            team_key_ = ctx.frame_accessor->Get<int32_t>("Player", "Team");
        }
        void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext&) override {
            if (!team_key_.IsValid())
                return;
            auto bucket = view.GetBucket("entity");
            bucket.RemoveIf([this](VTX::EntityView e) { return e.Get(team_key_) == 2; });
        }
        VTX::PropertyKey<int32_t> team_key_ {-1};
    };

    class FrameIndexRecorder : public VTX::IFramePostProcessor {
    public:
        void Process(VTX::FrameMutationView&, const VTX::FramePostProcessContext& ctx) override {
            indices_.push_back(ctx.global_frame_index);
        }
        std::vector<int32_t> indices_;
    };

} // namespace


TEST(WriterPostProcessor_MutationViewUnit, SetThenGetRoundTrips) {
    VTX::PropertyContainer pc;
    pc.float_properties = {0.0f};
    VTX::EntityMutator m(pc);
    VTX::PropertyKey<float> key {0};
    m.Set(key, 42.5f);
    EXPECT_FLOAT_EQ(m.Get(key), 42.5f);
}

TEST(WriterPostProcessor_ChainUnit, OrderAndRemove) {
    VTX::FramePostProcessorChain chain;
    auto a = std::make_shared<ScoreSetter>(1);
    auto b = std::make_shared<ScoreSetter>(2);
    chain.Add(a);
    chain.Add(b);
    EXPECT_EQ(chain.size(), 2u);
    EXPECT_TRUE(chain.Remove(a));
    EXPECT_EQ(chain.size(), 1u);
    EXPECT_FALSE(chain.Remove(a));
}


class WriterPostProcessorTest : public ::testing::Test {
protected:
    std::string TestUuid() const {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        return VtxTest::SanitizePathComponent(info->name());
    }
};


TEST_F(WriterPostProcessorTest, NoProcessorBaselineUnchanged) {
    const auto cfg = MakeConfig(TestUuid());
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        RunWriter(*writer, 5);
    }
    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    const auto* frame = ctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    EXPECT_FLOAT_EQ(frame->GetBuckets()[0].entities[0].float_properties[0], 100.0f);
}

TEST_F(WriterPostProcessorTest, DoubleHealthIsPersistedToDisk) {
    const auto cfg = MakeConfig(TestUuid());
    auto proc = std::make_shared<DoubleHealthProcessor>();
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        writer->SetPostProcessor(proc);
        EXPECT_TRUE(proc->init_called_);
        EXPECT_TRUE(proc->health_key_.IsValid()) << "Init must have resolved the Health key against the loaded schema";
        RunWriter(*writer, 5);
    }
    EXPECT_TRUE(proc->clear_called_);
    EXPECT_EQ(proc->process_calls_, 5);

    auto rctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(rctx) << rctx.error;
    const auto* frame = rctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    EXPECT_FLOAT_EQ(frame->GetBuckets()[0].entities[0].float_properties[0], 200.0f);
    EXPECT_FLOAT_EQ(frame->GetBuckets()[0].entities[1].float_properties[0], 200.0f);
}

TEST_F(WriterPostProcessorTest, ChainLastWriterWinsOnDisk) {
    const auto cfg = MakeConfig(TestUuid());
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        auto chain = std::make_shared<VTX::FramePostProcessorChain>();
        chain->Add(std::make_shared<ScoreSetter>(11));
        chain->Add(std::make_shared<ScoreSetter>(22));
        writer->SetPostProcessor(chain);
        RunWriter(*writer, 3);
    }
    auto rctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(rctx) << rctx.error;
    const auto* frame = rctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->GetBuckets()[0].entities[0].int32_properties[1], 22);
}

TEST_F(WriterPostProcessorTest, ChainRemoveDropsAndOtherStillFires) {
    const auto cfg = MakeConfig(TestUuid());
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        auto chain = std::make_shared<VTX::FramePostProcessorChain>();
        auto a = std::make_shared<ScoreSetter>(11);
        auto b = std::make_shared<ScoreSetter>(22);
        chain->Add(a);
        chain->Add(b);
        chain->Remove(a);
        writer->SetPostProcessor(chain);
        RunWriter(*writer, 3);
    }
    auto rctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(rctx) << rctx.error;
    const auto* frame = rctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->GetBuckets()[0].entities[0].int32_properties[1], 22);
}

TEST_F(WriterPostProcessorTest, GhostInjectorEntityIsOnDisk) {
    const auto cfg = MakeConfig(TestUuid());
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        writer->SetPostProcessor(std::make_shared<GhostInjector>());
        RunWriter(*writer, 3);
    }
    auto rctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(rctx) << rctx.error;
    const auto* frame = rctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    const auto& entities = frame->GetBuckets()[0].entities;
    EXPECT_EQ(entities.size(), kPlayersPerFrame + 1);
    EXPECT_EQ(entities.back().int32_properties[0], 99);
    EXPECT_FLOAT_EQ(entities.back().float_properties[0], 1.0f);
}

TEST_F(WriterPostProcessorTest, TeamTwoFilterDropsEntitiesFromDisk) {
    const auto cfg = MakeConfig(TestUuid());
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        writer->SetPostProcessor(std::make_shared<TeamTwoFilter>());
        RunWriter(*writer, 3);
    }
    auto rctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(rctx) << rctx.error;
    const auto* frame = rctx.reader->GetFrameSync(0);
    ASSERT_NE(frame, nullptr);
    const auto& entities = frame->GetBuckets()[0].entities;
    EXPECT_EQ(entities.size(), 2u);
    for (const auto& e : entities) {
        EXPECT_NE(e.int32_properties[0], 2);
    }
}

TEST_F(WriterPostProcessorTest, GlobalFrameIndexIsMonotonic) {
    const auto cfg = MakeConfig(TestUuid());
    auto recorder = std::make_shared<FrameIndexRecorder>();
    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        writer->SetPostProcessor(recorder);
        RunWriter(*writer, 7);
    }
    ASSERT_EQ(recorder->indices_.size(), 7u);
    for (size_t i = 0; i < recorder->indices_.size(); ++i) {
        EXPECT_EQ(recorder->indices_[i], static_cast<int32_t>(i));
    }
}

TEST_F(WriterPostProcessorTest, ClearPostProcessorCallsClearAndUnregisters) {
    const auto cfg = MakeConfig(TestUuid());
    auto proc = std::make_shared<DoubleHealthProcessor>();
    auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
    writer->SetPostProcessor(proc);

    for (int i = 0; i < 2; ++i) {
        auto f = MakePlayerFrame(i);
        VTX::GameTime::GameTimeRegister t;
        t.game_time = float(i) / 60.0f;
        writer->RecordFrame(f, t);
    }
    writer->ClearPostProcessor();
    EXPECT_TRUE(proc->clear_called_);
    EXPECT_EQ(writer->GetPostProcessor(), nullptr);

    const int calls_before = proc->process_calls_;
    auto f = MakePlayerFrame(2);
    VTX::GameTime::GameTimeRegister t;
    t.game_time = 2.0f / 60.0f;
    writer->RecordFrame(f, t);
    EXPECT_EQ(proc->process_calls_, calls_before);

    writer->Flush();
    writer->Stop();
}
