// Tests for frame finalization & strict writer behavior.
//
//   #1 FinalizeFrame validates before the frame enters the chunk pipeline.
//   #2 content_hash is recomputed after schema init and after .Set() overrides.
//   #4 Finalization freezes the frame: mutation handles are revoked.
//   #5 The last finalized frame is retained as an opt-in read-only snapshot,
//      queryable by (bucket, unique_id).
//   #6 Game-time rejection is observable via TryRecordFrame's RecordResult.
//   #7 Frame index is assigned monotonically by the writer (rejected frames do
//      not consume an index).
//   #8 RecordPipeline::Run reports written / rejected / skipped / validation /
//      timer outcomes separately.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vtx/common/vtx_frame_accessor.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/writer/core/vtx_data_source.h"
#include "vtx/writer/core/vtx_frame_mutation_view.h"
#include "vtx/writer/core/vtx_record_pipeline.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/core/vtx_writer_result.h"

#include "util/test_fixtures.h"

namespace {

    using VTX::FrameRejectReason;

    // Player schema layout (test_schema.json):
    //   string[0]=UniqueID string[1]=Name
    //   int32[0]=Team int32[1]=Score int32[2]=Deaths
    //   float[0]=Health float[1]=Armor
    VTX::PropertyContainer MakePlayer(const std::string& uid, int32_t team, float health) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {uid, "name_" + uid};
        pc.int32_properties = {team, 0, 0};
        pc.float_properties = {health, 50.0f};
        return pc;
    }

    VTX::Frame MakeFrameWith(std::vector<VTX::PropertyContainer> players) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        for (auto& p : players) {
            const std::string id = p.string_properties.empty() ? std::string {} : p.string_properties[0];
            bucket.unique_ids.push_back(id);
            bucket.entities.push_back(std::move(p));
        }
        return f;
    }

    VTX::GameTime::GameTimeRegister Increasing(float t) {
        VTX::GameTime::GameTimeRegister r;
        r.game_time = t;
        r.FrameFilterType = VTX::GameTime::EFilterType::OnlyIncreasing;
        return r;
    }

    VTX::WriterFacadeConfig MakeConfig(const std::string& uuid, bool retain_snapshot = false) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("frame_finalization_" + uuid + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "FrameFinalizationTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 8;
        cfg.use_compression = true;
        cfg.retain_finalized_snapshot = retain_snapshot;
        return cfg;
    }

    class SetHealthProcessor : public VTX::IFramePostProcessor {
    public:
        explicit SetHealthProcessor(float v)
            : value_(v) {}
        void Init(const VTX::FramePostProcessorInitContext& ctx) override {
            key_ = ctx.frame_accessor->Get<float>("Player", "Health");
        }
        void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext&) override {
            if (!key_.IsValid())
                return;
            auto bucket = view.GetBucket("entity");
            for (auto entity : bucket) {
                entity.Set(key_, value_);
            }
        }
        void Clear() override {}

    private:
        float value_;
        VTX::PropertyKey<float> key_ {-1};
    };

    // In-memory data source that replays a canned list of (frame, time) pairs.
    class CannedSource : public VTX::IFrameDataSource {
    public:
        void Add(VTX::Frame frame, VTX::GameTime::GameTimeRegister time) {
            frames_.emplace_back(std::move(frame), time);
        }
        bool Initialize() override { return true; }
        bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
            if (cursor_ >= frames_.size())
                return false;
            out_frame = frames_[cursor_].first; // copy: TryRecordFrame moves it out
            out_time = frames_[cursor_].second;
            ++cursor_;
            return true;
        }
        size_t GetExpectedTotalFrames() const override { return frames_.size(); }

    private:
        std::vector<std::pair<VTX::Frame, VTX::GameTime::GameTimeRegister>> frames_;
        size_t cursor_ = 0;
    };

    std::string TestUuid() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        return VtxTest::SanitizePathComponent(std::string(info->test_suite_name()) + "_" + info->name());
    }

} // namespace

// ---------------------------------------------------------------------------
// #6 / #7 -- TryRecordFrame reports outcomes; index is monotonic.
// ---------------------------------------------------------------------------

TEST(FrameFinalization, TryRecordFrameReportsWrittenWithMonotonicIndex) {
    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid()));
    ASSERT_NE(writer, nullptr);

    auto f0 = MakeFrameWith({MakePlayer("a", 1, 100.0f)});
    const auto r0 = writer->TryRecordFrame(f0, Increasing(0.0f));
    EXPECT_TRUE(r0.IsWritten());
    EXPECT_EQ(r0.reason, FrameRejectReason::None);
    EXPECT_EQ(r0.frame_index, 0);

    auto f1 = MakeFrameWith({MakePlayer("b", 1, 100.0f)});
    const auto r1 = writer->TryRecordFrame(f1, Increasing(1.0f));
    EXPECT_TRUE(r1.IsWritten());
    EXPECT_EQ(r1.frame_index, 1);

    writer->Stop();
}

TEST(FrameFinalization, TryRecordFrameRejectsBadGameTimeAndKeepsIndexContiguous) {
    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid()));
    ASSERT_NE(writer, nullptr);

    auto f0 = MakeFrameWith({MakePlayer("a", 1, 100.0f)});
    const auto r0 = writer->TryRecordFrame(f0, Increasing(0.0f));
    ASSERT_TRUE(r0.IsWritten());
    EXPECT_EQ(r0.frame_index, 0);

    // Decreasing time while OnlyIncreasing -> rejected by the timer.
    auto bad = MakeFrameWith({MakePlayer("b", 1, 100.0f)});
    const auto rbad = writer->TryRecordFrame(bad, Increasing(-50.0f));
    EXPECT_FALSE(rbad.IsWritten());
    EXPECT_EQ(rbad.reason, FrameRejectReason::GameTimeRejected);
    EXPECT_FALSE(rbad.detail.empty());
    EXPECT_EQ(rbad.frame_index, -1);

    // The rejected frame did not consume an index: the next written frame is 1.
    auto f1 = MakeFrameWith({MakePlayer("c", 1, 100.0f)});
    const auto r1 = writer->TryRecordFrame(f1, Increasing(1.0f));
    EXPECT_TRUE(r1.IsWritten());
    EXPECT_EQ(r1.frame_index, 1);

    writer->Stop();
}

// ---------------------------------------------------------------------------
// #1 -- validation runs before serialization (unresolved entity type rejected).
// ---------------------------------------------------------------------------

TEST(FrameFinalization, TryRecordFrameRejectsUnresolvedEntityType) {
    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid(), /*retain_snapshot=*/true));
    ASSERT_NE(writer, nullptr);

    auto frame = MakeFrameWith({MakePlayer("a", 1, 100.0f)});
    frame.GetMutableBuckets()[0].entities[0].entity_type_id = -1; // does not resolve to a schema struct

    const auto r = writer->TryRecordFrame(frame, Increasing(0.0f));
    EXPECT_FALSE(r.IsWritten());
    EXPECT_EQ(r.reason, FrameRejectReason::ValidationFailed);
    EXPECT_FALSE(r.detail.empty());

    // A rejected frame leaves no snapshot behind.
    EXPECT_EQ(writer->GetLastFinalizedFrame(), nullptr);

    writer->Stop();
}

// ---------------------------------------------------------------------------
// #5 -- last finalized frame is retained and queryable by (bucket, unique_id).
// ---------------------------------------------------------------------------

TEST(FrameFinalization, LastFinalizedSnapshotIsQueryable) {
    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid(), /*retain_snapshot=*/true));
    ASSERT_NE(writer, nullptr);

    EXPECT_EQ(writer->GetLastFinalizedFrame(), nullptr); // nothing finalized yet

    auto frame = MakeFrameWith({MakePlayer("a", 1, 100.0f), MakePlayer("b", 2, 80.0f)});
    ASSERT_TRUE(writer->TryRecordFrame(frame, Increasing(0.0f)).IsWritten());

    ASSERT_NE(writer->GetLastFinalizedFrame(), nullptr);
    const auto* ea = writer->FindEntity("entity", "a");
    ASSERT_NE(ea, nullptr);
    EXPECT_EQ(ea->int32_properties[0], 1);
    const auto* eb = writer->FindEntity("entity", "b");
    ASSERT_NE(eb, nullptr);
    EXPECT_EQ(eb->int32_properties[0], 2);

    EXPECT_EQ(writer->FindEntity("entity", "does_not_exist"), nullptr);
    EXPECT_EQ(writer->FindEntity("no_such_bucket", "a"), nullptr);

    // Snapshot tracks the *last* finalized frame only.
    auto frame2 = MakeFrameWith({MakePlayer("c", 3, 50.0f)});
    ASSERT_TRUE(writer->TryRecordFrame(frame2, Increasing(1.0f)).IsWritten());
    EXPECT_EQ(writer->FindEntity("entity", "a"), nullptr);
    EXPECT_NE(writer->FindEntity("entity", "c"), nullptr);

    writer->Stop();
}

// ---------------------------------------------------------------------------
// #2 -- content_hash recomputed after init and after post-processor overrides.
// ---------------------------------------------------------------------------

TEST(FrameFinalization, ContentHashIsRecomputedDuringFinalization) {
    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid(), /*retain_snapshot=*/true));
    ASSERT_NE(writer, nullptr);

    auto frame = MakeFrameWith({MakePlayer("a", 1, 123.0f)});
    frame.GetMutableBuckets()[0].entities[0].content_hash = 0xDEADBEEFull; // stale / bogus

    ASSERT_TRUE(writer->TryRecordFrame(frame, Increasing(0.0f)).IsWritten());

    const auto* e = writer->FindEntity("entity", "a");
    ASSERT_NE(e, nullptr);
    EXPECT_NE(e->content_hash, 0xDEADBEEFull);
    EXPECT_EQ(e->content_hash, VTX::Helpers::CalculateContainerHash(*e));

    writer->Stop();
}

TEST(FrameFinalization, ContentHashReflectsPostProcessorOverrides) {
    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid(), /*retain_snapshot=*/true));
    ASSERT_NE(writer, nullptr);
    writer->SetPostProcessor(std::make_shared<SetHealthProcessor>(7.0f));

    auto frame = MakeFrameWith({MakePlayer("a", 1, 100.0f)});
    ASSERT_TRUE(writer->TryRecordFrame(frame, Increasing(0.0f)).IsWritten());

    const auto* e = writer->FindEntity("entity", "a");
    ASSERT_NE(e, nullptr);
    EXPECT_FLOAT_EQ(e->float_properties[0], 7.0f);                        // override applied
    EXPECT_EQ(e->content_hash, VTX::Helpers::CalculateContainerHash(*e)); // hash matches final content

    writer->Stop();
}

// ---------------------------------------------------------------------------
// #4 -- finalization freezes the frame: mutation handles are revoked.
// ---------------------------------------------------------------------------

TEST(FrameFinalization, FreezeRevokesMutationHandles) {
    VTX::SchemaRegistry registry;
    ASSERT_TRUE(registry.LoadFromJson(VtxTest::FixturePath("test_schema.json")));
    VTX::FrameAccessor accessor;
    accessor.InitializeFromCache(registry.GetPropertyCache());

    VTX::Frame frame = MakeFrameWith({MakePlayer("a", 1, 100.0f)});
    VTX::FrameMutationView view(frame, accessor);

    const auto health = accessor.Get<float>("Player", "Health");
    ASSERT_TRUE(health.IsValid());

    auto bucket = view.GetBucket("entity");
    auto entity = bucket.entity(0);
    ASSERT_TRUE(bucket.valid());
    ASSERT_TRUE(entity.valid());
    const float before = entity.Get(health);

    view.Freeze();

    // Stashed handles are revoked.
    EXPECT_FALSE(bucket.valid());
    EXPECT_FALSE(entity.valid());
    EXPECT_EQ(entity.raw(), nullptr); // mutable raw access revoked

    // Mutating through a frozen handle is a no-op.
    entity.Set(health, before + 100.0f);
    EXPECT_FLOAT_EQ(frame.GetMutableBuckets()[0].entities[0].float_properties[0], before);

    // Newly requested handles from a frozen view are dead too.
    auto bucket_after = view.GetBucket("entity");
    EXPECT_FALSE(bucket_after.valid());
    EXPECT_FALSE(bucket_after.AddEntity().valid());
    EXPECT_EQ(bucket_after.entity_count(), 1u); // AddEntity was a no-op
}

// ---------------------------------------------------------------------------
// #5 (opt-in) -- the snapshot is off unless retain_finalized_snapshot is set.
// ---------------------------------------------------------------------------

TEST(FrameFinalization, SnapshotIsDisabledByDefault) {
    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid())); // retain off (default)
    ASSERT_NE(writer, nullptr);

    auto frame = MakeFrameWith({MakePlayer("a", 1, 100.0f)});
    ASSERT_TRUE(writer->TryRecordFrame(frame, Increasing(0.0f)).IsWritten());

    // The frame was written, but no snapshot is retained without opt-in.
    EXPECT_EQ(writer->GetLastFinalizedFrame(), nullptr);
    EXPECT_EQ(writer->FindEntity("entity", "a"), nullptr);

    writer->Stop();
}

// ---------------------------------------------------------------------------
// #8 -- pipeline reports outcomes separately.
// ---------------------------------------------------------------------------

TEST(FrameFinalization, PipelineReportSeparatesOutcomes) {
    auto source = std::make_unique<CannedSource>();
    source->Add(MakeFrameWith({MakePlayer("a", 1, 100.0f)}), Increasing(0.0f));  // written 0
    source->Add(MakeFrameWith({MakePlayer("a", 1, 100.0f)}), Increasing(1.0f));  // written 1
    source->Add(MakeFrameWith({MakePlayer("a", 1, 100.0f)}), Increasing(-9.0f)); // timer reject
    source->Add(MakeFrameWith({MakePlayer("a", 1, 100.0f)}), Increasing(2.0f));  // written 2

    auto bad_type = MakeFrameWith({MakePlayer("z", 1, 100.0f)});
    bad_type.GetMutableBuckets()[0].entities[0].entity_type_id = -1;
    source->Add(std::move(bad_type), Increasing(3.0f)); // validation reject

    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeConfig(TestUuid()));
    ASSERT_NE(writer, nullptr);

    VTX::RecordPipeline pipeline(std::move(source), std::move(writer));
    const VTX::PipelineReport report = pipeline.Run();

    EXPECT_EQ(report.written, 3u);
    EXPECT_EQ(report.rejected, 2u);
    EXPECT_EQ(report.timer_errors, 1u);
    EXPECT_EQ(report.validation_errors, 1u);
    EXPECT_EQ(report.skipped, 0u);
    EXPECT_EQ(report.Total(), 5u);
}
