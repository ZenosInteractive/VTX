#pragma once
// arena_mappings.h -- Arena game data model + mappings (JSON ingest + VTX slot output).
//
// Two layers of compile-time reflection live here:
//
//   1. VTX::JsonMapping<T>
//        Drives UniversalDeserializer<>::Load<ArenaReplayJson>(JsonAdapter)
//        in advance_write.cpp. Walks JSON -> C++ struct.
//
//   2. VTX::StructMapping<T> + VTX::StructFrameBinding<ArenaFrame>
//        Drives GenericNativeLoader::Load / LoadFrame in advance_write.cpp.
//        Walks C++ struct -> VTX::PropertyContainer / VTX::Frame.
//
// The C++ data model uses VTX::Vector / VTX::Quat directly so no per-field
// conversion is needed between the JSON ingest layer and the VTX output layer.
// (This is the equivalent of the manual ArenaToVtx::Map* helpers we deleted --
//  the schema-driven StructMapping<> now does that work automatically.)

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "vtx/common/adapters/json/json_policy.h"
#include "vtx/common/adapters/native/struct_mapping.h"
#include "vtx/common/readers/frame_reader/native_loader.h"
#include "vtx/common/readers/frame_reader/type_traits.h"
#include "vtx/common/vtx_types.h"

#include "arena_generated.h"

// ===================================================================
//  Arena game data model (matches the JSON data source structure)
// ===================================================================

struct ArenaPlayer {
    std::string unique_id;
    std::string name;
    int team = 0;
    float health = 100.0f;
    float armor = 50.0f;
    VTX::Vector position;
    VTX::Quat rotation;
    VTX::Vector velocity;
    bool is_alive = true;
    int score = 0;
    int deaths = 0;
};

struct ArenaProjectile {
    std::string unique_id;
    std::string owner_id;
    VTX::Vector position;
    VTX::Vector velocity;
    float damage = 25.0f;
    std::string type = "bullet";
};

struct ArenaMatchState {
    // Default to the canonical id. The match-state JSON doesn't carry one;
    // the FB/Proto bindings inject the same literal -- this keeps parity.
    std::string unique_id = "match_001";
    int score_team1 = 0;
    int score_team2 = 0;
    int round = 1;
    std::string phase;
    float time_remaining = 0.0f;
};

struct ArenaFrame {
    int frame_index = 0;
    float game_time = 0.0f;
    int64_t utc_ticks = 0;
    std::vector<ArenaPlayer> players;
    std::vector<ArenaProjectile> projectiles;
    ArenaMatchState match_state;
};

struct ArenaReplayJson {
    std::string replay_name;
    int total_frames = 0;
    int fps = 60;
    double duration_seconds = 0.0;
    std::vector<ArenaFrame> frames;
};

// ===================================================================
//  JsonMapping<T> specializations  (JSON key -> C++ member)
// ===================================================================
//  Two new mappings for the VTX math types so the JSON ingest layer can fill
//  them directly. Same shape as the JSON file: {"x":..., "y":..., "z":...}.

template <>
struct VTX::JsonMapping<VTX::Vector> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeField("x", &VTX::Vector::x), MakeField("y", &VTX::Vector::y),
                               MakeField("z", &VTX::Vector::z));
    }
};

template <>
struct VTX::JsonMapping<VTX::Quat> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeField("x", &VTX::Quat::x), MakeField("y", &VTX::Quat::y),
                               MakeField("z", &VTX::Quat::z), MakeField("w", &VTX::Quat::w));
    }
};

template <>
struct VTX::JsonMapping<ArenaPlayer> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeField("unique_id", &ArenaPlayer::unique_id), MakeField("name", &ArenaPlayer::name),
                               MakeField("team", &ArenaPlayer::team), MakeField("health", &ArenaPlayer::health),
                               MakeField("armor", &ArenaPlayer::armor), MakeField("position", &ArenaPlayer::position),
                               MakeField("rotation", &ArenaPlayer::rotation),
                               MakeField("velocity", &ArenaPlayer::velocity),
                               MakeField("is_alive", &ArenaPlayer::is_alive), MakeField("score", &ArenaPlayer::score),
                               MakeField("deaths", &ArenaPlayer::deaths));
    }
};

template <>
struct VTX::JsonMapping<ArenaProjectile> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("unique_id", &ArenaProjectile::unique_id), MakeField("owner_id", &ArenaProjectile::owner_id),
            MakeField("position", &ArenaProjectile::position), MakeField("velocity", &ArenaProjectile::velocity),
            MakeField("damage", &ArenaProjectile::damage), MakeField("type", &ArenaProjectile::type));
    }
};

template <>
struct VTX::JsonMapping<ArenaMatchState> {
    // unique_id is intentionally NOT in this mapping -- it isn't in the JSON file.
    // The C++ default ("match_001") survives because UniversalDeserializer
    // skips missing keys.
    static constexpr auto GetFields() {
        return std::make_tuple(MakeField("score_team1", &ArenaMatchState::score_team1),
                               MakeField("score_team2", &ArenaMatchState::score_team2),
                               MakeField("round", &ArenaMatchState::round), MakeField("phase", &ArenaMatchState::phase),
                               MakeField("time_remaining", &ArenaMatchState::time_remaining));
    }
};

template <>
struct VTX::JsonMapping<ArenaFrame> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("frame_index", &ArenaFrame::frame_index), MakeField("game_time", &ArenaFrame::game_time),
            MakeField("utc_ticks", &ArenaFrame::utc_ticks), MakeField("players", &ArenaFrame::players),
            MakeField("projectiles", &ArenaFrame::projectiles), MakeField("match_state", &ArenaFrame::match_state));
    }
};

template <>
struct VTX::JsonMapping<ArenaReplayJson> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeField("replay_name", &ArenaReplayJson::replay_name),
                               MakeField("total_frames", &ArenaReplayJson::total_frames),
                               MakeField("fps", &ArenaReplayJson::fps),
                               MakeField("duration_seconds", &ArenaReplayJson::duration_seconds),
                               MakeField("frames", &ArenaReplayJson::frames));
    }
};

// ===================================================================
//  StructMapping<T> specializations  (C++ member -> VTX slot name)
// ===================================================================
//  Replaces the hand-written ArenaToVtx::MapPlayer / MapProjectile /
//  MapMatchState helpers from the previous version. GenericNativeLoader
//  walks these tuples automatically.

template <>
struct VTX::StructMapping<ArenaPlayer> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeStructField(ArenaSchema::Player::UniqueID, &ArenaPlayer::unique_id),
                               MakeStructField(ArenaSchema::Player::Name, &ArenaPlayer::name),
                               MakeStructField(ArenaSchema::Player::Team, &ArenaPlayer::team),
                               MakeStructField(ArenaSchema::Player::Health, &ArenaPlayer::health),
                               MakeStructField(ArenaSchema::Player::Armor, &ArenaPlayer::armor),
                               MakeStructField(ArenaSchema::Player::Position, &ArenaPlayer::position),
                               MakeStructField(ArenaSchema::Player::Rotation, &ArenaPlayer::rotation),
                               MakeStructField(ArenaSchema::Player::Velocity, &ArenaPlayer::velocity),
                               MakeStructField(ArenaSchema::Player::IsAlive, &ArenaPlayer::is_alive),
                               MakeStructField(ArenaSchema::Player::Score, &ArenaPlayer::score),
                               MakeStructField(ArenaSchema::Player::Deaths, &ArenaPlayer::deaths));
    }
};

template <>
struct VTX::StructMapping<ArenaProjectile> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeStructField(ArenaSchema::Projectile::UniqueID, &ArenaProjectile::unique_id),
                               MakeStructField(ArenaSchema::Projectile::OwnerID, &ArenaProjectile::owner_id),
                               MakeStructField(ArenaSchema::Projectile::Position, &ArenaProjectile::position),
                               MakeStructField(ArenaSchema::Projectile::Velocity, &ArenaProjectile::velocity),
                               MakeStructField(ArenaSchema::Projectile::Damage, &ArenaProjectile::damage),
                               MakeStructField(ArenaSchema::Projectile::Type, &ArenaProjectile::type));
    }
};

template <>
struct VTX::StructMapping<ArenaMatchState> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeStructField(ArenaSchema::MatchState::UniqueID, &ArenaMatchState::unique_id),
            MakeStructField(ArenaSchema::MatchState::ScoreTeam1, &ArenaMatchState::score_team1),
            MakeStructField(ArenaSchema::MatchState::ScoreTeam2, &ArenaMatchState::score_team2),
            MakeStructField(ArenaSchema::MatchState::Round, &ArenaMatchState::round),
            MakeStructField(ArenaSchema::MatchState::Phase, &ArenaMatchState::phase),
            MakeStructField(ArenaSchema::MatchState::TimeRemaining, &ArenaMatchState::time_remaining));
    }
};

// ===================================================================
//  StructFrameBinding<ArenaFrame>  (ArenaFrame -> VTX::Frame buckets)
// ===================================================================
//  Same pattern as FlatBufferBinding<arena_fb::FrameData>::TransferToFrame
//  and ProtoBinding<arena_pb::FrameData>::TransferToFrame in advance_write.cpp.
//  The bucket/entity layout is game-specific; what gets pushed into each
//  PropertyContainer is driven automatically by StructMapping<> above.

template <>
struct VTX::StructFrameBinding<ArenaFrame> {
    static void TransferToFrame(const ArenaFrame& src, VTX::Frame& dest, VTX::GenericNativeLoader& loader,
                                const std::string& /*schema_name*/) {
        dest = VTX::Frame {};
        VTX::Bucket& bucket = dest.GetBucket("entity");
        bucket.entities.clear();
        bucket.unique_ids.clear();

        loader.AppendActorList(bucket, VTX::ArenaSchema::Player::StructName, src.players,
                               [](const ArenaPlayer& p) { return p.unique_id; });

        loader.AppendActorList(bucket, VTX::ArenaSchema::Projectile::StructName, src.projectiles,
                               [](const ArenaProjectile& p) { return p.unique_id; });

        loader.AppendSingleEntity(bucket, VTX::ArenaSchema::MatchState::StructName, src.match_state,
                                  [](const ArenaMatchState& m) { return m.unique_id; });
    }
};
