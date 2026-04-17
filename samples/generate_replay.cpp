// generate_replay.cpp -- Arena sample: data-source producer.
//
// Stage 1 of the two-stage arena sample pipeline:
//
//     generate_replay.cpp   (this file)
//         |
//         |  1. Simulate a 5v5 arena match (60 s @ 60 FPS = 3600 frames)
//         |  2. Serialize the simulation to 3 raw data-source files:
//         |       content/writer/arena/arena_replay_data.json       (JSON)
//         |       content/writer/arena/arena_replay_data.proto.bin  (Protobuf)
//         |       content/writer/arena/arena_replay_data.fbs.bin    (FlatBuffers)
//         v
//     advance_write.cpp     (next stage)
//         |  Reads the 3 data sources back through the SDK's integration
//         |  primitives (JsonMapping + UniversalDeserializer / ProtoBinding /
//         |  FlatBufferBinding) and wraps them in IFrameDataSource adapters
//         |  that feed the VTX writer, producing the .vtx replays.
//         v
//     content/reader/arena/arena_from_{json,proto,fbs}_ds.vtx
//
// The split mirrors a real game pipeline: a game server emits raw telemetry
// in its own format, and a separate conversion tool ingests it into .vtx.
//
// Build & run from the samples/ directory (working directory matters because
// the output paths are relative):
//
//   cmake --build build --target vtx_sample_generate
//   ./vtx_sample_generate

// ---- VTX SDK ----
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"

// ---- Arena mappings (JSON data model + VTX mapping) ----
#include "arena_mappings.h"

// ---- Arena generated code (from samples/schemas/) ----
#include "arena_data.pb.h"
#include "arena_data_generated.h"

// ---- Standard library ----
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---- JSON (transitive via vtx_common) ----
#include <nlohmann/json.hpp>

// ---- FlatBuffers runtime ----
#include <flatbuffers/flatbuffers.h>


// ===================================================================
//  SECTION 1 — Constants
// ===================================================================

static constexpr int     TOTAL_FRAMES       = 3600;
static constexpr float   FPS                = 60.0f;
static constexpr float   DT                 = 1.0f / 60.0f;
static constexpr int     NUM_PLAYERS        = 10;          // 5v5
static constexpr double  ARENA_MIN          = -50.0;
static constexpr double  ARENA_MAX          =  50.0;
static constexpr int     RESPAWN_FRAMES     = 180;         // 3 s

static constexpr int64_t UTC_TICKS_PER_FRAME = 166'666LL;  // 10^7 / 60

// Phase boundaries.
static constexpr int WARMUP_END  = 300;
static constexpr int PLAYING_END = 3300;

// Combat cadence (active during "playing" only).
static constexpr int PROJ_INTERVAL  = 300;
static constexpr int PROJ_OFFSET    = 150;
static constexpr int KILL_INTERVAL  = 600;
static constexpr int KILL_OFFSET    = 300;
static constexpr int KILL_MIN_FRAME = 600;
static constexpr int PROJ_LIFETIME  = 90;


// ===================================================================
//  SECTION 2 — Arena simulation types (the game's own data model)
// ===================================================================

struct PlayerSim {
    std::string unique_id;
    std::string name;
    int   team = 1;
    float health = 100.0f, armor = 50.0f;
    double pos_x = 0, pos_y = 0, pos_z = 0;
    double vel_x = 0, vel_y = 0, vel_z = 0;
    float  rot_x = 0, rot_y = 0, rot_z = 0, rot_w = 1;
    bool  is_alive = true;
    int   score = 0, deaths = 0, respawn_timer = 0;
    // Movement params (set once at init).
    double base_x = 0, base_z = 0;
    double freq_x = 0, freq_z = 0;
    double phase_x = 0, phase_z = 0;
};

struct ProjectileSim {
    std::string unique_id, owner_id;
    double pos_x = 0, pos_y = 0, pos_z = 0;
    double vel_x = 0, vel_y = 0, vel_z = 0;
    float  damage = 25.0f;
    std::string type = "bullet";
    int lifetime = PROJ_LIFETIME;
};

struct MatchSim {
    int score_team1 = 0, score_team2 = 0, round = 1;
    std::string phase = "warmup";
    float time_remaining = 0.0f;
};

struct FrameSnapshot {
    std::vector<PlayerSim>     players;
    std::vector<ProjectileSim> projectiles;
    MatchSim                   match;
    float   game_time = 0.0f;
    int64_t utc_ticks = 0;
};


// ===================================================================
//  SECTION 3 — Simulation
// ===================================================================

static int64_t GetUtcNowTicks() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ns / 100;
}

static std::vector<FrameSnapshot> RunSimulation(int64_t base_utc)
{
    static const char* const kNames[NUM_PLAYERS] = {
        "Alpha","Bravo","Charlie","Delta","Echo",
        "Foxtrot","Golf","Hotel","India","Juliet"
    };

    uint64_t rng = 42;
    auto rand_d = [&](double lo, double hi) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        double t = static_cast<double>(rng >> 33) / static_cast<double>(1ULL << 31);
        return lo + t * (hi - lo);
    };

    std::vector<PlayerSim> players(NUM_PLAYERS);
    for (int i = 0; i < NUM_PLAYERS; ++i) {
        auto& p = players[i];
        p.unique_id = "player_" + std::to_string(i);
        p.name  = kNames[i];
        p.team  = (i < 5) ? 1 : 2;
        p.base_x = ARENA_MIN + (ARENA_MAX - ARENA_MIN) * (i + 0.5) / NUM_PLAYERS;
        p.base_z = (p.team == 1) ? -20.0 : 20.0;
        p.pos_x  = p.base_x;
        p.pos_z  = p.base_z;
        p.freq_x = rand_d(0.3, 1.2);  p.freq_z = rand_d(0.3, 1.2);
        p.phase_x = rand_d(0.0, 6.28); p.phase_z = rand_d(0.0, 6.28);
    }

    MatchSim match;
    std::vector<ProjectileSim> projectiles;
    int next_proj_id = 0;

    std::vector<FrameSnapshot> frames;
    frames.reserve(TOTAL_FRAMES);

    for (int f = 0; f < TOTAL_FRAMES; ++f) {
        const float t = static_cast<float>(f) * DT;

        // Phase.
        if      (f < WARMUP_END)  { match.phase = "warmup";   match.time_remaining = (WARMUP_END  - f) * DT; }
        else if (f < PLAYING_END) { match.phase = "playing";  match.time_remaining = (PLAYING_END - f) * DT; }
        else                      { match.phase = "roundend"; match.time_remaining = (TOTAL_FRAMES- f) * DT; }

        // Movement.
        for (auto& p : players) {
            if (!p.is_alive) {
                if (--p.respawn_timer <= 0) { p.is_alive = true; p.health = 100; p.armor = 50; p.pos_x = p.base_x; p.pos_z = p.base_z; }
                p.vel_x = p.vel_y = p.vel_z = 0; continue;
            }
            double px = p.pos_x, pz = p.pos_z;
            p.pos_x = std::clamp(p.base_x + 15.0 * std::sin(p.freq_x * t + p.phase_x), ARENA_MIN, ARENA_MAX);
            p.pos_z = std::clamp(p.base_z + 10.0 * std::sin(p.freq_z * t + p.phase_z), ARENA_MIN, ARENA_MAX);
            p.vel_x = (p.pos_x - px) / DT;  p.vel_z = (p.pos_z - pz) / DT;  p.vel_y = 0;
            if (std::abs(p.vel_x) > 0.001 || std::abs(p.vel_z) > 0.001) {
                float yaw = std::atan2(float(p.vel_x), float(p.vel_z));
                p.rot_y = std::sin(yaw * 0.5f); p.rot_w = std::cos(yaw * 0.5f); p.rot_x = p.rot_z = 0;
            }
        }

        // Combat.
        if (match.phase == "playing") {
            if (f % PROJ_INTERVAL == PROJ_OFFSET) {
                int si = (f / PROJ_INTERVAL) % NUM_PLAYERS;
                if (players[si].is_alive) {
                    ProjectileSim proj;
                    proj.unique_id = "proj_" + std::to_string(next_proj_id++);
                    proj.owner_id = players[si].unique_id;
                    proj.pos_x = players[si].pos_x; proj.pos_y = 1.0; proj.pos_z = players[si].pos_z;
                    proj.vel_z = (players[si].team == 1) ? 30.0 : -30.0;
                    projectiles.push_back(std::move(proj));
                }
            }
            if (f % KILL_INTERVAL == KILL_OFFSET && f >= KILL_MIN_FRAME) {
                int ki = ((f / KILL_INTERVAL) * 3) % NUM_PLAYERS;
                int vi = (ki + 5) % NUM_PLAYERS;
                if (players[ki].is_alive && players[vi].is_alive) {
                    players[vi].health = 0; players[vi].armor = 0; players[vi].is_alive = false;
                    players[vi].respawn_timer = RESPAWN_FRAMES; players[vi].deaths++;
                    players[ki].score++;
                    if (players[ki].team == 1) match.score_team1++; else match.score_team2++;
                }
            }
        }

        // Advance projectiles.
        for (auto& pr : projectiles) { pr.pos_x += pr.vel_x*DT; pr.pos_y += pr.vel_y*DT; pr.pos_z += pr.vel_z*DT; pr.lifetime--; }
        std::erase_if(projectiles, [](auto& p){ return p.lifetime <= 0; });

        FrameSnapshot snap;
        snap.players = players; snap.projectiles = projectiles; snap.match = match;
        snap.game_time = t;
        snap.utc_ticks = base_utc + int64_t(f) * UTC_TICKS_PER_FRAME;
        frames.push_back(std::move(snap));
    }
    return frames;
}


// ===================================================================
//  SECTION 4 — Data source EXPORT (simulation → 3 file formats)
// ===================================================================

// ---- 4a: JSON ----
static void ExportJsonSource(const std::vector<FrameSnapshot>& frames, const std::string& path)
{
    nlohmann::json root;
    root["replay_name"]      = "Arena Sample Replay";
    root["total_frames"]     = TOTAL_FRAMES;
    root["fps"]              = int(FPS);
    root["duration_seconds"] = double(TOTAL_FRAMES) / FPS;

    auto& jf = root["frames"];  jf = nlohmann::json::array();
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = frames[i];
        nlohmann::json f;
        f["frame_index"] = i;  f["game_time"] = s.game_time;  f["utc_ticks"] = s.utc_ticks;

        f["players"] = nlohmann::json::array();
        for (const auto& p : s.players)
            f["players"].push_back({{"unique_id",p.unique_id},{"name",p.name},{"team",p.team},
                {"health",p.health},{"armor",p.armor},
                {"position",{{"x",p.pos_x},{"y",p.pos_y},{"z",p.pos_z}}},
                {"rotation",{{"x",p.rot_x},{"y",p.rot_y},{"z",p.rot_z},{"w",p.rot_w}}},
                {"velocity",{{"x",p.vel_x},{"y",p.vel_y},{"z",p.vel_z}}},
                {"is_alive",p.is_alive},{"score",p.score},{"deaths",p.deaths}});

        f["projectiles"] = nlohmann::json::array();
        for (const auto& pr : s.projectiles)
            f["projectiles"].push_back({{"unique_id",pr.unique_id},{"owner_id",pr.owner_id},
                {"position",{{"x",pr.pos_x},{"y",pr.pos_y},{"z",pr.pos_z}}},
                {"velocity",{{"x",pr.vel_x},{"y",pr.vel_y},{"z",pr.vel_z}}},
                {"damage",pr.damage},{"type",pr.type}});

        f["match_state"] = {{"score_team1",s.match.score_team1},{"score_team2",s.match.score_team2},
            {"round",s.match.round},{"phase",s.match.phase},{"time_remaining",s.match.time_remaining}};
        jf.push_back(std::move(f));
    }
    std::ofstream(path) << root.dump(2);
}

// ---- 4b: Protobuf binary ----
static void ExportProtoSource(const std::vector<FrameSnapshot>& frames, const std::string& path)
{
    ::arena_pb::ArenaReplay replay;
    replay.set_replay_name("Arena Sample Replay");
    replay.set_total_frames(TOTAL_FRAMES);
    replay.set_fps(int(FPS));
    replay.set_duration_seconds(double(TOTAL_FRAMES) / FPS);

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = frames[i];
        auto* fd = replay.add_frames();
        fd->set_frame_index(int(i));
        fd->set_game_time(s.game_time);
        fd->set_utc_ticks(s.utc_ticks);

        for (const auto& p : s.players) {
            auto* pp = fd->add_players();
            pp->set_unique_id(p.unique_id);  pp->set_name(p.name);  pp->set_team(p.team);
            pp->set_health(p.health);        pp->set_armor(p.armor);
            auto* pos = pp->mutable_position(); pos->set_x(p.pos_x); pos->set_y(p.pos_y); pos->set_z(p.pos_z);
            auto* rot = pp->mutable_rotation(); rot->set_x(p.rot_x); rot->set_y(p.rot_y); rot->set_z(p.rot_z); rot->set_w(p.rot_w);
            auto* vel = pp->mutable_velocity(); vel->set_x(p.vel_x); vel->set_y(p.vel_y); vel->set_z(p.vel_z);
            pp->set_is_alive(p.is_alive);    pp->set_score(p.score);  pp->set_deaths(p.deaths);
        }
        for (const auto& pr : s.projectiles) {
            auto* pp = fd->add_projectiles();
            pp->set_unique_id(pr.unique_id);  pp->set_owner_id(pr.owner_id);
            auto* pos = pp->mutable_position(); pos->set_x(pr.pos_x); pos->set_y(pr.pos_y); pos->set_z(pr.pos_z);
            auto* vel = pp->mutable_velocity(); vel->set_x(pr.vel_x); vel->set_y(pr.vel_y); vel->set_z(pr.vel_z);
            pp->set_damage(pr.damage);  pp->set_type(pr.type);
        }
        auto* ms = fd->mutable_match_state();
        ms->set_score_team1(s.match.score_team1);  ms->set_score_team2(s.match.score_team2);
        ms->set_round(s.match.round);  ms->set_phase(s.match.phase);  ms->set_time_remaining(s.match.time_remaining);
    }

    std::ofstream ofs(path, std::ios::binary);
    replay.SerializeToOstream(&ofs);
}

// ---- 4c: FlatBuffers binary ----
static void ExportFbsSource(const std::vector<FrameSnapshot>& frames, const std::string& path)
{
    // Build using the Object API (ArenaReplayT, FrameDataT, PlayerT, ...).
    ::arena_fb::ArenaReplayT replay;
    replay.replay_name      = "Arena Sample Replay";
    replay.total_frames     = TOTAL_FRAMES;
    replay.fps              = int(FPS);
    replay.duration_seconds = double(TOTAL_FRAMES) / FPS;

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = frames[i];
        auto fd = std::make_unique<::arena_fb::FrameDataT>();
        fd->frame_index = int(i);
        fd->game_time   = s.game_time;
        fd->utc_ticks   = s.utc_ticks;

        for (const auto& p : s.players) {
            auto pp = std::make_unique<::arena_fb::PlayerT>();
            pp->unique_id = p.unique_id;  pp->name = p.name;  pp->team = p.team;
            pp->health = p.health;        pp->armor = p.armor;
            pp->position = std::make_unique<::arena_fb::Vec3>(p.pos_x, p.pos_y, p.pos_z);
            pp->rotation = std::make_unique<::arena_fb::Rotation>(p.rot_x, p.rot_y, p.rot_z, p.rot_w);
            pp->velocity = std::make_unique<::arena_fb::Vec3>(p.vel_x, p.vel_y, p.vel_z);
            pp->is_alive = p.is_alive;    pp->score = p.score;  pp->deaths = p.deaths;
            fd->players.push_back(std::move(pp));
        }
        for (const auto& pr : s.projectiles) {
            auto pp = std::make_unique<::arena_fb::ProjectileT>();
            pp->unique_id = pr.unique_id;  pp->owner_id = pr.owner_id;
            pp->position = std::make_unique<::arena_fb::Vec3>(pr.pos_x, pr.pos_y, pr.pos_z);
            pp->velocity = std::make_unique<::arena_fb::Vec3>(pr.vel_x, pr.vel_y, pr.vel_z);
            pp->damage = pr.damage;  pp->type = pr.type;
            fd->projectiles.push_back(std::move(pp));
        }
        auto ms = std::make_unique<::arena_fb::MatchStateT>();
        ms->score_team1 = s.match.score_team1;  ms->score_team2 = s.match.score_team2;
        ms->round = s.match.round;  ms->phase = s.match.phase;  ms->time_remaining = s.match.time_remaining;
        fd->match_state = std::move(ms);

        replay.frames.push_back(std::move(fd));
    }

    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(::arena_fb::ArenaReplay::Pack(fbb, &replay));

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
}


// ===================================================================
int main()
{
    const std::string schema_path   = "content/writer/arena/arena_schema.json";
    const std::string writer_dir    = "content/writer/arena";
    const std::string reader_dir    = "content/reader/arena";

    std::filesystem::create_directories(writer_dir);
    std::filesystem::create_directories(reader_dir);

    VTX_INFO("=== Arena Replay Generator ===");
    VTX_INFO("Simulating {} frames ({:.1f}s @ {:.0f} FPS)...", TOTAL_FRAMES, double(TOTAL_FRAMES)/FPS, FPS);

    // ---- Phase 1: Simulate ----
    // Use a fixed historical timestamp so the data is reproducible across
    // runs.  The writer accepts any strictly-increasing UTC sequence now.
    const int64_t base_utc = 1'745'000'000LL * 10'000'000LL; // 2025-04-19 UTC
    auto sim_frames = RunSimulation(base_utc);
    VTX_INFO("Simulation complete: {} frames, {} players.", int(sim_frames.size()), NUM_PLAYERS);

    // ---- Phase 2: Export 3 data-source files ----
    const std::string json_src  = writer_dir + "/arena_replay_data.json";
    const std::string proto_src = writer_dir + "/arena_replay_data.proto.bin";
    const std::string fbs_src   = writer_dir + "/arena_replay_data.fbs.bin";

    ExportJsonSource(sim_frames, json_src);
    VTX_INFO("Exported data source: {}", json_src);

    ExportProtoSource(sim_frames, proto_src);
    VTX_INFO("Exported data source: {}", proto_src);

    ExportFbsSource(sim_frames, fbs_src);
    VTX_INFO("Exported data source: {}", fbs_src);


    // ---- Summary ----
    VTX_INFO("=== Generation Complete ===");
    VTX_INFO("  Data sources (content/writer/arena/):");
    VTX_INFO("    arena_replay_data.json       (JSON)");
    VTX_INFO("    arena_replay_data.proto.bin   (Protobuf)");
    VTX_INFO("    arena_replay_data.fbs.bin     (FlatBuffers)");


    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
