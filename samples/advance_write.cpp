// advance_write.cpp -- Arena sample: data-source CONSUMER + writer driver.
//
// Stage 2 of the two-stage arena sample pipeline.  Reads the three data-
// source files produced by generate_replay.cpp, maps each record into a VTX
// Frame using SDK-native integration primitives, and writes a .vtx replay
// per source.
//
// Pipeline per source:
//
//     content/writer/arena/arena_replay_data.json       -> arena_from_json_ds.vtx
//     content/writer/arena/arena_replay_data.proto.bin  -> arena_from_proto_ds.vtx
//     content/writer/arena/arena_replay_data.fbs.bin    -> arena_from_fbs_ds.vtx
//
// Each source implements VTX::IFrameDataSource (Initialize / GetNextFrame /
// GetExpectedTotalFrames).  The pattern mirrors the real integrations shipped
// with the SDK -- see tools/integrations/rl/rl15/rl15_data_source.h.
//
// Mapping strategies demonstrated, one per format:
//
//   JSON  -> VTX::JsonMapping<T> specializations in arena_mappings.h are
//            walked by VTX::UniversalDeserializer to build ArenaReplayJson,
//            then ArenaToVtx::MapFrame() produces each VTX::Frame.
//
//   Proto -> VTX::ProtoBinding<T> specializations (below) are dispatched by
//            VTX::GenericProtobufLoader::LoadFrame().  Field-name lookups go
//            through SchemaRegistry at runtime.
//
//   FBS   -> VTX::FlatBufferBinding<T> specializations (below) are dispatched
//            by VTX::GenericFlatBufferLoader::LoadFrame().  Field addresses
//            are resolved via PropertyAddressCache (O(1) by entity_type_id).
//
// The integration-style bindings use constant-named slots from arena_generated.h,
// which is produced by:
//
//   python scripts/vtx_codegen.py \
//       samples/content/writer/arena/arena_schema.json \
//       samples/arena_generated.h ArenaSchema

#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/core/vtx_data_source.h"
#include "vtx/common/adapters/json/json_adapter.h"
#include "vtx/common/vtx_types_helpers.h"
#include "vtx/common/readers/frame_reader/flatbuffer_loader.h"
#include "vtx/common/readers/frame_reader/protobuff_loader.h"
#include "vtx/common/readers/frame_reader/universal_deserializer.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_types.h"

#include "arena_generated.h"
#include "arena_mappings.h"
#include "arena_data.pb.h"
#include "arena_data_generated.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <nlohmann/json.hpp>

namespace {

    VTX::Vector ToVtxVector(const ::arena_pb::Vec3& value) {
        return {value.x(), value.y(), value.z()};
    }

    VTX::Quat ToVtxQuat(const ::arena_pb::Rotation& value) {
        return {value.x(), value.y(), value.z(), value.w()};
    }

    VTX::Vector ToVtxVector(const ::arena_fb::Vec3& value) {
        return {value.x(), value.y(), value.z()};
    }

    VTX::Quat ToVtxQuat(const ::arena_fb::Rotation& value) {
        return {value.x(), value.y(), value.z(), value.w()};
    }

} // namespace

namespace VTX {

    // ===================================================================
    //  Protobuf bindings -- driven by VTX::GenericProtobufLoader
    // ===================================================================
    //  Each Transfer() method maps one protobuf message into a
    //  VTX::PropertyContainer by delegating scalar/struct/array copies to the
    //  loader, which resolves field slots via SchemaRegistry.

    template <>
    struct ProtoBinding<::arena_pb::Player> {
        static void Transfer(const ::arena_pb::Player& src, VTX::PropertyContainer& dest,
                             VTX::GenericProtobufLoader& loader, const std::string& schema_name) {
            loader.LoadField(dest, schema_name, ArenaSchema::Player::UniqueID, src.unique_id());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Name, src.name());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Team, src.team());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Health, src.health());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Armor, src.armor());
            if (src.has_position()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::Position, ToVtxVector(src.position()));
            }
            if (src.has_rotation()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::Rotation, ToVtxQuat(src.rotation()));
            }
            if (src.has_velocity()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::Velocity, ToVtxVector(src.velocity()));
            }
            loader.LoadField(dest, schema_name, ArenaSchema::Player::IsAlive, src.is_alive());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Score, src.score());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Deaths, src.deaths());
        }
    };

    template <>
    struct ProtoBinding<::arena_pb::Projectile> {
        static void Transfer(const ::arena_pb::Projectile& src, VTX::PropertyContainer& dest,
                             VTX::GenericProtobufLoader& loader, const std::string& schema_name) {
            loader.LoadField(dest, schema_name, ArenaSchema::Projectile::UniqueID, src.unique_id());
            loader.LoadField(dest, schema_name, ArenaSchema::Projectile::OwnerID, src.owner_id());
            if (src.has_position()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Position, ToVtxVector(src.position()));
            }
            if (src.has_velocity()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Velocity, ToVtxVector(src.velocity()));
            }
            loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Damage, src.damage());
            loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Type, src.type());
        }
    };

    template <>
    struct ProtoBinding<::arena_pb::MatchState> {
        static void Transfer(const ::arena_pb::MatchState& src, VTX::PropertyContainer& dest,
                             VTX::GenericProtobufLoader& loader, const std::string& schema_name) {
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::UniqueID, std::string("match_001"));
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::ScoreTeam1, src.score_team1());
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::ScoreTeam2, src.score_team2());
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::Round, src.round());
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::Phase, src.phase());
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::TimeRemaining, src.time_remaining());
        }
    };

    template <>
    struct ProtoBinding<::arena_pb::FrameData> {
        static void TransferToFrame(const ::arena_pb::FrameData& src, VTX::Frame& dest,
                                    VTX::GenericProtobufLoader& loader, const std::string& schema_name) {
            (void)schema_name;

            dest = VTX::Frame {};
            VTX::Bucket& entity_bucket = dest.GetBucket("entity");
            entity_bucket.entities.clear();
            entity_bucket.unique_ids.clear();

            loader.AppendActorList(entity_bucket, ArenaSchema::Player::StructName, src.players(),
                                   [](const ::arena_pb::Player& player) { return player.unique_id(); });

            loader.AppendActorList(entity_bucket, ArenaSchema::Projectile::StructName, src.projectiles(),
                                   [](const ::arena_pb::Projectile& projectile) { return projectile.unique_id(); });

            if (src.has_match_state()) {
                loader.AppendSingleActor(entity_bucket, ArenaSchema::MatchState::StructName, src.match_state(),
                                         [](const ::arena_pb::MatchState&) { return std::string("match_001"); });
            }
        }
    };

    // ===================================================================
    //  FlatBuffers bindings -- driven by VTX::GenericFlatBufferLoader
    // ===================================================================
    //  Transfer() receives a pointer to the generated FBS table (zero-copy
    //  access into the mapped buffer).  Field slots are resolved once via
    //  PropertyAddressCache -- subsequent frames hit the cached addresses.

    template <>
    struct FlatBufferBinding<::arena_fb::Player> {
        static void Transfer(const ::arena_fb::Player* src, VTX::PropertyContainer& dest,
                             VTX::GenericFlatBufferLoader& loader, const std::string& schema_name) {
            if (src->unique_id()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::UniqueID, src->unique_id()->str());
            }
            if (src->name()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::Name, src->name()->str());
            }
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Team, src->team());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Health, src->health());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Armor, src->armor());
            if (src->position()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::Position, ToVtxVector(*src->position()));
            }
            if (src->rotation()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::Rotation, ToVtxQuat(*src->rotation()));
            }
            if (src->velocity()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Player::Velocity, ToVtxVector(*src->velocity()));
            }
            loader.LoadField(dest, schema_name, ArenaSchema::Player::IsAlive, src->is_alive());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Score, src->score());
            loader.LoadField(dest, schema_name, ArenaSchema::Player::Deaths, src->deaths());
        }
    };

    template <>
    struct FlatBufferBinding<::arena_fb::Projectile> {
        static void Transfer(const ::arena_fb::Projectile* src, VTX::PropertyContainer& dest,
                             VTX::GenericFlatBufferLoader& loader, const std::string& schema_name) {
            if (src->unique_id()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Projectile::UniqueID, src->unique_id()->str());
            }
            if (src->owner_id()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Projectile::OwnerID, src->owner_id()->str());
            }
            if (src->position()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Position, ToVtxVector(*src->position()));
            }
            if (src->velocity()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Velocity, ToVtxVector(*src->velocity()));
            }
            loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Damage, src->damage());
            if (src->type()) {
                loader.LoadField(dest, schema_name, ArenaSchema::Projectile::Type, src->type()->str());
            }
        }
    };

    template <>
    struct FlatBufferBinding<::arena_fb::MatchState> {
        static void Transfer(const ::arena_fb::MatchState* src, VTX::PropertyContainer& dest,
                             VTX::GenericFlatBufferLoader& loader, const std::string& schema_name) {
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::UniqueID, std::string("match_001"));
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::ScoreTeam1, src->score_team1());
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::ScoreTeam2, src->score_team2());
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::Round, src->round());
            if (src->phase()) {
                loader.LoadField(dest, schema_name, ArenaSchema::MatchState::Phase, src->phase()->str());
            }
            loader.LoadField(dest, schema_name, ArenaSchema::MatchState::TimeRemaining, src->time_remaining());
        }
    };

    template <>
    struct FlatBufferBinding<::arena_fb::FrameData> {
        static void TransferToFrame(const ::arena_fb::FrameData* src, VTX::Frame& dest,
                                    VTX::GenericFlatBufferLoader& loader, const std::string& schema_name) {
            (void)schema_name;

            dest = VTX::Frame {};
            VTX::Bucket& entity_bucket = dest.GetBucket("entity");
            entity_bucket.entities.clear();
            entity_bucket.unique_ids.clear();

            loader.AppendActorList(entity_bucket, ArenaSchema::Player::StructName, src->players(),
                                   [](const ::arena_fb::Player* player) {
                                       return player->unique_id() ? player->unique_id()->str() : std::string {};
                                   });

            loader.AppendActorList(entity_bucket, ArenaSchema::Projectile::StructName, src->projectiles(),
                                   [](const ::arena_fb::Projectile* projectile) {
                                       return projectile->unique_id() ? projectile->unique_id()->str() : std::string {};
                                   });

            loader.AppendSingleEntity(entity_bucket, ArenaSchema::MatchState::StructName, src->match_state(),
                                      [](const ::arena_fb::MatchState*) { return std::string("match_001"); });
        }
    };

} // namespace VTX


// ===================================================================
//  Data source 1 -- JSON
// ===================================================================
//  Parses the whole JSON blob on Initialize(), then cursors through the
//  deserialized ArenaReplayJson frame-by-frame.  The manual MapFrame()
//  bridge from arena_mappings.h handles the arena-types -> VTX conversion.

class ArenaJsonDataSource : public VTX::IFrameDataSource {
public:
    explicit ArenaJsonDataSource(std::string filepath)
        : filepath_(std::move(filepath)) {}

    bool Initialize() override {
        std::ifstream ifs(filepath_);
        if (!ifs.is_open()) {
            VTX_ERROR("[JSON ] Could not open: {}", filepath_);
            return false;
        }

        try {
            root_ = nlohmann::json::parse(ifs);
            replay_ = VTX::UniversalDeserializer<>::Load<ArenaReplayJson>(VTX::JsonAdapter(root_));
        } catch (const std::exception& e) {
            VTX_ERROR("[JSON ] Parse failed: {}", e.what());
            return false;
        }

        total_ = replay_.frames.size();
        VTX_INFO("[JSON ] Opened {} ({} frames)", filepath_, static_cast<int>(total_));
        return true;
    }

    bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
        if (cursor_ >= total_) {
            return false;
        }

        const ArenaFrame& af = replay_.frames[cursor_++];
        out_frame = ArenaToVtx::MapFrame(af);

        out_time = {af.game_time, std::nullopt, VTX::GameTime::EFilterType::OnlyIncreasing};

        return true;
    }

    size_t GetExpectedTotalFrames() const override { return total_; }

private:
    std::string filepath_;
    nlohmann::json root_;
    ArenaReplayJson replay_;
    size_t total_ = 0;
    size_t cursor_ = 0;
};

// ===================================================================
//  Data source 2 -- Protobuf
// ===================================================================
//  ParseFromIstream() materialises the whole ArenaReplay message, then each
//  GetNextFrame() delegates to the GenericProtobufLoader, which in turn
//  dispatches to the ProtoBinding<FrameData>::TransferToFrame() specialised
//  above.  The schema registry provides runtime field-name resolution.

class ArenaProtoDataSource : public VTX::IFrameDataSource {
public:
    ArenaProtoDataSource(std::string filepath, VTX::SchemaRegistry& schema)
        : filepath_(std::move(filepath))
        , loader_(schema, false) {}

    bool Initialize() override {
        std::ifstream ifs(filepath_, std::ios::binary);
        if (!ifs.is_open()) {
            VTX_ERROR("[Proto] Could not open: {}", filepath_);
            return false;
        }
        if (!replay_.ParseFromIstream(&ifs)) {
            VTX_ERROR("[Proto] ParseFromIstream failed for: {}", filepath_);
            return false;
        }

        total_ = static_cast<size_t>(replay_.frames_size());
        VTX_INFO("[Proto] Opened {} ({} frames)", filepath_, static_cast<int>(total_));
        return true;
    }

    bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
        if (cursor_ >= total_) {
            return false;
        }

        const auto& frame = replay_.frames(static_cast<int>(cursor_++));
        out_frame = VTX::Frame {};
        loader_.LoadFrame(frame, out_frame, "ArenaFrame");

        out_time = {frame.game_time(), std::nullopt, VTX::GameTime::EFilterType::OnlyIncreasing};

        return true;
    }

    size_t GetExpectedTotalFrames() const override { return total_; }

private:
    std::string filepath_;
    ::arena_pb::ArenaReplay replay_;
    VTX::GenericProtobufLoader loader_;
    size_t total_ = 0;
    size_t cursor_ = 0;
};

// ===================================================================
//  Data source 3 -- FlatBuffers
// ===================================================================
//  Reads the whole file into a contiguous buffer (required for FBS zero-copy
//  access), then walks the frame vector.  GenericFlatBufferLoader dispatches
//  to FlatBufferBinding<FrameData>::TransferToFrame() for each frame.
//  Field addresses come from PropertyAddressCache, keyed by entity_type_id.

class ArenaFbsDataSource : public VTX::IFrameDataSource {
public:
    ArenaFbsDataSource(std::string filepath, const VTX::PropertyAddressCache& cache)
        : filepath_(std::move(filepath))
        , loader_(cache, false) {}

    bool Initialize() override {
        std::ifstream ifs(filepath_, std::ios::binary | std::ios::ate);
        if (!ifs.is_open()) {
            VTX_ERROR("[FBS  ] Could not open: {}", filepath_);
            return false;
        }

        const size_t size = static_cast<size_t>(ifs.tellg());
        ifs.seekg(0);
        buffer_.resize(size);
        ifs.read(reinterpret_cast<char*>(buffer_.data()), static_cast<std::streamsize>(size));

        replay_ = ::arena_fb::GetArenaReplay(buffer_.data());
        if (!replay_ || !replay_->frames()) {
            VTX_ERROR("[FBS  ] Root table empty or missing: {}", filepath_);
            return false;
        }

        total_ = replay_->frames()->size();
        VTX_INFO("[FBS  ] Opened {} ({} frames)", filepath_, static_cast<int>(total_));
        return true;
    }

    bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
        if (cursor_ >= total_) {
            return false;
        }

        const auto* frame = replay_->frames()->Get(static_cast<flatbuffers::uoffset_t>(cursor_++));
        out_frame = VTX::Frame {};
        loader_.LoadFrame(frame, out_frame, "ArenaFrame");

        out_time = {frame->game_time(), std::nullopt, VTX::GameTime::EFilterType::OnlyIncreasing};

        return true;
    }

    size_t GetExpectedTotalFrames() const override { return total_; }

private:
    std::string filepath_;
    std::vector<uint8_t> buffer_;
    const ::arena_fb::ArenaReplay* replay_ = nullptr;
    VTX::GenericFlatBufferLoader loader_;
    size_t total_ = 0;
    size_t cursor_ = 0;
};

// ===================================================================
//  Pipeline driver -- initialise source, stream frames into the writer
// ===================================================================

using WriterFactoryFn = std::unique_ptr<VTX::IVtxWriterFacade> (*)(const VTX::WriterFacadeConfig&);

static bool RunPipeline(VTX::IFrameDataSource& source, const std::string& output_path, const std::string& schema_path,
                        const std::string& uuid, WriterFactoryFn factory) {
    if (!source.Initialize()) {
        VTX_ERROR("Data source Initialize() failed.");
        return false;
    }

    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath = output_path;
    cfg.schema_json_path = schema_path;
    cfg.replay_name = "Arena (advance_write)";
    cfg.replay_uuid = uuid;
    cfg.default_fps = 60.0f;
    cfg.chunk_max_frames = 500;
    cfg.use_compression = true;

    auto writer = factory(cfg);
    if (!writer) {
        VTX_ERROR("Writer creation failed for {}", output_path);
        return false;
    }

    VTX::Frame frame;
    VTX::GameTime::GameTimeRegister time;
    int count = 0;
    while (source.GetNextFrame(frame, time)) {
        writer->RecordFrame(frame, time);
        ++count;
    }
    writer->Flush();
    writer->Stop();

    VTX_INFO("  -> wrote {} frames to {}", count, output_path);
    return true;
}

int main() {
    const std::string schema = "content/writer/arena/arena_schema.json";
    const std::string writer_dir = "content/writer/arena";
    const std::string reader_dir = "content/reader/arena";

    VTX::SchemaRegistry arena_schema;
    if (!arena_schema.LoadFromJson(schema)) {
        VTX_ERROR("Failed to load schema: {}", schema);
        return 1;
    }

    VTX_INFO("=== advance_write - IFrameDataSource pattern ===");
    VTX_INFO("Demonstrates JSON, Protobuf and FlatBuffers sources using integration-style bindings.");

    VTX_INFO("--- 1. JSON data source ---");
    ArenaJsonDataSource json_ds(writer_dir + "/arena_replay_data.json");
    RunPipeline(json_ds, reader_dir + "/arena_from_json_ds.vtx", schema, "arena-adv-json-0001",
                VTX::CreateFlatBuffersWriterFacade);

    VTX_INFO("--- 2. Protobuf data source ---");
    ArenaProtoDataSource proto_ds(writer_dir + "/arena_replay_data.proto.bin", arena_schema);
    RunPipeline(proto_ds, reader_dir + "/arena_from_proto_ds.vtx", schema, "arena-adv-proto-0001",
                VTX::CreateProtobuffWriterFacade);

    VTX_INFO("--- 3. FlatBuffers data source ---");
    ArenaFbsDataSource fbs_ds(writer_dir + "/arena_replay_data.fbs.bin", arena_schema.GetPropertyCache());
    RunPipeline(fbs_ds, reader_dir + "/arena_from_fbs_ds.vtx", schema, "arena-adv-fbs-0001",
                VTX::CreateFlatBuffersWriterFacade);

    VTX_INFO("=== Complete ===");
    VTX_INFO("Outputs (content/reader/arena/):");
    VTX_INFO("  arena_from_json_ds.vtx   (from JSON source)");
    VTX_INFO("  arena_from_proto_ds.vtx  (from Protobuf source)");
    VTX_INFO("  arena_from_fbs_ds.vtx    (from FlatBuffers source)");

    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
