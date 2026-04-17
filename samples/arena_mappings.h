#pragma once
// arena_mappings.h -- Arena game data model + JSON mapping for the samples.
//
// Two concerns live here:
//
//   1. ArenaVec3 / ArenaQuat / ArenaPlayer / ArenaProjectile / ArenaMatchState /
//      ArenaFrame / ArenaReplayJson
//          C++ types that mirror the arena replay JSON structure.
//
//   2. VTX::JsonMapping<T> specializations for each of those types.
//          Same compile-time reflection pattern used by real integrations
//          (see tools/integrations/sf/sf_mappings.h).  Consumed by
//          VTX::UniversalDeserializer<>::Load<ArenaReplayJson>(JsonAdapter)
//          in advance_write.cpp.
//
//   3. ArenaToVtx::MapFrame()
//          Hand-written bridge from the arena data model to a VTX::Frame
//          (PropertyContainer layout matches content/writer/arena/arena_schema.json).
//          This is the "manual" counterpart to the schema-driven ProtoBinding
//          and FlatBufferBinding specializations declared in advance_write.cpp.
//
// Property-vector indices assigned here MUST match the field order in
// arena_schema.json -- see the comments above each MapXxx() function.

#include <cstdint>
#include <string>
#include <vector>

#include "vtx/common/adapters/json/json_policy.h"
#include "vtx/common/readers/frame_reader/type_traits.h"
#include "vtx/common/vtx_types.h"

// ===================================================================
//  Arena game data model (matches the JSON data source structure)
// ===================================================================

struct ArenaVec3  { double x = 0, y = 0, z = 0; };
struct ArenaQuat  { float  x = 0, y = 0, z = 0, w = 1; };

struct ArenaPlayer {
    std::string unique_id;
    std::string name;
    int         team     = 0;
    float       health   = 100.0f;
    float       armor    = 50.0f;
    ArenaVec3   position;
    ArenaQuat   rotation;
    ArenaVec3   velocity;
    bool        is_alive = true;
    int         score    = 0;
    int         deaths   = 0;
};

struct ArenaProjectile {
    std::string unique_id;
    std::string owner_id;
    ArenaVec3   position;
    ArenaVec3   velocity;
    float       damage = 25.0f;
    std::string type   = "bullet";
};

struct ArenaMatchState {
    int         score_team1    = 0;
    int         score_team2    = 0;
    int         round          = 1;
    std::string phase;
    float       time_remaining = 0.0f;
};

struct ArenaFrame {
    int                          frame_index = 0;
    float                        game_time   = 0.0f;
    int64_t                      utc_ticks   = 0;
    std::vector<ArenaPlayer>     players;
    std::vector<ArenaProjectile> projectiles;
    ArenaMatchState              match_state;
};

struct ArenaReplayJson {
    std::string                replay_name;
    int                        total_frames     = 0;
    int                        fps              = 60;
    double                     duration_seconds = 0.0;
    std::vector<ArenaFrame>    frames;
};

// ===================================================================
//  JsonMapping<T> specializations  (JSON key → C++ member)
//
//  Same pattern as sf_mappings.h.  Consumed by UniversalDeserializer
//  or any code that inspects the mapping tuple at compile time.
// ===================================================================

template <>
struct VTX::JsonMapping<ArenaVec3> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("x", &ArenaVec3::x),
            MakeField("y", &ArenaVec3::y),
            MakeField("z", &ArenaVec3::z)
        );
    }
};

template <>
struct VTX::JsonMapping<ArenaQuat> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("x", &ArenaQuat::x),
            MakeField("y", &ArenaQuat::y),
            MakeField("z", &ArenaQuat::z),
            MakeField("w", &ArenaQuat::w)
        );
    }
};

template <>
struct VTX::JsonMapping<ArenaPlayer> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("unique_id", &ArenaPlayer::unique_id),
            MakeField("name",      &ArenaPlayer::name),
            MakeField("team",      &ArenaPlayer::team),
            MakeField("health",    &ArenaPlayer::health),
            MakeField("armor",     &ArenaPlayer::armor),
            MakeField("position",  &ArenaPlayer::position),
            MakeField("rotation",  &ArenaPlayer::rotation),
            MakeField("velocity",  &ArenaPlayer::velocity),
            MakeField("is_alive",  &ArenaPlayer::is_alive),
            MakeField("score",     &ArenaPlayer::score),
            MakeField("deaths",    &ArenaPlayer::deaths)
        );
    }
};

template <>
struct VTX::JsonMapping<ArenaProjectile> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("unique_id", &ArenaProjectile::unique_id),
            MakeField("owner_id",  &ArenaProjectile::owner_id),
            MakeField("position",  &ArenaProjectile::position),
            MakeField("velocity",  &ArenaProjectile::velocity),
            MakeField("damage",    &ArenaProjectile::damage),
            MakeField("type",      &ArenaProjectile::type)
        );
    }
};

template <>
struct VTX::JsonMapping<ArenaMatchState> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("score_team1",    &ArenaMatchState::score_team1),
            MakeField("score_team2",    &ArenaMatchState::score_team2),
            MakeField("round",          &ArenaMatchState::round),
            MakeField("phase",          &ArenaMatchState::phase),
            MakeField("time_remaining", &ArenaMatchState::time_remaining)
        );
    }
};

template <>
struct VTX::JsonMapping<ArenaFrame> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("frame_index",  &ArenaFrame::frame_index),
            MakeField("game_time",    &ArenaFrame::game_time),
            MakeField("utc_ticks",    &ArenaFrame::utc_ticks),
            MakeField("players",      &ArenaFrame::players),
            MakeField("projectiles",  &ArenaFrame::projectiles),
            MakeField("match_state",  &ArenaFrame::match_state)
        );
    }
};

template <>
struct VTX::JsonMapping<ArenaReplayJson> {
    static constexpr auto GetFields() {
        return std::make_tuple(
            MakeField("replay_name",      &ArenaReplayJson::replay_name),
            MakeField("total_frames",     &ArenaReplayJson::total_frames),
            MakeField("fps",              &ArenaReplayJson::fps),
            MakeField("duration_seconds", &ArenaReplayJson::duration_seconds),
            MakeField("frames",           &ArenaReplayJson::frames)
        );
    }
};

// ===================================================================
//  ArenaToVtx — arena game types → VTX PropertyContainer
//
//  Property indices must match the field order in arena_schema.json.
// ===================================================================

namespace ArenaToVtx {

inline VTX::Vector ToVtxVector(const ArenaVec3& v) { return {v.x, v.y, v.z}; }
inline VTX::Quat   ToVtxQuat(const ArenaQuat& q)   { return {q.x, q.y, q.z, q.w}; }

/// Player → entity_type_id 0
///   string[0]=UniqueID  string[1]=Name
///   int32[0]=Team       int32[1]=Score   int32[2]=Deaths
///   float[0]=Health     float[1]=Armor
///   vector[0]=Position  vector[1]=Velocity
///   quat[0]=Rotation
///   bool[0]=IsAlive
inline VTX::PropertyContainer MapPlayer(const ArenaPlayer& p) {
    VTX::PropertyContainer pc;
    pc.entity_type_id    = 0;
    pc.string_properties = { p.unique_id, p.name };
    pc.int32_properties  = { p.team, p.score, p.deaths };
    pc.float_properties  = { p.health, p.armor };
    pc.vector_properties = { ToVtxVector(p.position), ToVtxVector(p.velocity) };
    pc.quat_properties   = { ToVtxQuat(p.rotation) };
    pc.bool_properties   = { p.is_alive };
    return pc;
}

/// Projectile → entity_type_id 1
///   string[0]=UniqueID  string[1]=OwnerID  string[2]=Type
///   vector[0]=Position  vector[1]=Velocity
///   float[0]=Damage
inline VTX::PropertyContainer MapProjectile(const ArenaProjectile& pr) {
    VTX::PropertyContainer pc;
    pc.entity_type_id    = 1;
    pc.string_properties = { pr.unique_id, pr.owner_id, pr.type };
    pc.vector_properties = { ToVtxVector(pr.position), ToVtxVector(pr.velocity) };
    pc.float_properties  = { pr.damage };
    return pc;
}

/// MatchState → entity_type_id 2
///   string[0]=UniqueID  string[1]=Phase
///   int32[0]=ScoreTeam1  int32[1]=ScoreTeam2  int32[2]=Round
///   float[0]=TimeRemaining
inline VTX::PropertyContainer MapMatchState(const ArenaMatchState& m) {
    VTX::PropertyContainer pc;
    pc.entity_type_id    = 2;
    pc.string_properties = { "match_001", m.phase };
    pc.int32_properties  = { m.score_team1, m.score_team2, m.round };
    pc.float_properties  = { m.time_remaining };
    return pc;
}

/// Full frame → single "entity" bucket.
inline VTX::Frame MapFrame(const ArenaFrame& af) {
    VTX::Frame frame;
    VTX::Bucket& bucket = frame.CreateBucket("entity");
    for (const auto& p  : af.players)     { bucket.unique_ids.push_back(p.unique_id);  bucket.entities.push_back(MapPlayer(p)); }
    for (const auto& pr : af.projectiles) { bucket.unique_ids.push_back(pr.unique_id); bucket.entities.push_back(MapProjectile(pr)); }
    bucket.unique_ids.push_back("match_001");
    bucket.entities.push_back(MapMatchState(af.match_state));
    return frame;
}

} // namespace ArenaToVtx
