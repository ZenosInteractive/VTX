// Tests for VtxDiff::IVtxDifferFacade::DiffRawFrames -- structural tree diff.
//
// Builds tiny .vtx files with controlled differences frame-to-frame, then
// asserts on the shape of the resulting PatchIndex.

#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>

#include "vtx/differ/core/vtx_differ_facade.h"
#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

namespace {

    VTX::PropertyContainer MakePlayer(const std::string& uid, const std::string& name, int team, float health,
                                      float armor, VTX::Vector pos, bool is_alive = true, int score = 0,
                                      int deaths = 0) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {uid, name};
        pc.int32_properties = {team, score, deaths};
        pc.float_properties = {health, armor};
        pc.vector_properties = {pos, VTX::Vector {0.0, 0.0, 0.0}};
        pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
        pc.bool_properties = {is_alive};
        return pc;
    }

    /// Builds a two-frame .vtx file from raw frame-builder callbacks.  Returns the
    /// path of the written file.
    template <typename F1, typename F2>
    std::string WriteTwoFrameFile(const std::string& uuid, F1 build_frame_0, F2 build_frame_1) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("diff_" + uuid + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "DiffTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 16;
        cfg.use_compression = true;

        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        {
            VTX::Frame f0 = build_frame_0();
            VTX::GameTime::GameTimeRegister t;
            t.game_time = 0.0f;
            writer->RecordFrame(f0, t);
        }
        {
            VTX::Frame f1 = build_frame_1();
            VTX::GameTime::GameTimeRegister t;
            t.game_time = 1.0f / 60.0f;
            writer->RecordFrame(f1, t);
        }
        writer->Flush();
        writer->Stop();
        return cfg.output_filepath;
    }

    /// Opens a file, pulls raw bytes of frames 0 and 1, and runs the differ.
    /// Returns the PatchIndex.  We need to copy A's bytes because loading B may
    /// evict A's chunk.
    VtxDiff::PatchIndex DiffFile(const std::string& path, const VtxDiff::DiffOptions& opts = {}) {
        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            ADD_FAILURE() << ctx.error;
            return {};
        }

        auto differ = VtxDiff::CreateDifferFacade(ctx.format);
        if (!differ) {
            ADD_FAILURE() << "CreateDifferFacade returned nullptr";
            return {};
        }

        auto raw_a = ctx.reader->GetRawFrameBytes(0);
        std::vector<std::byte> bytes_a(raw_a.begin(), raw_a.end());

        auto raw_b = ctx.reader->GetRawFrameBytes(1);
        return differ->DiffRawFrames(bytes_a, raw_b, opts);
    }

} // namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

TEST(Differ, FactoryReturnsNullForUnknownFormat) {
    EXPECT_EQ(VtxDiff::CreateDifferFacade(VTX::VtxFormat::Unknown), nullptr);
}

TEST(Differ, FactoryReturnsNonNullForFlatBuffers) {
    EXPECT_NE(VtxDiff::CreateDifferFacade(VTX::VtxFormat::FlatBuffers), nullptr);
}

TEST(Differ, FactoryReturnsNonNullForProtobuf) {
    EXPECT_NE(VtxDiff::CreateDifferFacade(VTX::VtxFormat::Protobuf), nullptr);
}

// ---------------------------------------------------------------------------
// Identical frames
// ---------------------------------------------------------------------------

TEST(Differ, IdenticalFramesProduceZeroOps) {
    const auto build = [] {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto path = WriteTwoFrameFile("identical", build, build);

    auto patch = DiffFile(path);
    EXPECT_TRUE(patch.operations.empty());
}

// ---------------------------------------------------------------------------
// Replace ops
// ---------------------------------------------------------------------------

TEST(Differ, FloatPropertyChangeShowsUpAsReplace) {
    const auto build0 = [] {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        bucket.unique_ids.push_back("player_0");
        // Health dropped from 100 -> 75.  Everything else identical.
        bucket.entities.push_back(MakePlayer("player_0", "Alpha", 1, 75.0f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto path = WriteTwoFrameFile("replace_float", build0, build1);

    auto patch = DiffFile(path);
    ASSERT_FALSE(patch.operations.empty());

    bool saw_float_replace = false;
    for (const auto& op : patch.operations) {
        if (op.ContainerType == VtxDiff::EVTXContainerType::FloatProperties &&
            (op.Operation == VtxDiff::DiffOperation::Replace || op.Operation == VtxDiff::DiffOperation::ReplaceRange)) {
            saw_float_replace = true;
            EXPECT_EQ(op.ActorId, "player_0");
        }
    }
    EXPECT_TRUE(saw_float_replace);
}

TEST(Differ, VectorPropertyChangeShowsUpAsReplace) {
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {10.0, 0.0, 0.0}));
        return f;
    };
    const auto path = WriteTwoFrameFile("replace_vec", build0, build1);

    auto patch = DiffFile(path);
    ASSERT_FALSE(patch.operations.empty());

    bool saw_vector_op = false;
    for (const auto& op : patch.operations) {
        if (op.ContainerType == VtxDiff::EVTXContainerType::VectorProperties) {
            saw_vector_op = true;
            EXPECT_EQ(op.ActorId, "player_0");
        }
    }
    EXPECT_TRUE(saw_vector_op);
}

// ---------------------------------------------------------------------------
// Float epsilon
// ---------------------------------------------------------------------------

TEST(Differ, FloatWithinEpsilonProducesNoOp) {
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.000001f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.000002f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto path = WriteTwoFrameFile("epsilon_ignored", build0, build1);

    VtxDiff::DiffOptions opts;
    opts.compare_floats_with_epsilon = true;
    opts.float_epsilon = 1e-3f;
    auto patch = DiffFile(path, opts);

    // Any float-containing op would be unexpected with epsilon this large.
    for (const auto& op : patch.operations) {
        EXPECT_NE(op.ContainerType, VtxDiff::EVTXContainerType::FloatProperties);
    }
}

TEST(Differ, FloatOutsideEpsilonProducesOp) {
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 110.0f, 50.0f, VTX::Vector {0.0, 0.0, 0.0}));
        return f;
    };
    const auto path = WriteTwoFrameFile("epsilon_exceeded", build0, build1);

    VtxDiff::DiffOptions opts;
    opts.compare_floats_with_epsilon = true;
    opts.float_epsilon = 1e-3f;
    auto patch = DiffFile(path, opts);

    bool saw_float = false;
    for (const auto& op : patch.operations) {
        if (op.ContainerType == VtxDiff::EVTXContainerType::FloatProperties) {
            saw_float = true;
        }
    }
    EXPECT_TRUE(saw_float);
}

// ---------------------------------------------------------------------------
// Add / Remove
// ---------------------------------------------------------------------------

TEST(Differ, EntityAddedBetweenFramesYieldsAddOp) {
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0, 0, 0}));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids = {"player_0", "player_1"};
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0, 0, 0}));
        b.entities.push_back(MakePlayer("player_1", "Bravo", 2, 80.0f, 40.0f, VTX::Vector {5, 0, 0}));
        return f;
    };
    const auto path = WriteTwoFrameFile("added_entity", build0, build1);
    auto patch = DiffFile(path);

    bool saw_add = false;
    for (const auto& op : patch.operations) {
        if (op.Operation == VtxDiff::DiffOperation::Add)
            saw_add = true;
    }
    EXPECT_TRUE(saw_add);
}

TEST(Differ, EntityRemovedBetweenFramesYieldsRemoveOp) {
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids = {"player_0", "player_1"};
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0, 0, 0}));
        b.entities.push_back(MakePlayer("player_1", "Bravo", 2, 80.0f, 40.0f, VTX::Vector {5, 0, 0}));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("player_0");
        b.entities.push_back(MakePlayer("player_0", "Alpha", 1, 100.0f, 50.0f, VTX::Vector {0, 0, 0}));
        return f;
    };
    const auto path = WriteTwoFrameFile("removed_entity", build0, build1);
    auto patch = DiffFile(path);

    bool saw_remove = false;
    for (const auto& op : patch.operations) {
        if (op.Operation == VtxDiff::DiffOperation::Remove)
            saw_remove = true;
    }
    EXPECT_TRUE(saw_remove);
}

// ---------------------------------------------------------------------------
// DiffIndexPath sanity checks (lightweight, no file needed)
// ---------------------------------------------------------------------------

TEST(DiffIndexPath, AppendProducesExpectedSequence) {
    VtxDiff::DiffIndexPath p;
    EXPECT_TRUE(p.IsEmpty());
    auto p2 = p.Append(3).Append(7).Append(9);
    ASSERT_EQ(p2.size(), 3u);
    EXPECT_EQ(p2[0], 3);
    EXPECT_EQ(p2[1], 7);
    EXPECT_EQ(p2[2], 9);
}

TEST(DiffIndexPath, PushBackSilentlyDropsOverflow) {
    VtxDiff::DiffIndexPath p;
    for (int i = 0; i < 20; ++i)
        p.push_back(i); // capacity is 16
    EXPECT_EQ(p.count, 16);
}

TEST(DiffIndexPath, EqualityUsesCountAndContents) {
    auto a = VtxDiff::DiffIndexPath {}.Append(1).Append(2);
    auto b = VtxDiff::DiffIndexPath {}.Append(1).Append(2);
    auto c = VtxDiff::DiffIndexPath {}.Append(1).Append(3);
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a == c);
}
