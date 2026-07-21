// End-to-end round-trip tests: writer -> reader -> verify.
//
// The key correctness property of the SDK: what you write is what you read.
// Runs as a TEST_P parametrized over VTX::VtxFormat so every scenario covers
// both the FlatBuffers and Protobuf backends without copy-pasted bodies.

#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <string>

#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/writer/policies/sinks/file_sink.h"

#include "util/test_fixtures.h"

namespace {

    constexpr int kTotalFrames = 120;   // enough to force multiple chunks
    constexpr int kChunkMaxFrames = 40; // = 3 chunks
    constexpr float kFps = 60.0f;

    const char* FormatName(VTX::VtxFormat f) {
        return f == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf";
    }

    VTX::Frame BuildFrame(int frame_index) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {"player_0", "Alpha"};
        pc.int32_properties = {1, frame_index, 0}; // Team, Score(=frame), Deaths
        pc.float_properties = {100.0f - float(frame_index), 50.0f};
        pc.vector_properties = {VTX::Vector {double(frame_index), 0.0, 0.0}, VTX::Vector {1.0, 0.0, 0.0}};
        pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
        pc.bool_properties = {true};

        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

} // namespace


// ===========================================================================
//  Parametrized fixture -- one body, runs against both backends.
// ===========================================================================

class RoundtripTest : public ::testing::TestWithParam<VTX::VtxFormat> {
protected:
    VTX::WriterFacadeConfig MakeConfig(const std::string& suffix, const std::string& uuid) const {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath =
            VtxTest::OutputPath(std::string("roundtrip_") + FormatName(GetParam()) + "_" + suffix + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "RoundtripTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = kFps;
        cfg.chunk_max_frames = kChunkMaxFrames;
        cfg.use_compression = true;
        return cfg;
    }

    std::unique_ptr<VTX::IVtxWriterFacade> CreateWriter(const VTX::WriterFacadeConfig& cfg) const {
        return GetParam() == VTX::VtxFormat::FlatBuffers ? VTX::CreateFlatBuffersWriterFacade(cfg)
                                                         : VTX::CreateProtobufWriterFacade(cfg);
    }
};

// ---------------------------------------------------------------------------
// Every recorded value must round-trip identically back out of the reader.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, PreservesFrameData) {
    auto cfg = MakeConfig("preserves", "uuid-rt-preserves");
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < kTotalFrames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.format, GetParam());

    auto* reader = ctx.reader.get();
    EXPECT_EQ(reader->GetTotalFrames(), kTotalFrames);

    // Seek table reflects the chunking.
    const auto& seek_table = reader->GetSeekTable();
    EXPECT_GE(seek_table.size(), size_t(kTotalFrames / kChunkMaxFrames));

    // Header + footer fidelity.
    auto header = reader->GetHeader();
    EXPECT_EQ(header.replay_name, "RoundtripTest");

    auto footer = reader->GetFooter();
    EXPECT_EQ(footer.total_frames, kTotalFrames);

    // Spot-check boundary and middle frames.
    for (int frame_index : {0, kTotalFrames / 2, kTotalFrames - 1}) {
        const VTX::Frame* f = reader->GetFrameSync(frame_index);
        ASSERT_NE(f, nullptr) << "frame " << frame_index;
        ASSERT_EQ(f->GetBuckets().size(), 1u);
        const auto& entities = f->GetBuckets()[0].entities;
        ASSERT_EQ(entities.size(), 1u);

        const auto& e = entities[0];
        ASSERT_GE(e.int32_properties.size(), 2u);
        EXPECT_EQ(e.int32_properties[1], frame_index);

        ASSERT_GE(e.float_properties.size(), 1u);
        EXPECT_FLOAT_EQ(e.float_properties[0], 100.0f - float(frame_index));

        ASSERT_GE(e.vector_properties.size(), 1u);
        EXPECT_DOUBLE_EQ(e.vector_properties[0].x, double(frame_index));
    }
}

// ---------------------------------------------------------------------------
// Writer must accept historical (pre-"now") UTC timestamps -- regression for
// the VTXGameTimes fix of 2026-04-17.  Same contract on both backends.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, AcceptsHistoricalUtc) {
    auto cfg = MakeConfig("historical_utc", "uuid-rt-hist");
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);

        const int64_t base_utc = 1'745'000'000LL * 10'000'000LL; // 2025-04-19
        for (int i = 0; i < 20; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            t.created_utc_time = base_utc + int64_t(i) * 166'666LL;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 20);
}

// ---------------------------------------------------------------------------
// Multi-bucket frames survive the round-trip on both backends, and bucket
// names come back from the schema's "buckets" array (they are not stored on
// the wire). Regression: the FlatBuffers writer used to hardcode two bucket
// slots ("data"/"bone_data") and silently drop buckets at index >= 2.
// ---------------------------------------------------------------------------

namespace {

    constexpr const char* kMultiBucketSchema = R"({
        "version": "1.0.0",
        "buckets": ["entity", "bone_data", "economy"],
        "property_mapping": [
            {
                "struct": "Tiny",
                "values": [
                    {
                        "name": "Score",
                        "structType": "",
                        "typeId": "Int32",
                        "keyId": "None",
                        "containerType": "None",
                        "meta": { "type": "int32", "keyType": "", "category": "Tiny",
                                  "displayName": "Score", "tooltip": "",
                                  "defaultValue": "0", "version": 1, "fixedArrayDim": 1 }
                    }
                ]
            }
        ]
    })";

    // Buckets are deliberately created in a different order than the schema
    // declares them: the writer must normalize the layout to schema order.
    VTX::Frame BuildMultiBucketFrame(int frame_index) {
        VTX::Frame f;
        int bucket_ordinal = 0;
        for (const char* name : {"bone_data", "entity", "economy"}) {
            auto& bucket = f.CreateBucket(name);

            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.int32_properties = {frame_index, bucket_ordinal};

            bucket.unique_ids.push_back(std::string(name) + "_0");
            bucket.entities.push_back(std::move(pc));
            ++bucket_ordinal;
        }
        return f;
    }

} // namespace

TEST_P(RoundtripTest, MultiBucketFramesSurviveRoundtrip) {
    auto cfg = MakeConfig("multibucket", "uuid-rt-multibucket");
    cfg.schema_json_path.clear();
    cfg.schema_json_content = kMultiBucketSchema;
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < 10; ++i) {
            auto frame = BuildMultiBucketFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            writer->RecordFrame(frame, t);
        }
        writer->Flush();
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    ASSERT_EQ(ctx.reader->GetTotalFrames(), 10);

    for (int frame_index : {0, 9}) {
        const VTX::Frame* f = ctx.reader->GetFrameSync(frame_index);
        ASSERT_NE(f, nullptr) << "frame " << frame_index;

        // All three buckets survive (no silent dropping), in schema order.
        ASSERT_EQ(f->GetBuckets().size(), 3u);
        ASSERT_EQ(f->bucket_map.size(), 3u);
        ASSERT_EQ(f->bucket_map.at("entity"), 0u);
        ASSERT_EQ(f->bucket_map.at("bone_data"), 1u);
        ASSERT_EQ(f->bucket_map.at("economy"), 2u);

        // Content ends up under the schema-declared position regardless of the
        // creation order inside the frame ("bone_data" was created first).
        // int32[1] carries the creation ordinal: entity=1, bone_data=0, economy=2.
        const auto& buckets = f->GetBuckets();
        ASSERT_EQ(buckets[0].entities.size(), 1u);
        EXPECT_EQ(buckets[0].unique_ids[0], "entity_0");
        EXPECT_EQ(buckets[0].entities[0].int32_properties[0], frame_index);
        EXPECT_EQ(buckets[0].entities[0].int32_properties[1], 1);

        ASSERT_EQ(buckets[1].entities.size(), 1u);
        EXPECT_EQ(buckets[1].unique_ids[0], "bone_data_0");
        EXPECT_EQ(buckets[1].entities[0].int32_properties[1], 0);

        ASSERT_EQ(buckets[2].entities.size(), 1u);
        EXPECT_EQ(buckets[2].unique_ids[0], "economy_0");
        EXPECT_EQ(buckets[2].entities[0].int32_properties[1], 2);

        // type_ranges must be rebuilt for EVERY bucket, not just index 0, so
        // typed access works on non-first buckets too (regression: SortBucketByTypeId
        // used to run only for bucket 0, leaving others with empty type_ranges).
        EXPECT_EQ(buckets[0].GetEntitiesOfType(0).size(), 1u);
        EXPECT_EQ(buckets[1].GetEntitiesOfType(0).size(), 1u);
        EXPECT_EQ(buckets[2].GetEntitiesOfType(0).size(), 1u);
    }
}

// ---------------------------------------------------------------------------
// A frame built positionally (raw GetMutableBuckets().push_back, empty
// bucket_map) is adopted into the schema layout instead of rejected.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, AdoptsPositionallyBuiltFrameToSchemaLayout) {
    auto cfg = MakeConfig("positional", "uuid-rt-positional");
    cfg.schema_json_path.clear();
    cfg.schema_json_content = kMultiBucketSchema; // declares ["entity","bone_data","economy"]
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < 4; ++i) {
            VTX::Frame f;
            // Build positionally: no CreateBucket, so bucket_map stays empty.
            auto& raw = f.GetMutableBuckets();
            raw.emplace_back(); // will map to schema bucket 0 ("entity")
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.int32_properties = {i};
            raw[0].unique_ids.push_back("e_0");
            raw[0].entities.push_back(std::move(pc));

            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            const auto r = writer->TryRecordFrame(f, t);
            EXPECT_TRUE(r.IsWritten()) << "positional frame " << i << " rejected: " << r.error.message;
        }
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    ASSERT_EQ(ctx.reader->GetTotalFrames(), 4);

    const VTX::Frame* f = ctx.reader->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    // The single positional bucket was adopted as schema bucket 0 and the two
    // remaining declared buckets were materialized empty.
    ASSERT_EQ(f->GetBuckets().size(), 3u);
    EXPECT_EQ(f->bucket_map.at("entity"), 0u);
    ASSERT_EQ(f->GetBuckets()[0].entities.size(), 1u);
    EXPECT_EQ(f->GetBuckets()[0].unique_ids[0], "e_0");
    EXPECT_TRUE(f->GetBuckets()[1].entities.empty());
    EXPECT_TRUE(f->GetBuckets()[2].entities.empty());
}

// ---------------------------------------------------------------------------
// A schema without a "buckets" array (legacy) may record zero-bucket frames
// (idle/gap ticks). They must round-trip without crashing on load.
// Regression: the reader policies indexed bucket 0 unconditionally.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, ZeroBucketFramesFromSchemalessWriterRoundtrip) {
    auto cfg = MakeConfig("zero_bucket", "uuid-rt-zerobucket");
    cfg.schema_json_path.clear();
    cfg.schema_json_content =
        R"({"version":"1.0.0","property_mapping":[{"struct":"S","values":[)"
        R"({"name":"X","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}})"
        R"(]}]})";
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < 3; ++i) {
            VTX::Frame frame; // zero buckets
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;
    ASSERT_EQ(ctx.reader->GetTotalFrames(), 3);

    const VTX::Frame* f = ctx.reader->GetFrameSync(0); // must not crash
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->GetBuckets().empty());
}

// ---------------------------------------------------------------------------
// Bucket names restored on read for the single-bucket fixture schema too.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, RestoresBucketNamesFromSchemaOnRead) {
    auto cfg = MakeConfig("bucket_names", "uuid-rt-bucketnames");
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < 5; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;

    const VTX::Frame* f = ctx.reader->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->bucket_map.size(), 1u); // test_schema.json declares ["entity"]
    EXPECT_EQ(f->bucket_map.at("entity"), 0u);

    // By-name const lookup works on read frames now.
    const VTX::Frame& frame_ref = *f;
    EXPECT_EQ(frame_ref.GetBucket("entity").entities.size(), 1u);
}

// ---------------------------------------------------------------------------
// Populated array and map field VALUES survive write -> read on both backends.
// (The existing scalar roundtrip does not cover FlatArray / map serialization.)
// Note: pre-sizing creates EMPTY declared slots in memory, but an array with no
// data is intentionally not serialized, so this covers the case that matters on
// disk -- populated arrays/maps.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, ArrayAndMapFieldValuesRoundtrip) {
    auto cfg = MakeConfig("arraymap", "uuid-rt-arraymap");
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);

        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer e;
        e.entity_type_id = 0;
        e.float_arrays.AppendSubArray({1.5f, 2.5f, 3.5f});
        e.string_arrays.AppendSubArray({std::string("alpha"), std::string("beta")});

        VTX::MapContainer m;
        m.keys = {"rifle", "pistol"};
        VTX::PropertyContainer v0;
        v0.entity_type_id = 0;
        v0.int32_properties = {30};
        VTX::PropertyContainer v1;
        v1.entity_type_id = 0;
        v1.int32_properties = {12};
        m.values.push_back(std::move(v0));
        m.values.push_back(std::move(v1));
        e.map_properties.push_back(std::move(m));

        bucket.unique_ids.push_back("e0");
        bucket.entities.push_back(std::move(e));

        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        writer->RecordFrame(f, t);
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;

    const VTX::Frame* f = ctx.reader->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->GetBuckets().size(), 1u);
    ASSERT_EQ(f->GetBuckets()[0].entities.size(), 1u);
    const auto& e = f->GetBuckets()[0].entities[0];

    // Float array survives with its values, in order.
    ASSERT_EQ(e.float_arrays.SubArrayCount(), 1u);
    const auto fa = e.float_arrays.GetSubArray(0);
    ASSERT_EQ(fa.size(), 3u);
    EXPECT_FLOAT_EQ(fa[0], 1.5f);
    EXPECT_FLOAT_EQ(fa[1], 2.5f);
    EXPECT_FLOAT_EQ(fa[2], 3.5f);

    // String array survives.
    ASSERT_EQ(e.string_arrays.SubArrayCount(), 1u);
    const auto sa = e.string_arrays.GetSubArray(0);
    ASSERT_EQ(sa.size(), 2u);
    EXPECT_EQ(sa[0], "alpha");
    EXPECT_EQ(sa[1], "beta");

    // Map survives with keys and per-key values.
    ASSERT_EQ(e.map_properties.size(), 1u);
    ASSERT_EQ(e.map_properties[0].keys.size(), 2u);
    EXPECT_EQ(e.map_properties[0].keys[0], "rifle");
    EXPECT_EQ(e.map_properties[0].keys[1], "pistol");
    ASSERT_EQ(e.map_properties[0].values.size(), 2u);
    ASSERT_FALSE(e.map_properties[0].values[0].int32_properties.empty());
    EXPECT_EQ(e.map_properties[0].values[0].int32_properties[0], 30);
    EXPECT_EQ(e.map_properties[0].values[1].int32_properties[0], 12);
}

// ---------------------------------------------------------------------------
// Declared-but-empty array fields are restored on read. An array with no data
// is not serialized, so a reader must re-create the declared empty subarrays
// from the schema for the read frame to mirror an ingest-loaded frame's layout.
// (Maps already round-trip their slot count, so this covers only arrays.)
// ---------------------------------------------------------------------------

namespace {
    // Hero declares three array-typed fields (one float array, two string arrays,
    // one struct array); an entity that leaves them empty exercises read-side
    // restoration.
    constexpr const char* kHeroArraySchema = R"({
        "version": "1.0.0",
        "buckets": ["entity"],
        "property_mapping": [
            { "struct": "Item", "values": [
                {"name":"Id","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}}
            ]},
            { "struct": "Hero", "values": [
                {"name":"UniqueID","typeId":"String","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":1}},
                {"name":"Score","typeId":"Int32","containerType":"None","keyId":"None","structType":"","meta":{"defaultValue":"0","fixedArrayDim":1}},
                {"name":"Cooldowns","typeId":"Float","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":0}},
                {"name":"Tags","typeId":"String","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":0}},
                {"name":"Names","typeId":"String","containerType":"Array","keyId":"None","structType":"","meta":{"defaultValue":"","fixedArrayDim":0}},
                {"name":"Inventory","typeId":"Struct","containerType":"Array","keyId":"None","structType":"Item","meta":{"defaultValue":"","fixedArrayDim":0}}
            ]}
        ]
    })";
} // namespace

TEST_P(RoundtripTest, ReaderRestoresDeclaredEmptyArrays) {
    VTX::SchemaRegistry reg;
    ASSERT_TRUE(reg.LoadFromRawString(kHeroArraySchema));
    const int32_t heroTypeId = reg.GetStructTypeId("Hero");
    ASSERT_GE(heroTypeId, 0);

    auto cfg = MakeConfig("declared_empty_arrays", "uuid-rt-declared-empty");
    cfg.schema_json_path.clear();
    cfg.schema_json_content = kHeroArraySchema;
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);

        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        VTX::PropertyContainer e;
        e.entity_type_id = heroTypeId;
        e.string_properties = {"hero_1"}; // only a scalar; all array fields left empty
        bucket.unique_ids.push_back("hero_1");
        bucket.entities.push_back(std::move(e));

        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        writer->RecordFrame(f, t);
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;

    const VTX::Frame* f = ctx.reader->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->GetBuckets()[0].entities.size(), 1u);
    const auto& e = f->GetBuckets()[0].entities[0];

    // The declared array fields come back present-and-empty even though nothing
    // was serialized for them (without read-side restoration these would be 0).
    EXPECT_EQ(e.float_arrays.SubArrayCount(), 1u);      // Cooldowns
    EXPECT_EQ(e.string_arrays.SubArrayCount(), 2u);     // Tags, Names
    EXPECT_EQ(e.any_struct_arrays.SubArrayCount(), 1u); // Inventory
    EXPECT_TRUE(e.float_arrays.GetSubArray(0).empty());
    EXPECT_TRUE(e.string_arrays.GetSubArray(0).empty());

    // A type with no declared array field is untouched.
    EXPECT_EQ(e.int32_arrays.SubArrayCount(), 0u);
}

// ---------------------------------------------------------------------------
// Struct-array fields (Vector/Quat/Transform/FloatRange) round-trip with their
// subarray partitioning intact. Regression: the FlatBuffers reader's
// unpackStructArray lambda restored `data` but silently dropped `offsets`,
// so any struct-array with >=2 subarrays lost its boundaries on load.
// Uses three subarrays with mixed sizes (including an empty one) so a fully
// broken offsets restore, or one that only preserves the terminal offset,
// both fail visibly.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, StructArraySubArrayOffsetsRoundtrip) {
    auto cfg = MakeConfig("struct_array_offsets", "uuid-rt-struct-offsets");
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);

        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer e;
        e.entity_type_id = 0;

        // 3 subarrays; middle one is deliberately empty.
        e.vector_arrays.AppendSubArray({VTX::Vector {1.0, 2.0, 3.0}, VTX::Vector {4.0, 5.0, 6.0}});
        e.vector_arrays.AppendSubArray({});
        e.vector_arrays.AppendSubArray(
            {VTX::Vector {7.0, 8.0, 9.0}, VTX::Vector {10.0, 11.0, 12.0}, VTX::Vector {13.0, 14.0, 15.0}});

        e.quat_arrays.AppendSubArray({VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}});
        e.quat_arrays.AppendSubArray({VTX::Quat {1.0f, 0.0f, 0.0f, 0.0f}, VTX::Quat {0.0f, 1.0f, 0.0f, 0.0f}});
        e.quat_arrays.AppendSubArray({});

        e.transform_arrays.AppendSubArray({});
        e.transform_arrays.AppendSubArray(
            {VTX::Transform {{1.0, 2.0, 3.0}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0, 1.0, 1.0}}});
        e.transform_arrays.AppendSubArray(
            {VTX::Transform {{4.0, 5.0, 6.0}, {0.0f, 0.0f, 0.0f, 1.0f}, {2.0, 2.0, 2.0}},
             VTX::Transform {{7.0, 8.0, 9.0}, {1.0f, 0.0f, 0.0f, 0.0f}, {3.0, 3.0, 3.0}}});

        e.range_arrays.AppendSubArray({VTX::FloatRange {0.0f, 100.0f, 0.5f}});
        e.range_arrays.AppendSubArray({VTX::FloatRange {-1.0f, 1.0f, 0.0f}, VTX::FloatRange {10.0f, 20.0f, 0.75f}});
        e.range_arrays.AppendSubArray({VTX::FloatRange {50.0f, 60.0f, 0.25f}});

        // Snapshot expected offsets/totals before move.
        const std::vector<uint32_t> expected_vec_offsets = e.vector_arrays.offsets;
        const std::vector<uint32_t> expected_quat_offsets = e.quat_arrays.offsets;
        const std::vector<uint32_t> expected_trans_offsets = e.transform_arrays.offsets;
        const std::vector<uint32_t> expected_range_offsets = e.range_arrays.offsets;

        ASSERT_EQ(expected_vec_offsets, (std::vector<uint32_t> {0u, 2u, 2u}));
        ASSERT_EQ(expected_quat_offsets, (std::vector<uint32_t> {0u, 1u, 3u}));
        ASSERT_EQ(expected_trans_offsets, (std::vector<uint32_t> {0u, 0u, 1u}));
        ASSERT_EQ(expected_range_offsets, (std::vector<uint32_t> {0u, 1u, 3u}));

        bucket.unique_ids.push_back("e0");
        bucket.entities.push_back(std::move(e));

        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        writer->RecordFrame(f, t);
        writer->Stop();
    }

    auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
    ASSERT_TRUE(ctx) << ctx.error;

    const VTX::Frame* f = ctx.reader->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->GetBuckets().size(), 1u);
    ASSERT_EQ(f->GetBuckets()[0].entities.size(), 1u);
    const auto& e = f->GetBuckets()[0].entities[0];

    // --- Vector array: 3 subarrays, offsets [0, 2, 2] ---
    ASSERT_EQ(e.vector_arrays.SubArrayCount(), 3u);
    EXPECT_EQ(e.vector_arrays.offsets, (std::vector<uint32_t> {0u, 2u, 2u}));
    ASSERT_EQ(e.vector_arrays.TotalElementCount(), 5u);
    {
        auto s0 = e.vector_arrays.GetSubArray(0);
        ASSERT_EQ(s0.size(), 2u);
        EXPECT_DOUBLE_EQ(s0[0].x, 1.0);
        EXPECT_DOUBLE_EQ(s0[1].z, 6.0);
        EXPECT_TRUE(e.vector_arrays.GetSubArray(1).empty());
        auto s2 = e.vector_arrays.GetSubArray(2);
        ASSERT_EQ(s2.size(), 3u);
        EXPECT_DOUBLE_EQ(s2[0].x, 7.0);
        EXPECT_DOUBLE_EQ(s2[2].z, 15.0);
    }

    // --- Quat array: 3 subarrays, offsets [0, 1, 3] ---
    ASSERT_EQ(e.quat_arrays.SubArrayCount(), 3u);
    EXPECT_EQ(e.quat_arrays.offsets, (std::vector<uint32_t> {0u, 1u, 3u}));
    ASSERT_EQ(e.quat_arrays.TotalElementCount(), 3u);
    {
        auto s0 = e.quat_arrays.GetSubArray(0);
        ASSERT_EQ(s0.size(), 1u);
        EXPECT_FLOAT_EQ(s0[0].w, 1.0f);
        auto s1 = e.quat_arrays.GetSubArray(1);
        ASSERT_EQ(s1.size(), 2u);
        EXPECT_FLOAT_EQ(s1[0].x, 1.0f);
        EXPECT_FLOAT_EQ(s1[1].y, 1.0f);
        EXPECT_TRUE(e.quat_arrays.GetSubArray(2).empty());
    }

    // --- Transform array: 3 subarrays, offsets [0, 0, 1] ---
    ASSERT_EQ(e.transform_arrays.SubArrayCount(), 3u);
    EXPECT_EQ(e.transform_arrays.offsets, (std::vector<uint32_t> {0u, 0u, 1u}));
    ASSERT_EQ(e.transform_arrays.TotalElementCount(), 3u);
    {
        EXPECT_TRUE(e.transform_arrays.GetSubArray(0).empty());
        auto s1 = e.transform_arrays.GetSubArray(1);
        ASSERT_EQ(s1.size(), 1u);
        EXPECT_DOUBLE_EQ(s1[0].translation.x, 1.0);
        auto s2 = e.transform_arrays.GetSubArray(2);
        ASSERT_EQ(s2.size(), 2u);
        EXPECT_DOUBLE_EQ(s2[0].translation.x, 4.0);
        EXPECT_DOUBLE_EQ(s2[1].scale.z, 3.0);
    }

    // --- Range array: 3 subarrays, offsets [0, 1, 3] ---
    ASSERT_EQ(e.range_arrays.SubArrayCount(), 3u);
    EXPECT_EQ(e.range_arrays.offsets, (std::vector<uint32_t> {0u, 1u, 3u}));
    ASSERT_EQ(e.range_arrays.TotalElementCount(), 4u);
    {
        auto s0 = e.range_arrays.GetSubArray(0);
        ASSERT_EQ(s0.size(), 1u);
        EXPECT_FLOAT_EQ(s0[0].max, 100.0f);
        auto s1 = e.range_arrays.GetSubArray(1);
        ASSERT_EQ(s1.size(), 2u);
        EXPECT_FLOAT_EQ(s1[1].value_normalized, 0.75f);
        auto s2 = e.range_arrays.GetSubArray(2);
        ASSERT_EQ(s2.size(), 1u);
        EXPECT_FLOAT_EQ(s2[0].min, 50.0f);
    }
}

// ---------------------------------------------------------------------------
// PRISM perf observer plumbing: attaching a FileSinkAtomicPerfObserver via
// WriterFacadeConfig must produce non-zero timings on the three sink stages
// (serialize / compress / disk write) after a real write session on both
// backends. Regression guard for the facade wiring, since a missing
// `internal_cfg.sink_config.perf_observer = config.perf_observer` line would
// silently leave the observer as null.
// ---------------------------------------------------------------------------

TEST_P(RoundtripTest, PerfObserverReceivesSinkTimings) {
    VTX::FileSinkAtomicPerfObserver perf;

    auto cfg = MakeConfig("perf_observer", "uuid-rt-perf-observer");
    cfg.perf_observer = &perf;
    cfg.use_compression = true; // ensure the compression stage runs
    {
        auto writer = CreateWriter(cfg);
        ASSERT_TRUE(writer);
        for (int i = 0; i < 30; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }

    const auto stats = perf.Snapshot();
    EXPECT_GT(stats.serialization_us, 0u) << "SerializeChunk/Header/Footer never notified";
    EXPECT_GT(stats.disk_write_us, 0u) << "TimedWrite never notified";
    EXPECT_GT(stats.compression_us, 0u) << "ZSTD_compress never notified";
}

// ---------------------------------------------------------------------------
// Backend instantiation -- produces:
//   BothBackends/RoundtripTest.PreservesFrameData/FlatBuffers
//   BothBackends/RoundtripTest.PreservesFrameData/Protobuf
//   BothBackends/RoundtripTest.AcceptsHistoricalUtc/FlatBuffers
//   BothBackends/RoundtripTest.AcceptsHistoricalUtc/Protobuf
// ---------------------------------------------------------------------------

INSTANTIATE_TEST_SUITE_P(BothBackends, RoundtripTest,
                         ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
                         [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) {
                             return std::string(FormatName(info.param));
                         });
