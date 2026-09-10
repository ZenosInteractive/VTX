// Replay entity filter tests: FilterReplayFile() writes a copy of a replay with
// entities dropped (blacklist) or exclusively kept (whitelist) by unique id glob,
// struct name or property value. Every frame survives; the header is byte-identical;
// the footer keeps the frame count, duration and per-frame time table; the seek
// table is rebuilt and its checksums match the bytes on disk; timeline events are
// carried over and pruned to the entities that still exist.
//
// Runs as a TEST_P over both backends (FlatBuffers + Protobuf).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "vtx_schema_generated.h" // complete fbsvtx/cppvtx types before the policy headers
#include "vtx_schema.pb.h"

#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/transform/vtx_replay_filter.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"

#include "util/replay_snapshot.h"
#include "util/test_fixtures.h"

namespace {

    // Read-back helpers shared with the cut / framing tests.
    using Snapshot = VtxTest::ReplaySnapshot;
    using VtxTest::ExpectEntityEqual;
    using VtxTest::ExpectSeekTableMatchesFile;
    using VtxTest::ReadReplay;

    constexpr int kFrames = 100; // 3 chunks: [0,39] [40,79] [80,99]
    constexpr int kChunkFrames = 40;
    constexpr float kFps = 60.0f;
    constexpr int64_t kBaseUtc = 17'000'000'000'000'000LL; // unix-relative 100 ns ticks
    constexpr int64_t kFrameTicks = 166'666;

    std::string ReadFileText(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // The fixture schema with three buckets instead of one; the struct definitions
    // are unchanged so the entity builders below stay valid.
    std::string MultiBucketSchemaJson() {
        std::string json = ReadFileText(VtxTest::FixturePath("test_schema.json"));
        const std::string single = "\"buckets\": [\"entity\"]";
        const size_t at = json.find(single);
        if (at != std::string::npos) {
            json.replace(at, single.size(), "\"buckets\": [\"entity\", \"fx\", \"telemetry\"]");
        }
        return json;
    }

    const char* FormatName(VTX::VtxFormat f) {
        return f == VTX::VtxFormat::FlatBuffers ? "FlatBuffers" : "Protobuf";
    }

    // Fixture schema (tests/fixtures/test_schema.json), one bucket "entity":
    //   Player (0):     strings [UniqueID, Name]  int32 [Team, Score, Deaths]  float [Health, Armor]
    //                   vector [Position, Velocity]  quat [Rotation]  bool [IsAlive]
    //   Projectile (1): strings [UniqueID, OwnerID, Type]  vector [Position, Velocity]  float [Damage]
    //   MatchState (2): strings [UniqueID, Phase]  int32 [ScoreTeam1, ScoreTeam2, Round]  float [TimeRemaining]
    constexpr int32_t kPlayer = 0;
    constexpr int32_t kProjectile = 1;
    constexpr int32_t kMatchState = 2;

    VTX::PropertyContainer MakePlayer(const std::string& id, const std::string& name, int team, int frame) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = kPlayer;
        pc.string_properties = {id, name};
        pc.int32_properties = {team, frame, frame / 10};
        pc.float_properties = {100.0f - float(frame % 50), 25.0f + float(team)};
        pc.vector_properties = {VTX::Vector {double(frame), double(team), 0.0}, VTX::Vector {1.0, 0.0, 0.0}};
        pc.quat_properties = {VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f}};
        pc.bool_properties = {frame % 7 != 0};
        return pc;
    }

    VTX::PropertyContainer MakeProjectile(const std::string& id, const std::string& owner, const std::string& type,
                                          int frame) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = kProjectile;
        pc.string_properties = {id, owner, type};
        pc.vector_properties = {VTX::Vector {double(frame) * 2.0, 1.0, 2.0}, VTX::Vector {0.0, 5.0, 0.0}};
        pc.float_properties = {10.0f + float(frame % 3)};
        return pc;
    }

    VTX::PropertyContainer MakeMatch(int frame) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = kMatchState;
        pc.string_properties = {"match", frame < 50 ? "warmup" : "live"};
        pc.int32_properties = {frame / 20, frame / 30, 1 + frame / 50};
        pc.float_properties = {300.0f - float(frame)};
        return pc;
    }

    void Add(VTX::Bucket& bucket, const std::string& id, VTX::PropertyContainer pc) {
        bucket.unique_ids.push_back(id);
        bucket.entities.push_back(std::move(pc));
    }

    std::string ProjectileId(int frame) {
        return "proj_" + std::to_string(frame % 3);
    }

    // Two players, one projectile (id cycles proj_0..proj_2) and the match state.
    VTX::Frame BuildFrame(int frame) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        Add(bucket, "player_0", MakePlayer("player_0", "Alpha", 1, frame));
        Add(bucket, "player_1", MakePlayer("player_1", "Bravo", 2, frame));
        Add(bucket, ProjectileId(frame),
            MakeProjectile(ProjectileId(frame), frame % 2 ? "player_1" : "player_0", frame % 2 ? "rocket" : "bullet",
                           frame));
        Add(bucket, "match", MakeMatch(frame));
        return f;
    }

    // Same entities spread over three buckets: players in "entity" (0), the
    // projectile in "fx" (1), the match state in "telemetry" (2).
    VTX::Frame BuildMultiBucketFrame(int frame) {
        VTX::Frame f;
        auto& entity = f.CreateBucket("entity");
        Add(entity, "player_0", MakePlayer("player_0", "Alpha", 1, frame));
        Add(entity, "player_1", MakePlayer("player_1", "Bravo", 2, frame));
        auto& fx = f.CreateBucket("fx");
        Add(fx, ProjectileId(frame), MakeProjectile(ProjectileId(frame), "player_0", "bullet", frame));
        auto& telemetry = f.CreateBucket("telemetry");
        Add(telemetry, "match", MakeMatch(frame));
        return f;
    }

    // The output bucket must be exactly the source bucket minus the entities `dropped`
    // rejects, in the original order, with every survivor unchanged.
    template <typename DropPredicate>
    void ExpectBucketFiltered(const VTX::Bucket& source, const VTX::Bucket& output, DropPredicate dropped,
                              const std::string& what) {
        std::vector<size_t> expected;
        for (size_t i = 0; i < source.entities.size(); ++i) {
            if (!dropped(source, i)) {
                expected.push_back(i);
            }
        }
        ASSERT_EQ(output.entities.size(), expected.size()) << what;
        ASSERT_EQ(output.unique_ids.size(), expected.size()) << what;
        for (size_t o = 0; o < expected.size(); ++o) {
            const size_t s = expected[o];
            EXPECT_EQ(output.unique_ids[o], source.unique_ids[s]) << what << " entity " << o;
            ExpectEntityEqual(source.entities[s], output.entities[o], what + " entity " + source.unique_ids[s]);
        }
    }

    template <typename DropPredicate>
    void ExpectFramesFiltered(const Snapshot& source, const Snapshot& output, DropPredicate dropped) {
        ASSERT_EQ(output.frames.size(), source.frames.size());
        for (size_t f = 0; f < source.frames.size(); ++f) {
            const auto& sb = source.frames[f].GetBuckets();
            const auto& ob = output.frames[f].GetBuckets();
            ASSERT_EQ(ob.size(), sb.size()) << "frame " << f;
            for (size_t b = 0; b < sb.size(); ++b) {
                ExpectBucketFiltered(sb[b], ob[b], dropped,
                                     "frame " + std::to_string(f) + " bucket " + std::to_string(b));
            }
        }
    }

    // Like ExpectFramesFiltered, with the predicate also given the bucket index.
    template <typename DropPredicate>
    void ExpectFramesFilteredByBucket(const Snapshot& source, const Snapshot& output, DropPredicate dropped) {
        ASSERT_EQ(output.frames.size(), source.frames.size());
        for (size_t f = 0; f < source.frames.size(); ++f) {
            const auto& sb = source.frames[f].GetBuckets();
            const auto& ob = output.frames[f].GetBuckets();
            ASSERT_EQ(ob.size(), sb.size()) << "frame " << f;
            for (size_t b = 0; b < sb.size(); ++b) {
                ExpectBucketFiltered(
                    sb[b], ob[b], [&](const VTX::Bucket& bucket, size_t i) { return dropped(b, bucket, i); },
                    "frame " + std::to_string(f) + " bucket " + std::to_string(b));
            }
        }
    }

    // Header bit-identical fields, same frame count/duration/time table, same chunk
    // frame ranges.
    void ExpectSameHeaderFooterFraming(const Snapshot& source, const Snapshot& output) {
        EXPECT_EQ(output.format, source.format);
        EXPECT_EQ(output.header.replay_uuid, source.header.replay_uuid);
        EXPECT_EQ(output.header.replay_name, source.header.replay_name);
        EXPECT_EQ(output.header.recorded_utc_timestamp, source.header.recorded_utc_timestamp);
        EXPECT_EQ(output.header.custom_json_metadata, source.header.custom_json_metadata);
        EXPECT_EQ(output.header.version.format_major, source.header.version.format_major);
        EXPECT_EQ(output.header.version.format_minor, source.header.version.format_minor);
        EXPECT_EQ(output.header.version.schema_version, source.header.version.schema_version);
        EXPECT_EQ(output.schema.property_mapping, source.schema.property_mapping);
        EXPECT_EQ(output.schema.data_identifier, source.schema.data_identifier);

        EXPECT_EQ(output.footer.total_frames, source.footer.total_frames);
        EXPECT_FLOAT_EQ(output.footer.duration_seconds, source.footer.duration_seconds);
        EXPECT_EQ(output.footer.times.game_time, source.footer.times.game_time);
        EXPECT_EQ(output.footer.times.created_utc, source.footer.times.created_utc);
        EXPECT_EQ(output.footer.times.gaps, source.footer.times.gaps);
        EXPECT_EQ(output.footer.times.segments, source.footer.times.segments);
        EXPECT_FALSE(source.footer.times.created_utc.empty()) << "fixture should stamp created_utc";

        ASSERT_EQ(output.footer.chunk_index.size(), source.footer.chunk_index.size());
        for (size_t i = 0; i < source.footer.chunk_index.size(); ++i) {
            EXPECT_EQ(output.footer.chunk_index[i].chunk_index, source.footer.chunk_index[i].chunk_index) << i;
            EXPECT_EQ(output.footer.chunk_index[i].start_frame, source.footer.chunk_index[i].start_frame) << i;
            EXPECT_EQ(output.footer.chunk_index[i].end_frame, source.footer.chunk_index[i].end_frame) << i;
        }
    }

    VTX::TimelineEvent MakeEvent(float game_time, const std::string& type, const std::string& label,
                                 const std::string& entity_id) {
        VTX::TimelineEvent ev;
        ev.game_time = game_time;
        ev.event_type = type;
        ev.label = label;
        ev.location = VTX::Vector {1.0, 2.0, 3.0};
        ev.entity_unique_id = entity_id;
        return ev;
    }

    // The public writer never emits timeline events, so fabricate a source that has
    // them: replace the file's footer block with one serialized through the same
    // policy the sink uses, carrying the original seek table and time table plus the
    // given events.
    void RewriteFooterWithEvents(const std::string& path, VTX::VtxFormat format,
                                 const std::vector<VTX::TimelineEvent>& events) {
        const Snapshot s = ReadReplay(path);
        ASSERT_TRUE(s.ok);

        std::vector<VTX::ChunkIndexData> seek_table;
        for (const VTX::ChunkIndexEntry& e : s.footer.chunk_index) {
            VTX::ChunkIndexData d;
            d.chunk_index = e.chunk_index;
            d.file_offset = static_cast<int64_t>(e.file_offset);
            d.chunk_size_bytes = e.chunk_size_bytes;
            d.start_frame = e.start_frame;
            d.end_frame = e.end_frame;
            d.checksum = e.checksum;
            seek_table.push_back(d);
        }
        const std::vector<int64_t> game_times(s.footer.times.game_time.begin(), s.footer.times.game_time.end());
        const std::vector<int64_t> created_utc(s.footer.times.created_utc.begin(), s.footer.times.created_utc.end());
        const std::vector<int32_t> gaps(s.footer.times.gaps.begin(), s.footer.times.gaps.end());
        const std::vector<int32_t> segments(s.footer.times.segments.begin(), s.footer.times.segments.end());

        VTX::SessionFooter sf;
        sf.total_frames = s.footer.total_frames;
        sf.duration_seconds = s.footer.duration_seconds;
        sf.game_times = &game_times;
        sf.created_utc = &created_utc;
        sf.gaps = &gaps;
        sf.segments = &segments;
        sf.events = &events;

        const bool flat = format == VTX::VtxFormat::FlatBuffers;
        const std::string payload = flat ? VTX::FlatBuffersVtxPolicy::SerializeFooter(seek_table, sf)
                                         : VTX::ProtobufVtxPolicy::SerializeFooter(seek_table, sf);
        const std::string magic =
            flat ? VTX::FlatBuffersVtxPolicy::GetMagicBytes() : VTX::ProtobufVtxPolicy::GetMagicBytes();

        // Locate the existing footer block: [payload][u32 size][magic] at the tail.
        uint64_t file_size = 0;
        uint32_t old_footer_size = 0;
        {
            std::ifstream in(path, std::ios::binary);
            ASSERT_TRUE(in);
            in.seekg(0, std::ios::end);
            file_size = static_cast<uint64_t>(in.tellg());
            in.seekg(static_cast<std::streamoff>(file_size - magic.size() - sizeof(uint32_t)));
            ASSERT_TRUE(in.read(reinterpret_cast<char*>(&old_footer_size), sizeof(old_footer_size)));
        }
        const uint64_t footer_offset = file_size - magic.size() - sizeof(uint32_t) - old_footer_size;
        std::filesystem::resize_file(path, footer_offset);

        std::ofstream out(path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out);
        const uint32_t footer_size = static_cast<uint32_t>(payload.size());
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.write(reinterpret_cast<const char*>(&footer_size), sizeof(footer_size));
        out.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        ASSERT_TRUE(out.good());
    }

} // namespace

class ReplayFilterTest : public ::testing::TestWithParam<VTX::VtxFormat> {
protected:
    std::string Path(const std::string& suffix) const {
        return VtxTest::OutputPath(std::string("replay_filter_") + FormatName(GetParam()) + "_" + suffix + ".vtx");
    }

    std::string WriteSource(const std::string& suffix) const {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = Path(suffix + "_src");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "ReplayFilterTest";
        cfg.replay_uuid = "uuid-filter-" + suffix;
        cfg.default_fps = kFps;
        cfg.chunk_max_frames = kChunkFrames;
        cfg.use_compression = true;

        auto writer = GetParam() == VTX::VtxFormat::FlatBuffers ? VTX::CreateFlatBuffersWriterFacade(cfg)
                                                                : VTX::CreateProtobufWriterFacade(cfg);
        EXPECT_TRUE(writer);
        if (!writer) {
            return {};
        }
        for (int i = 0; i < kFrames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            t.created_utc_time = kBaseUtc + int64_t(i) * kFrameTicks;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
        return cfg.output_filepath;
    }

    std::string WriteMultiBucketSource(const std::string& suffix) const {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = Path(suffix + "_src");
        cfg.schema_json_content = MultiBucketSchemaJson();
        cfg.replay_name = "ReplayFilterTest";
        cfg.replay_uuid = "uuid-filter-" + suffix;
        cfg.default_fps = kFps;
        cfg.chunk_max_frames = kChunkFrames;
        cfg.use_compression = true;

        auto writer = GetParam() == VTX::VtxFormat::FlatBuffers ? VTX::CreateFlatBuffersWriterFacade(cfg)
                                                                : VTX::CreateProtobufWriterFacade(cfg);
        EXPECT_TRUE(writer);
        if (!writer) {
            return {};
        }
        for (int i = 0; i < kFrames; ++i) {
            auto frame = BuildMultiBucketFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / kFps;
            t.created_utc_time = kBaseUtc + int64_t(i) * kFrameTicks;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
        return cfg.output_filepath;
    }
};

// ---------------------------------------------------------------------------
// Blacklist by struct name: every Projectile goes, everything else is untouched,
// header/footer framing survives, the seek table describes the bytes on disk.
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, DropByStructName) {
    const std::string src = WriteSource("dropstruct");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("dropstruct_out");

    VTX::ReplayFilterSpec spec;
    spec.mode = VTX::ReplayFilterMode::Drop;
    spec.rules.push_back(VTX::ReplayFilterRule::StructName("Projectile"));

    std::vector<std::pair<int32_t, int32_t>> ticks;
    const auto r = VTX::FilterReplayFile(src, dst, spec, [&](int32_t done, int32_t total) {
        ticks.emplace_back(done, total);
        return true;
    });
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.total_frames, kFrames);
    EXPECT_EQ(r.chunks_rewritten, 3);
    EXPECT_EQ(r.entities_seen, 4u * kFrames);
    EXPECT_EQ(r.entities_dropped, 1u * kFrames);
    EXPECT_EQ(r.entities_kept, 3u * kFrames);
    ASSERT_EQ(r.dropped_by_struct.size(), 1u);
    EXPECT_EQ(r.dropped_by_struct.at("Projectile"), 1u * kFrames);
    EXPECT_EQ(r.events_kept, 0u);
    EXPECT_EQ(r.events_dropped, 0u);
    EXPECT_GT(r.output_bytes, 0u);
    EXPECT_LE(r.output_bytes, r.source_bytes);
    EXPECT_GE(r.elapsed_seconds, 0.0);
    ASSERT_EQ(ticks.size(), 4u); // (0,3) then one per chunk
    EXPECT_EQ(ticks.front(), std::make_pair(0, 3));
    EXPECT_EQ(ticks.back(), std::make_pair(3, 3));

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectSameHeaderFooterFraming(source, output);
    ExpectFramesFiltered(source, output,
                         [](const VTX::Bucket& b, size_t i) { return b.entities[i].entity_type_id == kProjectile; });
    ExpectSeekTableMatchesFile(dst, output.footer);
    // The reader exposes the seek-table checksums through GetFooter() for the
    // source too (they used to be dropped in the footer conversion).
    ExpectSeekTableMatchesFile(src, source.footer);
}

// ---------------------------------------------------------------------------
// Whitelist by unique id glob: only the players remain.
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, KeepByUniqueIdGlob) {
    const std::string src = WriteSource("keepid");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("keepid_out");

    VTX::ReplayFilterSpec spec;
    spec.mode = VTX::ReplayFilterMode::Keep;
    spec.rules.push_back(VTX::ReplayFilterRule::UniqueId("player_*"));

    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.entities_kept, 2u * kFrames);
    EXPECT_EQ(r.entities_dropped, 2u * kFrames);
    EXPECT_EQ(r.dropped_by_struct.at("Projectile"), 1u * kFrames);
    EXPECT_EQ(r.dropped_by_struct.at("MatchState"), 1u * kFrames);

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectSameHeaderFooterFraming(source, output);
    ExpectFramesFiltered(source, output,
                         [](const VTX::Bucket& b, size_t i) { return b.unique_ids[i].rfind("player_", 0) != 0; });
    for (const VTX::Frame& frame : output.frames) {
        ASSERT_EQ(frame.GetBuckets().size(), 1u);
        EXPECT_EQ(frame.GetBuckets()[0].unique_ids, (std::vector<std::string> {"player_0", "player_1"}));
    }
    ExpectSeekTableMatchesFile(dst, output.footer);
}

// ---------------------------------------------------------------------------
// Property rules: on one struct, and on "any struct with that field".
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, DropByPropertyValue) {
    const std::string src = WriteSource("dropprop");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("dropprop_out");

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::Property("Player", "Name", "Bra*"));
    // Empty struct = every struct that declares a scalar "UniqueID" (all three do).
    spec.rules.push_back(VTX::ReplayFilterRule::Property("", "UniqueID", "proj_1"));

    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    int proj1_frames = 0;
    for (int i = 0; i < kFrames; ++i) {
        proj1_frames += (i % 3 == 1) ? 1 : 0;
    }
    EXPECT_EQ(r.entities_dropped, uint64_t(kFrames + proj1_frames));
    EXPECT_EQ(r.dropped_by_struct.at("Player"), uint64_t(kFrames));
    EXPECT_EQ(r.dropped_by_struct.at("Projectile"), uint64_t(proj1_frames));

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectSameHeaderFooterFraming(source, output);
    ExpectFramesFiltered(source, output, [](const VTX::Bucket& b, size_t i) {
        return b.unique_ids[i] == "player_1" || b.unique_ids[i] == "proj_1";
    });
    ExpectSeekTableMatchesFile(dst, output.footer);
}

// A numeric property, matched through its decimal string form.
TEST_P(ReplayFilterTest, DropByNumericProperty) {
    const std::string src = WriteSource("dropnum");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("dropnum_out");

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::Property("Player", "Team", "2"));
    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.entities_dropped, 1u * kFrames);

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectFramesFiltered(source, output, [](const VTX::Bucket& b, size_t i) { return b.unique_ids[i] == "player_1"; });
}

// ---------------------------------------------------------------------------
// Case-insensitive glob on the struct name.
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, CaseInsensitiveStructGlob) {
    const std::string src = WriteSource("cistruct");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("cistruct_out");

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::StructName("MATCH*", /*case_insensitive=*/true));
    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.entities_dropped, 1u * kFrames);
    EXPECT_EQ(r.dropped_by_struct.at("MatchState"), 1u * kFrames);

    // Case-sensitive by default: the same pattern matches nothing.
    VTX::ReplayFilterSpec strict;
    strict.rules.push_back(VTX::ReplayFilterRule::StructName("MATCH*"));
    const auto r2 = VTX::FilterReplayFile(src, Path("cistruct_strict_out"), strict);
    ASSERT_TRUE(r2.ok()) << r2.error;
    EXPECT_EQ(r2.entities_dropped, 0u);
    EXPECT_EQ(r2.entities_kept, 4u * kFrames);
}

// ---------------------------------------------------------------------------
// A whitelist that matches nothing still writes every frame, with empty buckets.
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, KeepNothingLeavesEmptyBuckets) {
    const std::string src = WriteSource("keepnone");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("keepnone_out");

    VTX::ReplayFilterSpec spec;
    spec.mode = VTX::ReplayFilterMode::Keep;
    spec.rules.push_back(VTX::ReplayFilterRule::UniqueId("nothing_has_this_id"));
    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.entities_kept, 0u);
    EXPECT_EQ(r.entities_dropped, 4u * kFrames);

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectSameHeaderFooterFraming(source, output);
    ASSERT_EQ(output.frames.size(), size_t(kFrames));
    for (const VTX::Frame& frame : output.frames) {
        ASSERT_EQ(frame.GetBuckets().size(), 1u);
        EXPECT_TRUE(frame.GetBuckets()[0].entities.empty());
        EXPECT_TRUE(frame.GetBuckets()[0].unique_ids.empty());
    }
    ExpectSeekTableMatchesFile(dst, output.footer);
}

// ---------------------------------------------------------------------------
// Timeline events ride along and are pruned to the entities that still exist.
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, EventsCarriedAndPruned) {
    const std::string src = WriteSource("events");
    ASSERT_FALSE(src.empty());
    const std::vector<VTX::TimelineEvent> events = {
        MakeEvent(0.5f, "Fire", "proj_0 launched", "proj_0"),
        MakeEvent(1.0f, "Spawn", "player_0 spawned", "player_0"),
        MakeEvent(1.5f, "Round", "round started", ""),
    };
    RewriteFooterWithEvents(src, GetParam(), events);
    {
        const Snapshot s = ReadReplay(src);
        ASSERT_TRUE(s.ok);
        ASSERT_EQ(s.footer.events.size(), 3u) << "fixture should now carry events";
        EXPECT_EQ(s.footer.total_frames, kFrames);
    }

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::StructName("Projectile"));
    spec.prune_events = true;
    const std::string pruned = Path("events_pruned");
    const auto r = VTX::FilterReplayFile(src, pruned, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.events_kept, 2u);
    EXPECT_EQ(r.events_dropped, 1u);
    {
        const Snapshot out = ReadReplay(pruned);
        ASSERT_TRUE(out.ok);
        ASSERT_EQ(out.footer.events.size(), 2u);
        EXPECT_EQ(out.footer.events[0].entity_unique_id, "player_0");
        EXPECT_EQ(out.footer.events[0].event_type, "Spawn");
        EXPECT_EQ(out.footer.events[0].label, "player_0 spawned");
        EXPECT_FLOAT_EQ(out.footer.events[0].game_time, 1.0f);
        EXPECT_EQ(out.footer.events[0].location, (VTX::Vector {1.0, 2.0, 3.0}));
        EXPECT_EQ(out.footer.events[1].entity_unique_id, "");
        EXPECT_EQ(out.footer.events[1].label, "round started");
        EXPECT_EQ(out.footer.total_frames, kFrames);
    }

    spec.prune_events = false;
    const std::string carried = Path("events_carried");
    const auto r2 = VTX::FilterReplayFile(src, carried, spec);
    ASSERT_TRUE(r2.ok()) << r2.error;
    EXPECT_EQ(r2.events_kept, 3u);
    EXPECT_EQ(r2.events_dropped, 0u);
    const Snapshot out = ReadReplay(carried);
    ASSERT_TRUE(out.ok);
    ASSERT_EQ(out.footer.events.size(), 3u);
    EXPECT_EQ(out.footer.events[0].entity_unique_id, "proj_0");
}

// ---------------------------------------------------------------------------
// Failure paths report an error and never leave a destination behind.
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, ErrorsAreReported) {
    const std::string src = WriteSource("errors");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("errors_out");
    namespace fs = std::filesystem;

    {
        VTX::ReplayFilterSpec empty;
        const auto r = VTX::FilterReplayFile(src, dst, empty);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("no rules"), std::string::npos) << r.error;
        EXPECT_FALSE(fs::exists(dst));
    }
    {
        VTX::ReplayFilterSpec spec;
        spec.rules.push_back(VTX::ReplayFilterRule::Property("Nope", "X", "*"));
        const auto r = VTX::FilterReplayFile(src, dst, spec);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("not in the schema"), std::string::npos) << r.error;
        EXPECT_FALSE(fs::exists(dst));
    }
    {
        VTX::ReplayFilterSpec spec;
        spec.rules.push_back(VTX::ReplayFilterRule::Property("Player", "Nope", "*"));
        const auto r = VTX::FilterReplayFile(src, dst, spec);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("has no field"), std::string::npos) << r.error;
    }
    {
        VTX::ReplayFilterSpec spec;
        spec.rules.push_back(VTX::ReplayFilterRule::Property("Player", "Position", "*"));
        const auto r = VTX::FilterReplayFile(src, dst, spec);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("not a scalar"), std::string::npos) << r.error;
    }
    {
        VTX::ReplayFilterSpec spec;
        spec.rules.push_back(VTX::ReplayFilterRule::StructName(""));
        const auto r = VTX::FilterReplayFile(src, dst, spec);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("empty pattern"), std::string::npos) << r.error;
    }
    {
        VTX::ReplayFilterSpec spec;
        spec.rules.push_back(VTX::ReplayFilterRule::StructName("Projectile"));
        const auto r = VTX::FilterReplayFile(src, src, spec);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("different file"), std::string::npos) << r.error;
        EXPECT_TRUE(fs::exists(src));
    }
    {
        VTX::ReplayFilterSpec spec;
        spec.rules.push_back(VTX::ReplayFilterRule::StructName("Projectile"));
        const auto r = VTX::FilterReplayFile(Path("does_not_exist"), dst, spec);
        EXPECT_FALSE(r.ok());
        EXPECT_NE(r.error.find("does not exist"), std::string::npos) << r.error;
        EXPECT_FALSE(fs::exists(dst));
    }
}

TEST_P(ReplayFilterTest, CancelRemovesDestination) {
    const std::string src = WriteSource("cancel");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("cancel_out");

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::StructName("Projectile"));
    const auto r = VTX::FilterReplayFile(src, dst, spec, [](int32_t done, int32_t) { return done < 1; });
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error.find("cancel"), std::string::npos) << r.error;
    EXPECT_EQ(r.chunks_rewritten, 1);
    EXPECT_FALSE(std::filesystem::exists(dst));
}

// ---------------------------------------------------------------------------
// Bucket rules: a dropped bucket is emptied in every frame and keeps its slot.
// ---------------------------------------------------------------------------
TEST_P(ReplayFilterTest, DropByBucketName) {
    const std::string src = WriteMultiBucketSource("dropbucket");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("dropbucket_out");

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::Bucket("fx"));
    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.entities_seen, 4u * kFrames);
    EXPECT_EQ(r.entities_dropped, 1u * kFrames);
    EXPECT_EQ(r.entities_kept, 3u * kFrames);
    ASSERT_EQ(r.dropped_by_bucket.size(), 1u);
    EXPECT_EQ(r.dropped_by_bucket.at("fx"), 1u * kFrames);
    EXPECT_EQ(r.dropped_by_struct.at("Projectile"), 1u * kFrames);

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectSameHeaderFooterFraming(source, output);
    ExpectFramesFilteredByBucket(source, output, [](size_t bucket, const VTX::Bucket&, size_t) { return bucket == 1; });
    for (const VTX::Frame& frame : output.frames) {
        ASSERT_EQ(frame.GetBuckets().size(), 3u); // the emptied bucket keeps its slot
        EXPECT_TRUE(frame.GetBuckets()[1].entities.empty());
        EXPECT_TRUE(frame.GetBuckets()[1].unique_ids.empty());
        EXPECT_EQ(frame.GetBuckets()[0].unique_ids, (std::vector<std::string> {"player_0", "player_1"}));
        EXPECT_EQ(frame.GetBuckets()[2].unique_ids, (std::vector<std::string> {"match"}));
    }
    ExpectSeekTableMatchesFile(dst, output.footer);
}

// A whitelisted bucket keeps all of its entities; other rules still apply elsewhere.
TEST_P(ReplayFilterTest, KeepBucketCombinesWithOtherRules) {
    const std::string src = WriteMultiBucketSource("keepbucket");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("keepbucket_out");

    VTX::ReplayFilterSpec spec;
    spec.mode = VTX::ReplayFilterMode::Keep;
    spec.rules.push_back(VTX::ReplayFilterRule::Bucket("telemetry"));
    spec.rules.push_back(VTX::ReplayFilterRule::StructName("Player"));
    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.entities_kept, 3u * kFrames);
    EXPECT_EQ(r.entities_dropped, 1u * kFrames);
    EXPECT_EQ(r.dropped_by_bucket.at("fx"), 1u * kFrames);

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectSameHeaderFooterFraming(source, output);
    ExpectFramesFilteredByBucket(source, output, [](size_t bucket, const VTX::Bucket& b, size_t i) {
        return bucket != 2 && b.entities[i].entity_type_id != kPlayer;
    });
    for (const VTX::Frame& frame : output.frames) {
        ASSERT_EQ(frame.GetBuckets().size(), 3u);
        EXPECT_EQ(frame.GetBuckets()[0].entities.size(), 2u);
        EXPECT_TRUE(frame.GetBuckets()[1].entities.empty());
        EXPECT_EQ(frame.GetBuckets()[2].unique_ids, (std::vector<std::string> {"match"}));
    }
}

// Buckets can be addressed by index or by (case-insensitive) name glob; a rule
// that matches no schema bucket is an error rather than a silent no-op.
TEST_P(ReplayFilterTest, BucketByIndexAndGlob) {
    const std::string src = WriteMultiBucketSource("bucketidx");
    ASSERT_FALSE(src.empty());
    const std::string dst = Path("bucketidx_out");

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::Bucket("2"));
    spec.rules.push_back(VTX::ReplayFilterRule::Bucket("F?", /*case_insensitive=*/true));
    const auto r = VTX::FilterReplayFile(src, dst, spec);
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.entities_dropped, 2u * kFrames);
    EXPECT_EQ(r.dropped_by_bucket.at("fx"), 1u * kFrames);
    EXPECT_EQ(r.dropped_by_bucket.at("telemetry"), 1u * kFrames);

    const Snapshot source = ReadReplay(src);
    const Snapshot output = ReadReplay(dst);
    ASSERT_TRUE(source.ok);
    ASSERT_TRUE(output.ok);
    ExpectFramesFilteredByBucket(source, output, [](size_t bucket, const VTX::Bucket&, size_t) { return bucket >= 1; });

    VTX::ReplayFilterSpec strict;
    strict.rules.push_back(VTX::ReplayFilterRule::Bucket("F?")); // case-sensitive: matches nothing
    const auto r2 = VTX::FilterReplayFile(src, Path("bucketidx_strict_out"), strict);
    EXPECT_FALSE(r2.ok());
    EXPECT_NE(r2.error.find("no schema bucket"), std::string::npos) << r2.error;
    EXPECT_NE(r2.error.find("'telemetry'"), std::string::npos) << r2.error; // lists the available names

    VTX::ReplayFilterSpec bad_index;
    bad_index.rules.push_back(VTX::ReplayFilterRule::Bucket("99999999999"));
    const auto r3 = VTX::FilterReplayFile(src, Path("bucketidx_bad_out"), bad_index);
    EXPECT_FALSE(r3.ok());
    EXPECT_NE(r3.error.find("not a valid bucket index"), std::string::npos) << r3.error;
}

INSTANTIATE_TEST_SUITE_P(Formats, ReplayFilterTest,
                         ::testing::Values(VTX::VtxFormat::FlatBuffers, VTX::VtxFormat::Protobuf),
                         [](const ::testing::TestParamInfo<VTX::VtxFormat>& info) { return FormatName(info.param); });

// ===========================================================================
//  Rule primitives, no file involved.
// ===========================================================================
TEST(ReplayFilterGlob, Semantics) {
    EXPECT_TRUE(VTX::GlobMatch("*", ""));
    EXPECT_TRUE(VTX::GlobMatch("*", "anything"));
    EXPECT_TRUE(VTX::GlobMatch("", ""));
    EXPECT_FALSE(VTX::GlobMatch("", "a"));
    EXPECT_TRUE(VTX::GlobMatch("abc", "abc"));
    EXPECT_FALSE(VTX::GlobMatch("abc", "abcd"));
    EXPECT_TRUE(VTX::GlobMatch("a*", "abc"));
    EXPECT_TRUE(VTX::GlobMatch("*c", "abc"));
    EXPECT_TRUE(VTX::GlobMatch("a?c", "abc"));
    EXPECT_FALSE(VTX::GlobMatch("a?c", "ac"));
    EXPECT_TRUE(VTX::GlobMatch("*.vtx", "capture.vtx"));
    EXPECT_TRUE(VTX::GlobMatch("*x*y*", "axbyc"));
    EXPECT_TRUE(VTX::GlobMatch("a*b*c", "axxbxxc"));
    EXPECT_FALSE(VTX::GlobMatch("a*b*c", "acb"));
    EXPECT_TRUE(VTX::GlobMatch("**a", "ba"));
    EXPECT_FALSE(VTX::GlobMatch("abc", "ABC"));
    EXPECT_TRUE(VTX::GlobMatch("abc", "ABC", /*case_insensitive=*/true));
    EXPECT_TRUE(VTX::GlobMatch("SystemHealth*", "SystemHealthProcess"));
    EXPECT_FALSE(VTX::GlobMatch("SystemHealth*", "PlayerState"));
}

TEST(ReplayFilterMatcher, ApplyRemovesRejectedEntitiesInPlace) {
    VTX::SchemaRegistry registry;
    ASSERT_TRUE(registry.LoadFromJson(VtxTest::FixturePath("test_schema.json")));
    const VTX::PropertyAddressCache& cache = registry.GetPropertyCache();
    ASSERT_FALSE(cache.structs.empty());

    VTX::ReplayFilterSpec spec;
    spec.mode = VTX::ReplayFilterMode::Drop;
    spec.rules.push_back(VTX::ReplayFilterRule::StructName("Projectile"));
    spec.rules.push_back(VTX::ReplayFilterRule::UniqueId("player_1"));
    const VTX::ReplayFilterMatcher matcher(spec, cache);
    ASSERT_TRUE(matcher.valid()) << matcher.error();
    EXPECT_EQ(matcher.TypeName(kProjectile), "Projectile");
    EXPECT_EQ(matcher.TypeName(999), "type#999");

    VTX::Frame frame = BuildFrame(5); // player_0, player_1, proj_2, match
    VTX::Bucket& bucket = frame.GetBucket("entity");
    ASSERT_EQ(bucket.entities.size(), 4u);
    EXPECT_TRUE(matcher.Keeps(0, bucket, 0));
    EXPECT_FALSE(matcher.Keeps(0, bucket, 1));
    EXPECT_FALSE(matcher.Keeps(0, bucket, 2));
    EXPECT_TRUE(matcher.Keeps(0, bucket, 3));
    EXPECT_FALSE(matcher.Matches(0, bucket, 42)); // out of range never matches

    std::vector<std::string> dropped_ids;
    const size_t dropped = matcher.Apply(
        0, bucket, [&](const VTX::Bucket& b, size_t index) { dropped_ids.push_back(b.unique_ids[index]); });
    EXPECT_EQ(dropped, 2u);
    EXPECT_EQ(dropped_ids, (std::vector<std::string> {"player_1", "proj_2"}));
    EXPECT_EQ(bucket.unique_ids, (std::vector<std::string> {"player_0", "match"}));
    ASSERT_EQ(bucket.entities.size(), 2u);
    EXPECT_EQ(bucket.entities[0].entity_type_id, kPlayer);
    EXPECT_EQ(bucket.entities[1].entity_type_id, kMatchState);
    EXPECT_TRUE(bucket.type_ranges.empty());

    // Nothing left to remove: the bucket is untouched and 0 is reported.
    EXPECT_EQ(matcher.Apply(0, bucket), 0u);
    EXPECT_EQ(bucket.entities.size(), 2u);

    // Keep mode with a property rule resolved against the schema.
    VTX::ReplayFilterSpec keep;
    keep.mode = VTX::ReplayFilterMode::Keep;
    keep.rules.push_back(VTX::ReplayFilterRule::Property("MatchState", "Phase", "warm*"));
    const VTX::ReplayFilterMatcher keeper(keep, cache);
    ASSERT_TRUE(keeper.valid()) << keeper.error();
    VTX::Frame early = BuildFrame(5); // Phase = warmup
    VTX::Frame late = BuildFrame(75); // Phase = live
    EXPECT_EQ(keeper.Apply(0, early.GetBucket("entity")), 3u);
    EXPECT_EQ(early.GetBucket("entity").unique_ids, (std::vector<std::string> {"match"}));
    EXPECT_EQ(keeper.Apply(0, late.GetBucket("entity")), 4u);
    EXPECT_TRUE(late.GetBucket("entity").entities.empty());

    // Unresolvable rules are reported, not silently ignored.
    VTX::ReplayFilterSpec bad;
    bad.rules.push_back(VTX::ReplayFilterRule::Property("", "NoSuchField", "*"));
    const VTX::ReplayFilterMatcher invalid(bad, cache);
    EXPECT_FALSE(invalid.valid());
    EXPECT_NE(invalid.error().find("NoSuchField"), std::string::npos) << invalid.error();
}

TEST(ReplayFilterMatcher, BucketRulesResolveAgainstSchemaBuckets) {
    VTX::SchemaRegistry registry;
    ASSERT_TRUE(registry.LoadFromRawString(MultiBucketSchemaJson()));
    const VTX::PropertyAddressCache& cache = registry.GetPropertyCache();
    ASSERT_EQ(cache.bucket_names, (std::vector<std::string> {"entity", "fx", "telemetry"}));

    VTX::ReplayFilterSpec spec;
    spec.rules.push_back(VTX::ReplayFilterRule::Bucket("tele*"));
    spec.rules.push_back(VTX::ReplayFilterRule::Bucket("7")); // by index, beyond the named ones
    const VTX::ReplayFilterMatcher matcher(spec, cache);
    ASSERT_TRUE(matcher.valid()) << matcher.error();
    EXPECT_FALSE(matcher.BucketMatches(0));
    EXPECT_FALSE(matcher.BucketMatches(1));
    EXPECT_TRUE(matcher.BucketMatches(2));
    EXPECT_TRUE(matcher.BucketMatches(7));
    EXPECT_FALSE(matcher.BucketMatches(-1));
    EXPECT_EQ(matcher.BucketName(1), "fx");
    EXPECT_EQ(matcher.BucketName(7), "bucket#7");

    VTX::Frame frame = BuildMultiBucketFrame(3);
    VTX::Bucket& telemetry = frame.GetBucket("telemetry");
    VTX::Bucket& fx = frame.GetBucket("fx");
    EXPECT_TRUE(matcher.Matches(2, telemetry, 0));
    EXPECT_FALSE(matcher.Matches(1, fx, 0));
    EXPECT_FALSE(matcher.Keeps(2, telemetry, 0)); // Drop mode
    EXPECT_TRUE(matcher.Keeps(1, fx, 0));
    EXPECT_EQ(matcher.Apply(2, telemetry), 1u);
    EXPECT_TRUE(telemetry.entities.empty());
    EXPECT_EQ(matcher.Apply(1, fx), 0u);

    VTX::ReplayFilterSpec unknown;
    unknown.rules.push_back(VTX::ReplayFilterRule::Bucket("nope"));
    const VTX::ReplayFilterMatcher invalid(unknown, cache);
    EXPECT_FALSE(invalid.valid());
    EXPECT_NE(invalid.error().find("no schema bucket"), std::string::npos) << invalid.error();
}
