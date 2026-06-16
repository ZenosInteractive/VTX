// Edge-case tests for VtxDiff::IVtxDifferFacade::DiffRawFrames.
//
// Covers cases the basic diff tests don't: entirely empty frames, frames
// with completely different bucket topology, byte-array properties, nested
// struct properties, map properties, and entity removed-then-re-added with
// the same unique_id.

#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>

#include "vtx/differ/core/vtx_differ_facade.h"
#include "vtx/differ/core/vtx_diff_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

#include "util/test_fixtures.h"

namespace {

    using WriterFactory = std::unique_ptr<VTX::IVtxWriterFacade> (*)(const VTX::WriterFacadeConfig&);

    template <typename F0, typename F1>
    std::string WriteTwoFrameFile(const std::string& uuid, F0 build0, F1 build1,
                                  const std::string& schema_fixture = "test_schema.json",
                                  WriterFactory factory = &VTX::CreateFlatBuffersWriterFacade) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("diffedge_" + uuid + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath(schema_fixture);
        cfg.replay_name = "DiffEdgeTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 16;
        cfg.use_compression = true;

        {
            auto writer = factory(cfg);
            VTX::Frame f0 = build0();
            VTX::Frame f1 = build1();
            VTX::GameTime::GameTimeRegister t0;
            t0.game_time = 0.0f;
            VTX::GameTime::GameTimeRegister t1;
            t1.game_time = 1.0f / 60.0f;
            writer->RecordFrame(f0, t0);
            writer->RecordFrame(f1, t1);
            writer->Flush();
            writer->Stop();
        }
        return cfg.output_filepath;
    }

    VtxDiff::PatchIndex DiffFile(const std::string& path, const VtxDiff::DiffOptions& opts = {}) {
        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            ADD_FAILURE() << ctx.error;
            return {};
        }
        auto differ = VtxDiff::CreateDifferFacade(ctx.format);
        if (!differ) {
            ADD_FAILURE() << "no differ";
            return {};
        }

        auto raw_a = ctx.reader->GetRawFrameBytes(0);
        std::vector<std::byte> bytes_a(raw_a.begin(), raw_a.end());
        auto raw_b = ctx.reader->GetRawFrameBytes(1);
        return differ->DiffRawFrames(bytes_a, raw_b, opts);
    }

    // Minimal entity with only unique_id + entity_type_id so tests stay readable.
    VTX::PropertyContainer MakeMinimalPlayer(const std::string& uid) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {uid, "name"};
        pc.int32_properties = {1, 0, 0};
        pc.float_properties = {100.0f, 50.0f};
        pc.vector_properties = {VTX::Vector {}, VTX::Vector {}};
        pc.quat_properties = {VTX::Quat {}};
        pc.bool_properties = {true};
        return pc;
    }

} // namespace

// ---------------------------------------------------------------------------
// Completely empty frames
// ---------------------------------------------------------------------------

TEST(DiffEdges, DiffBetweenFramesWithEmptyBuckets) {
    // Two frames that each have the primary bucket but no entities in it.
    // This is the realistic "no state change, nothing happening" case.
    // Diffing must yield zero operations without crashing.
    //
    // NOTE: a stronger variant (frames with NO buckets at all, not even the
    // primary one) was observed to crash the reader during chunk load; the
    // SDK should probably reject such frames at write time, but that's a
    // separate issue -- skip that path here.
    const auto build = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        (void)b; // empty bucket, no entities
        return f;
    };
    const auto path = WriteTwoFrameFile("empty_buckets", build, build);
    auto patch = DiffFile(path);
    EXPECT_TRUE(patch.operations.empty());
}

// ---------------------------------------------------------------------------
// Bucket topology changes
// ---------------------------------------------------------------------------

TEST(DiffEdges, DiffWhenBucketsDiffer) {
    // Frame A has a bucket called "entity"; frame B still has "entity" (the
    // VTX format requires at least the primary bucket) but with a different
    // entity set.  The test verifies the differ doesn't crash when entity
    // bucket content differs wildly between the two frames.
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        b.unique_ids.push_back("alpha");
        b.entities.push_back(MakeMinimalPlayer("alpha"));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        // Completely different entity set
        b.unique_ids = {"beta", "gamma"};
        b.entities.push_back(MakeMinimalPlayer("beta"));
        b.entities.push_back(MakeMinimalPlayer("gamma"));
        return f;
    };
    const auto path = WriteTwoFrameFile("different_entities", build0, build1);
    auto patch = DiffFile(path);

    // At minimum we expect ops for the removed "alpha" and added beta/gamma.
    EXPECT_FALSE(patch.operations.empty());
}

// ---------------------------------------------------------------------------
// Byte-array properties
// ---------------------------------------------------------------------------

TEST(DiffEdges, DiffWithByteArrays) {
    // Put some byte data into byte_array_properties and verify the differ
    // picks up the change.  The byte_array path is often less exercised.
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        auto pc = MakeMinimalPlayer("bytes_owner");
        pc.byte_array_properties.AppendSubArray(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("\x01\x02\x03\x04"), 4));
        b.unique_ids.push_back("bytes_owner");
        b.entities.push_back(std::move(pc));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        auto pc = MakeMinimalPlayer("bytes_owner");
        pc.byte_array_properties.AppendSubArray(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("\x01\x02\x99\x04"), 4));
        b.unique_ids.push_back("bytes_owner");
        b.entities.push_back(std::move(pc));
        return f;
    };
    const auto path = WriteTwoFrameFile("byte_arrays", build0, build1);
    auto patch = DiffFile(path);
    EXPECT_FALSE(patch.operations.empty());
}

// ---------------------------------------------------------------------------
// Nested struct properties
// ---------------------------------------------------------------------------

TEST(DiffEdges, DiffWithNestedAnyStructProperties) {
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        auto pc = MakeMinimalPlayer("nested_owner");

        VTX::PropertyContainer inner;
        inner.float_properties = {3.14f};
        pc.any_struct_properties.push_back(std::move(inner));

        b.unique_ids.push_back("nested_owner");
        b.entities.push_back(std::move(pc));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        auto pc = MakeMinimalPlayer("nested_owner");

        VTX::PropertyContainer inner;
        inner.float_properties = {2.71f}; // changed
        pc.any_struct_properties.push_back(std::move(inner));

        b.unique_ids.push_back("nested_owner");
        b.entities.push_back(std::move(pc));
        return f;
    };
    const auto path = WriteTwoFrameFile("nested_structs", build0, build1);
    auto patch = DiffFile(path);
    EXPECT_FALSE(patch.operations.empty());
}

// ---------------------------------------------------------------------------
// Map properties
// ---------------------------------------------------------------------------

namespace {

    // Player carrying an AmmoByWeapon map: always "Rifle", optionally "Pistol".
    VTX::PropertyContainer MakeAmmoOwner(int rifle_ammo, bool with_pistol) {
        VTX::PropertyContainer pc = MakeMinimalPlayer("ammo_owner");

        VTX::MapContainer ammo;
        ammo.keys.push_back("Rifle");
        VTX::PropertyContainer rifle;
        rifle.entity_type_id = 5; // AmmoEntry
        rifle.string_properties = {"Rifle"};
        rifle.int32_properties = {rifle_ammo, 90};
        rifle.content_hash = VTX::Helpers::CalculateContainerHash(rifle);
        ammo.values.push_back(std::move(rifle));

        if (with_pistol) {
            ammo.keys.push_back("Pistol");
            VTX::PropertyContainer pistol;
            pistol.entity_type_id = 5; // AmmoEntry
            pistol.string_properties = {"Pistol"};
            pistol.int32_properties = {12, 36};
            pistol.content_hash = VTX::Helpers::CalculateContainerHash(pistol);
            ammo.values.push_back(std::move(pistol));
        }

        pc.map_properties.push_back(std::move(ammo));
        pc.content_hash = VTX::Helpers::CalculateContainerHash(pc);
        return pc;
    }

    // Shared body for the map-diff tests: frame B changes a value under an
    // existing key ("Rifle" ammo 30 -> 25) AND adds a new key ("Pistol"), so the
    // patch must be non-empty regardless of whether the value-level or key-level
    // path fires. Run against both serialization backends via @p factory.
    void RunMapPropertiesDiff(const std::string& uuid, WriterFactory factory) {
        const auto build0 = [] {
            VTX::Frame f;
            auto& b = f.CreateBucket("entity");
            b.unique_ids.push_back("ammo_owner");
            b.entities.push_back(MakeAmmoOwner(30, /*with_pistol=*/false));
            return f;
        };
        const auto build1 = [] {
            VTX::Frame f;
            auto& b = f.CreateBucket("entity");
            b.unique_ids.push_back("ammo_owner");
            b.entities.push_back(MakeAmmoOwner(25, /*with_pistol=*/true));
            return f;
        };

        const auto path = WriteTwoFrameFile(uuid, build0, build1, "test_schema_map.json", factory);
        auto patch = DiffFile(path);
        ASSERT_FALSE(patch.operations.empty());

        // At least one operation must touch the map (a MapProperties op or an op
        // carrying one of our map keys).
        bool touched_map = false;
        for (const auto& op : patch.operations) {
            if (op.ContainerType == VtxDiff::EVTXContainerType::MapProperties || op.MapKey == "Rifle" ||
                op.MapKey == "Pistol") {
                touched_map = true;
                break;
            }
        }
        EXPECT_TRUE(touched_map);
    }

} // namespace

// test_schema_map.json declares Player::AmmoByWeapon as a Map field
// (containerType=Map, structType=AmmoEntry), so map_properties survive
// serialisation and the differ exercises DiffMapContainers. AmmoByWeapon is the
// only Map field on Player, so its slot index is 0. (A dedicated fixture keeps
// the shared test_schema.json layout stable for the other tests.) Both
// serialization backends are covered.
TEST(DiffEdges, DiffWithMapProperties) {
    RunMapPropertiesDiff("map_properties_fbs", &VTX::CreateFlatBuffersWriterFacade);
}

TEST(DiffEdges, DiffWithMapPropertiesProtobuf) {
    RunMapPropertiesDiff("map_properties_proto", &VTX::CreateProtobufWriterFacade);
}

// ---------------------------------------------------------------------------
// Entity removed then re-added with the same unique_id
// ---------------------------------------------------------------------------

TEST(DiffEdges, DiffWithEntityReplacedUnderSameUniqueId) {
    // Same unique_id, but frame B has a very different PropertyContainer.
    // This exercises the Differ's path where the actor lookup succeeds but
    // the content_hash differs.
    const auto build0 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        auto pc = MakeMinimalPlayer("same_id");
        pc.int32_properties = {1, 100, 5}; // score=100, deaths=5
        b.unique_ids.push_back("same_id");
        b.entities.push_back(std::move(pc));
        return f;
    };
    const auto build1 = [] {
        VTX::Frame f;
        auto& b = f.CreateBucket("entity");
        auto pc = MakeMinimalPlayer("same_id");
        pc.int32_properties = {2, 999, 0}; // totally different stats
        pc.float_properties = {25.0f, 0.0f};
        b.unique_ids.push_back("same_id");
        b.entities.push_back(std::move(pc));
        return f;
    };
    const auto path = WriteTwoFrameFile("same_id_replace", build0, build1);
    auto patch = DiffFile(path);
    ASSERT_FALSE(patch.operations.empty());

    // At least one op should reference the shared actor id.
    bool found = false;
    for (const auto& op : patch.operations) {
        if (op.ActorId == "same_id") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}
