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

static constexpr int TOTAL_FRAMES = 3600;
static constexpr float FPS = 60.0f;
static constexpr float DT = 1.0f / 60.0f;
static constexpr int NUM_PLAYERS = 10; // 5v5
static constexpr double ARENA_MIN = -50.0;
static constexpr double ARENA_MAX = 50.0;
static constexpr int RESPAWN_FRAMES = 180; // 3 s

static constexpr int64_t UTC_TICKS_PER_FRAME = 166'666LL; // 10^7 / 60

// Phase boundaries.
static constexpr int WARMUP_END = 300;
static constexpr int PLAYING_END = 3300;

// Combat cadence (active during "playing" only).
static constexpr int PROJ_INTERVAL = 300;
static constexpr int PROJ_OFFSET = 150;
static constexpr int KILL_INTERVAL = 600;
static constexpr int KILL_OFFSET = 300;
static constexpr int KILL_MIN_FRAME = 600;
static constexpr int PROJ_LIFETIME = 90;


// ===================================================================
//  SECTION 2 — Arena simulation types (the game's own data model)
// ===================================================================

// ---- Nested container element types (the "rich" data) ----

// One row of a player's inventory -> mapped to an array of nested VTX structs.
struct InventoryItemSim {
    std::string item_id;
    std::string display_name;
    int quantity = 1;
    float durability = 100.0f;
    int slot = 0;
};

// One weapon's ammo -> mapped to a VTX Map<weapon, AmmoEntry> entry, keyed by
// weapon_name (the entry's first string property, matching the loader convention).
struct AmmoEntrySim {
    std::string weapon_name;
    int ammo = 0;
    int reserve = 0;
};

struct PlayerSim {
    std::string unique_id;
    std::string name;
    int team = 1;
    float health = 100.0f, armor = 50.0f;
    double pos_x = 0, pos_y = 0, pos_z = 0;
    double vel_x = 0, vel_y = 0, vel_z = 0;
    float rot_x = 0, rot_y = 0, rot_z = 0, rot_w = 1;
    bool is_alive = true;
    int score = 0, deaths = 0, respawn_timer = 0;
    // Movement params (set once at init).
    double base_x = 0, base_z = 0;
    double freq_x = 0, freq_z = 0;
    double phase_x = 0, phase_z = 0;

    // ---- Rich containers (exercise scalar arrays, a nested struct, an
    //      array-of-structs and a map; all evolve over the match) ----
    std::vector<std::string> abilities;       // scalar string array
    std::vector<float> ability_cooldowns;     // scalar float array (parallel to abilities)
    std::string primary_weapon;               // -> Loadout nested struct
    std::string secondary_weapon;             // -> Loadout nested struct
    int grenades = 2;                          // -> Loadout nested struct
    std::vector<InventoryItemSim> inventory;  // array of nested structs
    std::vector<AmmoEntrySim> ammo_by_weapon; // map<weapon, AmmoEntry>
};

// ---- Per-player container mutators (keep the sim loop readable) ----

// The armor plate tracks the player's armor: its durability mirrors the scalar
// Armor field while equipped, and the whole row drops out of the inventory when
// the plate breaks (armor == 0), returning on respawn -- so the inventory
// array-of-structs varies in length frame-to-frame, not just in content.
static void SyncArmorPlate(PlayerSim& p) {
    auto it = std::find_if(p.inventory.begin(), p.inventory.end(),
                           [](const InventoryItemSim& i) { return i.item_id == "armor_plate"; });
    if (p.armor > 0.0f) {
        if (it == p.inventory.end()) {
            p.inventory.push_back(InventoryItemSim {"armor_plate", "Armor Plate", 1, p.armor, 2});
        } else {
            it->durability = p.armor;
            it->quantity = 1;
        }
    } else if (it != p.inventory.end()) {
        p.inventory.erase(it);
    }
}

// Spend one medkit on respawn; drop the row entirely once depleted so the
// inventory array length varies frame-to-frame.
static void ConsumeMedkit(PlayerSim& p) {
    for (auto it = p.inventory.begin(); it != p.inventory.end(); ++it) {
        if (it->item_id == "medkit") {
            if (it->quantity > 0)
                --it->quantity;
            if (it->quantity == 0)
                p.inventory.erase(it);
            return;
        }
    }
}

static void RefillAmmo(PlayerSim& p) {
    for (auto& a : p.ammo_by_weapon) {
        if (a.weapon_name == "Pistol") {
            a.ammo = 12;
            a.reserve = 36;
        } else {
            a.ammo = 30;
            a.reserve = 90;
        }
    }
}

static AmmoEntrySim* PrimaryAmmo(PlayerSim& p) {
    for (auto& a : p.ammo_by_weapon) {
        if (a.weapon_name != "Pistol")
            return &a;
    }
    return nullptr;
}

struct ProjectileSim {
    std::string unique_id, owner_id;
    double pos_x = 0, pos_y = 0, pos_z = 0;
    double vel_x = 0, vel_y = 0, vel_z = 0;
    float damage = 25.0f;
    std::string type = "bullet";
    int lifetime = PROJ_LIFETIME;
};

struct MatchSim {
    int score_team1 = 0, score_team2 = 0, round = 1;
    std::string phase = "warmup";
    float time_remaining = 0.0f;
};

struct FrameSnapshot {
    std::vector<PlayerSim> players;
    std::vector<ProjectileSim> projectiles;
    MatchSim match;
    float game_time = 0.0f;
    int64_t utc_ticks = 0;
};


// ===================================================================
//  SECTION 3 — Simulation
// ===================================================================

static int64_t GetUtcNowTicks() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count();
    return ns / 100;
}

static std::vector<FrameSnapshot> RunSimulation(int64_t base_utc) {
    static const char* const kNames[NUM_PLAYERS] = {"Alpha",   "Bravo", "Charlie", "Delta", "Echo",
                                                    "Foxtrot", "Golf",  "Hotel",   "India", "Juliet"};

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
        p.name = kNames[i];
        p.team = (i < 5) ? 1 : 2;
        p.base_x = ARENA_MIN + (ARENA_MAX - ARENA_MIN) * (i + 0.5) / NUM_PLAYERS;
        p.base_z = (p.team == 1) ? -20.0 : 20.0;
        p.pos_x = p.base_x;
        p.pos_z = p.base_z;
        p.freq_x = rand_d(0.3, 1.2);
        p.freq_z = rand_d(0.3, 1.2);
        p.phase_x = rand_d(0.0, 6.28);
        p.phase_z = rand_d(0.0, 6.28);

        // ---- Rich containers ----
        p.primary_weapon = (p.team == 1) ? "Rifle" : "SMG";
        p.secondary_weapon = "Pistol";
        p.grenades = 2;

        // Scalar arrays: a shared pair plus a team-specific ultimate, with a
        // parallel cooldown array that starts ready (0).
        p.abilities = {"Dash", "Shield", (p.team == 1) ? "Airstrike" : "Cloak"};
        p.ability_cooldowns.assign(p.abilities.size(), 0.0f);

        // Array of nested structs: medkit + ammo box + armor plate.
        p.inventory = {
            InventoryItemSim {"medkit", "Medkit", 2, 100.0f, 0},
            InventoryItemSim {"ammo_box", "Ammo Box", 1, 100.0f, 1},
            InventoryItemSim {"armor_plate", "Armor Plate", 1, p.armor, 2},
        };

        // Map<weapon, ammo>: primary weapon + sidearm.
        p.ammo_by_weapon = {
            AmmoEntrySim {p.primary_weapon, 30, 90},
            AmmoEntrySim {"Pistol", 12, 36},
        };
    }

    MatchSim match;
    std::vector<ProjectileSim> projectiles;
    int next_proj_id = 0;

    std::vector<FrameSnapshot> frames;
    frames.reserve(TOTAL_FRAMES);

    for (int f = 0; f < TOTAL_FRAMES; ++f) {
        const float t = static_cast<float>(f) * DT;

        // Phase.
        if (f < WARMUP_END) {
            match.phase = "warmup";
            match.time_remaining = (WARMUP_END - f) * DT;
        } else if (f < PLAYING_END) {
            match.phase = "playing";
            match.time_remaining = (PLAYING_END - f) * DT;
        } else {
            match.phase = "roundend";
            match.time_remaining = (TOTAL_FRAMES - f) * DT;
        }

        // Movement.
        for (auto& p : players) {
            if (!p.is_alive) {
                if (--p.respawn_timer <= 0) {
                    p.is_alive = true;
                    p.health = 100;
                    p.armor = 50;
                    p.pos_x = p.base_x;
                    p.pos_z = p.base_z;
                    // Respawn refit: heal from a medkit, top up ammo, reset cooldowns.
                    ConsumeMedkit(p);
                    RefillAmmo(p);
                    std::fill(p.ability_cooldowns.begin(), p.ability_cooldowns.end(), 0.0f);
                }
                p.vel_x = p.vel_y = p.vel_z = 0;
                SyncArmorPlate(p); // armor is 0 while dead
                continue;
            }
            // Tick ability cooldowns down toward ready.
            for (auto& cd : p.ability_cooldowns)
                cd = std::max(0.0f, cd - DT);
            SyncArmorPlate(p);

            double px = p.pos_x, pz = p.pos_z;
            p.pos_x = std::clamp(p.base_x + 15.0 * std::sin(p.freq_x * t + p.phase_x), ARENA_MIN, ARENA_MAX);
            p.pos_z = std::clamp(p.base_z + 10.0 * std::sin(p.freq_z * t + p.phase_z), ARENA_MIN, ARENA_MAX);
            p.vel_x = (p.pos_x - px) / DT;
            p.vel_z = (p.pos_z - pz) / DT;
            p.vel_y = 0;
            if (std::abs(p.vel_x) > 0.001 || std::abs(p.vel_z) > 0.001) {
                float yaw = std::atan2(float(p.vel_x), float(p.vel_z));
                p.rot_y = std::sin(yaw * 0.5f);
                p.rot_w = std::cos(yaw * 0.5f);
                p.rot_x = p.rot_z = 0;
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
                    proj.pos_x = players[si].pos_x;
                    proj.pos_y = 1.0;
                    proj.pos_z = players[si].pos_z;
                    proj.vel_z = (players[si].team == 1) ? 30.0 : -30.0;
                    projectiles.push_back(std::move(proj));

                    // Firing spends a round from the primary weapon (reloading
                    // from reserve when the magazine runs dry) and puts Dash on
                    // cooldown -- the map and cooldown array mutate together.
                    if (AmmoEntrySim* a = PrimaryAmmo(players[si])) {
                        if (--a->ammo <= 0) {
                            const int reload = std::min(30, a->reserve);
                            a->ammo = reload;
                            a->reserve -= reload;
                        }
                    }
                    if (!players[si].ability_cooldowns.empty())
                        players[si].ability_cooldowns[0] = 4.0f;
                }
            }
            if (f % KILL_INTERVAL == KILL_OFFSET && f >= KILL_MIN_FRAME) {
                int ki = ((f / KILL_INTERVAL) * 3) % NUM_PLAYERS;
                int vi = (ki + 5) % NUM_PLAYERS;
                if (players[ki].is_alive && players[vi].is_alive) {
                    players[vi].health = 0;
                    players[vi].armor = 0;
                    players[vi].is_alive = false;
                    players[vi].respawn_timer = RESPAWN_FRAMES;
                    players[vi].deaths++;
                    players[ki].score++;
                    // The killer lobs a grenade (Loadout.grenades ticks down).
                    if (players[ki].grenades > 0)
                        --players[ki].grenades;
                    if (players[ki].team == 1)
                        match.score_team1++;
                    else
                        match.score_team2++;
                }
            }
        }

        // Advance projectiles.
        for (auto& pr : projectiles) {
            pr.pos_x += pr.vel_x * DT;
            pr.pos_y += pr.vel_y * DT;
            pr.pos_z += pr.vel_z * DT;
            pr.lifetime--;
        }
        std::erase_if(projectiles, [](auto& p) { return p.lifetime <= 0; });

        FrameSnapshot snap;
        snap.players = players;
        snap.projectiles = projectiles;
        snap.match = match;
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
static void ExportJsonSource(const std::vector<FrameSnapshot>& frames, const std::string& path) {
    nlohmann::json root;
    root["replay_name"] = "Arena Sample Replay";
    root["total_frames"] = TOTAL_FRAMES;
    root["fps"] = int(FPS);
    root["duration_seconds"] = double(TOTAL_FRAMES) / FPS;

    auto& jf = root["frames"];
    jf = nlohmann::json::array();
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = frames[i];
        nlohmann::json f;
        f["frame_index"] = i;
        f["game_time"] = s.game_time;
        f["utc_ticks"] = s.utc_ticks;

        f["players"] = nlohmann::json::array();
        for (const auto& p : s.players) {
            nlohmann::json pj = {{"unique_id", p.unique_id},
                                 {"name", p.name},
                                 {"team", p.team},
                                 {"health", p.health},
                                 {"armor", p.armor},
                                 {"position", {{"x", p.pos_x}, {"y", p.pos_y}, {"z", p.pos_z}}},
                                 {"rotation", {{"x", p.rot_x}, {"y", p.rot_y}, {"z", p.rot_z}, {"w", p.rot_w}}},
                                 {"velocity", {{"x", p.vel_x}, {"y", p.vel_y}, {"z", p.vel_z}}},
                                 {"is_alive", p.is_alive},
                                 {"score", p.score},
                                 {"deaths", p.deaths},
                                 {"abilities", p.abilities},
                                 {"ability_cooldowns", p.ability_cooldowns},
                                 {"loadout",
                                  {{"primary_weapon", p.primary_weapon},
                                   {"secondary_weapon", p.secondary_weapon},
                                   {"grenades", p.grenades},
                                   {"has_armor", p.armor > 0.0f}}}};

            pj["inventory"] = nlohmann::json::array();
            for (const auto& it : p.inventory)
                pj["inventory"].push_back({{"item_id", it.item_id},
                                           {"display_name", it.display_name},
                                           {"quantity", it.quantity},
                                           {"durability", it.durability},
                                           {"slot", it.slot}});

            pj["ammo_by_weapon"] = nlohmann::json::array();
            for (const auto& a : p.ammo_by_weapon)
                pj["ammo_by_weapon"].push_back(
                    {{"weapon_name", a.weapon_name}, {"ammo", a.ammo}, {"reserve", a.reserve}});

            f["players"].push_back(std::move(pj));
        }

        f["projectiles"] = nlohmann::json::array();
        for (const auto& pr : s.projectiles)
            f["projectiles"].push_back({{"unique_id", pr.unique_id},
                                        {"owner_id", pr.owner_id},
                                        {"position", {{"x", pr.pos_x}, {"y", pr.pos_y}, {"z", pr.pos_z}}},
                                        {"velocity", {{"x", pr.vel_x}, {"y", pr.vel_y}, {"z", pr.vel_z}}},
                                        {"damage", pr.damage},
                                        {"type", pr.type}});

        f["match_state"] = {{"score_team1", s.match.score_team1},
                            {"score_team2", s.match.score_team2},
                            {"round", s.match.round},
                            {"phase", s.match.phase},
                            {"time_remaining", s.match.time_remaining}};
        jf.push_back(std::move(f));
    }
    std::ofstream(path) << root.dump(2);
}

// ---- 4b: Protobuf binary ----
static void ExportProtoSource(const std::vector<FrameSnapshot>& frames, const std::string& path) {
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
            pp->set_unique_id(p.unique_id);
            pp->set_name(p.name);
            pp->set_team(p.team);
            pp->set_health(p.health);
            pp->set_armor(p.armor);
            auto* pos = pp->mutable_position();
            pos->set_x(p.pos_x);
            pos->set_y(p.pos_y);
            pos->set_z(p.pos_z);
            auto* rot = pp->mutable_rotation();
            rot->set_x(p.rot_x);
            rot->set_y(p.rot_y);
            rot->set_z(p.rot_z);
            rot->set_w(p.rot_w);
            auto* vel = pp->mutable_velocity();
            vel->set_x(p.vel_x);
            vel->set_y(p.vel_y);
            vel->set_z(p.vel_z);
            pp->set_is_alive(p.is_alive);
            pp->set_score(p.score);
            pp->set_deaths(p.deaths);

            // Rich containers.
            for (const auto& ability : p.abilities)
                pp->add_abilities(ability);
            for (float cd : p.ability_cooldowns)
                pp->add_ability_cooldowns(cd);

            auto* lo = pp->mutable_loadout();
            lo->set_primary_weapon(p.primary_weapon);
            lo->set_secondary_weapon(p.secondary_weapon);
            lo->set_grenades(p.grenades);
            lo->set_has_armor(p.armor > 0.0f);

            for (const auto& it : p.inventory) {
                auto* iv = pp->add_inventory();
                iv->set_item_id(it.item_id);
                iv->set_display_name(it.display_name);
                iv->set_quantity(it.quantity);
                iv->set_durability(it.durability);
                iv->set_slot(it.slot);
            }
            for (const auto& a : p.ammo_by_weapon) {
                auto* ae = pp->add_ammo_by_weapon();
                ae->set_weapon_name(a.weapon_name);
                ae->set_ammo(a.ammo);
                ae->set_reserve(a.reserve);
            }
        }
        for (const auto& pr : s.projectiles) {
            auto* pp = fd->add_projectiles();
            pp->set_unique_id(pr.unique_id);
            pp->set_owner_id(pr.owner_id);
            auto* pos = pp->mutable_position();
            pos->set_x(pr.pos_x);
            pos->set_y(pr.pos_y);
            pos->set_z(pr.pos_z);
            auto* vel = pp->mutable_velocity();
            vel->set_x(pr.vel_x);
            vel->set_y(pr.vel_y);
            vel->set_z(pr.vel_z);
            pp->set_damage(pr.damage);
            pp->set_type(pr.type);
        }
        auto* ms = fd->mutable_match_state();
        ms->set_score_team1(s.match.score_team1);
        ms->set_score_team2(s.match.score_team2);
        ms->set_round(s.match.round);
        ms->set_phase(s.match.phase);
        ms->set_time_remaining(s.match.time_remaining);
    }

    std::ofstream ofs(path, std::ios::binary);
    replay.SerializeToOstream(&ofs);
}

// ---- 4c: Raw binary (BinaryCursor sample) ----
// Layout (all little-endian, byte-aligned):
//
//   Header:
//     MAGIC: u32   "ABIN" (0x4E494241)
//     VERSION: u16  1
//     TOTAL_FRAMES: u32
//     FPS: u16
//     DURATION_SECONDS: double
//
//   For each frame:
//     FRAME_BYTES: u32   (length of the frame block that follows -- lets
//                         the consumer SubCursor() the frame as a bounded region)
//     [frame block, FRAME_BYTES bytes:]
//       FRAME_INDEX: i32
//       GAME_TIME:   float
//       UTC_TICKS:   i64
//       PLAYER_COUNT: u32
//       per player: u16-len UniqueID, u16-len Name, i32 Team, f32 Health, f32 Armor,
//                   3x f64 Position, 4x f32 Rotation, 3x f64 Velocity,
//                   u8 IsAlive, i32 Score, i32 Deaths,
//                   -- rich containers --
//                   ABILITY_COUNT: u32, per ability: u16-len Name
//                   COOLDOWN_COUNT: u32, per cooldown: f32
//                   Loadout: u16-len PrimaryWeapon, u16-len SecondaryWeapon,
//                            i32 Grenades, u8 HasArmor
//                   INVENTORY_COUNT: u32, per item: u16-len ItemID,
//                            u16-len DisplayName, i32 Quantity, f32 Durability, i32 Slot
//                   AMMO_COUNT: u32, per entry: u16-len WeaponName, i32 Ammo, i32 Reserve
//       PROJ_COUNT: u32
//       per projectile: u16-len UniqueID, u16-len OwnerID, 3x f64 Position,
//                       3x f64 Velocity, f32 Damage, u16-len Type
//       MatchState: u16-len UniqueID, i32 ScoreTeam1, i32 ScoreTeam2,
//                   i32 Round, u16-len Phase, f32 TimeRemaining
//
// This layout exercises three BinaryCursor primitives: Read<T>() for POD,
// ReadLenString<uint16_t>() for length-prefixed strings, and SubCursor()
// to carve each frame as its own bounded slice.
static void ExportBinarySource(const std::vector<FrameSnapshot>& frames, const std::string& path) {
    std::vector<uint8_t> buf;

    auto append_bytes = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    auto append_lenstr = [&](const std::string& s) {
        const uint16_t len = static_cast<uint16_t>(s.size());
        append_bytes(&len, sizeof(len));
        append_bytes(s.data(), s.size());
    };
    auto reserve_u32 = [&]() {
        const size_t pos = buf.size();
        const uint32_t zero = 0;
        append_bytes(&zero, sizeof(zero));
        return pos;
    };
    auto patch_u32 = [&](size_t pos, uint32_t value) {
        std::memcpy(buf.data() + pos, &value, sizeof(value));
    };

    // ---- Header ----
    const uint32_t magic = 0x4E494241u; // 'ABIN' little-endian
    append_bytes(&magic, sizeof(magic));
    const uint16_t version = 1;
    append_bytes(&version, sizeof(version));
    const uint32_t total = TOTAL_FRAMES;
    append_bytes(&total, sizeof(total));
    const uint16_t fps_u16 = static_cast<uint16_t>(FPS);
    append_bytes(&fps_u16, sizeof(fps_u16));
    const double duration = double(TOTAL_FRAMES) / FPS;
    append_bytes(&duration, sizeof(duration));

    // ---- Per frame ----
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = frames[i];

        const size_t len_pos = reserve_u32();
        const size_t frame_start = buf.size();

        const int32_t frame_idx = static_cast<int32_t>(i);
        append_bytes(&frame_idx, sizeof(frame_idx));
        append_bytes(&s.game_time, sizeof(s.game_time));
        append_bytes(&s.utc_ticks, sizeof(s.utc_ticks));

        // Players
        const uint32_t player_count = static_cast<uint32_t>(s.players.size());
        append_bytes(&player_count, sizeof(player_count));
        for (const auto& p : s.players) {
            append_lenstr(p.unique_id);
            append_lenstr(p.name);
            const int32_t team = p.team;
            append_bytes(&team, sizeof(team));
            append_bytes(&p.health, sizeof(p.health));
            append_bytes(&p.armor, sizeof(p.armor));
            append_bytes(&p.pos_x, sizeof(p.pos_x));
            append_bytes(&p.pos_y, sizeof(p.pos_y));
            append_bytes(&p.pos_z, sizeof(p.pos_z));
            append_bytes(&p.rot_x, sizeof(p.rot_x));
            append_bytes(&p.rot_y, sizeof(p.rot_y));
            append_bytes(&p.rot_z, sizeof(p.rot_z));
            append_bytes(&p.rot_w, sizeof(p.rot_w));
            append_bytes(&p.vel_x, sizeof(p.vel_x));
            append_bytes(&p.vel_y, sizeof(p.vel_y));
            append_bytes(&p.vel_z, sizeof(p.vel_z));
            const uint8_t alive = p.is_alive ? 1 : 0;
            append_bytes(&alive, sizeof(alive));
            const int32_t score = p.score;
            append_bytes(&score, sizeof(score));
            const int32_t deaths = p.deaths;
            append_bytes(&deaths, sizeof(deaths));

            // ---- Rich containers (order MUST match arena_binary_mappings.h) ----
            // Abilities (scalar string array).
            const uint32_t ability_count = static_cast<uint32_t>(p.abilities.size());
            append_bytes(&ability_count, sizeof(ability_count));
            for (const auto& ability : p.abilities)
                append_lenstr(ability);

            // Ability cooldowns (scalar float array).
            const uint32_t cooldown_count = static_cast<uint32_t>(p.ability_cooldowns.size());
            append_bytes(&cooldown_count, sizeof(cooldown_count));
            for (float cd : p.ability_cooldowns)
                append_bytes(&cd, sizeof(cd));

            // Loadout (nested struct).
            append_lenstr(p.primary_weapon);
            append_lenstr(p.secondary_weapon);
            const int32_t grenades = p.grenades;
            append_bytes(&grenades, sizeof(grenades));
            const uint8_t has_armor = (p.armor > 0.0f) ? 1 : 0;
            append_bytes(&has_armor, sizeof(has_armor));

            // Inventory (array of nested structs).
            const uint32_t inventory_count = static_cast<uint32_t>(p.inventory.size());
            append_bytes(&inventory_count, sizeof(inventory_count));
            for (const auto& it : p.inventory) {
                append_lenstr(it.item_id);
                append_lenstr(it.display_name);
                const int32_t quantity = it.quantity;
                append_bytes(&quantity, sizeof(quantity));
                append_bytes(&it.durability, sizeof(it.durability));
                const int32_t slot = it.slot;
                append_bytes(&slot, sizeof(slot));
            }

            // AmmoByWeapon (map<weapon, AmmoEntry>).
            const uint32_t ammo_count = static_cast<uint32_t>(p.ammo_by_weapon.size());
            append_bytes(&ammo_count, sizeof(ammo_count));
            for (const auto& a : p.ammo_by_weapon) {
                append_lenstr(a.weapon_name);
                const int32_t ammo = a.ammo;
                append_bytes(&ammo, sizeof(ammo));
                const int32_t reserve = a.reserve;
                append_bytes(&reserve, sizeof(reserve));
            }
        }

        // Projectiles
        const uint32_t proj_count = static_cast<uint32_t>(s.projectiles.size());
        append_bytes(&proj_count, sizeof(proj_count));
        for (const auto& pr : s.projectiles) {
            append_lenstr(pr.unique_id);
            append_lenstr(pr.owner_id);
            append_bytes(&pr.pos_x, sizeof(pr.pos_x));
            append_bytes(&pr.pos_y, sizeof(pr.pos_y));
            append_bytes(&pr.pos_z, sizeof(pr.pos_z));
            append_bytes(&pr.vel_x, sizeof(pr.vel_x));
            append_bytes(&pr.vel_y, sizeof(pr.vel_y));
            append_bytes(&pr.vel_z, sizeof(pr.vel_z));
            append_bytes(&pr.damage, sizeof(pr.damage));
            append_lenstr(pr.type);
        }

        // MatchState
        append_lenstr(std::string("match_001"));
        append_bytes(&s.match.score_team1, sizeof(s.match.score_team1));
        append_bytes(&s.match.score_team2, sizeof(s.match.score_team2));
        append_bytes(&s.match.round, sizeof(s.match.round));
        append_lenstr(s.match.phase);
        append_bytes(&s.match.time_remaining, sizeof(s.match.time_remaining));

        // Backfill the frame length prefix.
        const uint32_t frame_bytes = static_cast<uint32_t>(buf.size() - frame_start);
        patch_u32(len_pos, frame_bytes);
    }

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
}

// ---- 4d: FlatBuffers binary ----
static void ExportFbsSource(const std::vector<FrameSnapshot>& frames, const std::string& path) {
    // Build using the Object API (ArenaReplayT, FrameDataT, PlayerT, ...).
    ::arena_fb::ArenaReplayT replay;
    replay.replay_name = "Arena Sample Replay";
    replay.total_frames = TOTAL_FRAMES;
    replay.fps = int(FPS);
    replay.duration_seconds = double(TOTAL_FRAMES) / FPS;

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = frames[i];
        auto fd = std::make_unique<::arena_fb::FrameDataT>();
        fd->frame_index = int(i);
        fd->game_time = s.game_time;
        fd->utc_ticks = s.utc_ticks;

        for (const auto& p : s.players) {
            auto pp = std::make_unique<::arena_fb::PlayerT>();
            pp->unique_id = p.unique_id;
            pp->name = p.name;
            pp->team = p.team;
            pp->health = p.health;
            pp->armor = p.armor;
            pp->position = std::make_unique<::arena_fb::Vec3>(p.pos_x, p.pos_y, p.pos_z);
            pp->rotation = std::make_unique<::arena_fb::Rotation>(p.rot_x, p.rot_y, p.rot_z, p.rot_w);
            pp->velocity = std::make_unique<::arena_fb::Vec3>(p.vel_x, p.vel_y, p.vel_z);
            pp->is_alive = p.is_alive;
            pp->score = p.score;
            pp->deaths = p.deaths;

            // Rich containers.
            pp->abilities = p.abilities;
            pp->ability_cooldowns = p.ability_cooldowns;

            pp->loadout = std::make_unique<::arena_fb::LoadoutT>();
            pp->loadout->primary_weapon = p.primary_weapon;
            pp->loadout->secondary_weapon = p.secondary_weapon;
            pp->loadout->grenades = p.grenades;
            pp->loadout->has_armor = (p.armor > 0.0f);

            for (const auto& it : p.inventory) {
                auto iv = std::make_unique<::arena_fb::InventoryItemT>();
                iv->item_id = it.item_id;
                iv->display_name = it.display_name;
                iv->quantity = it.quantity;
                iv->durability = it.durability;
                iv->slot = it.slot;
                pp->inventory.push_back(std::move(iv));
            }
            for (const auto& a : p.ammo_by_weapon) {
                auto ae = std::make_unique<::arena_fb::AmmoEntryT>();
                ae->weapon_name = a.weapon_name;
                ae->ammo = a.ammo;
                ae->reserve = a.reserve;
                pp->ammo_by_weapon.push_back(std::move(ae));
            }

            fd->players.push_back(std::move(pp));
        }
        for (const auto& pr : s.projectiles) {
            auto pp = std::make_unique<::arena_fb::ProjectileT>();
            pp->unique_id = pr.unique_id;
            pp->owner_id = pr.owner_id;
            pp->position = std::make_unique<::arena_fb::Vec3>(pr.pos_x, pr.pos_y, pr.pos_z);
            pp->velocity = std::make_unique<::arena_fb::Vec3>(pr.vel_x, pr.vel_y, pr.vel_z);
            pp->damage = pr.damage;
            pp->type = pr.type;
            fd->projectiles.push_back(std::move(pp));
        }
        auto ms = std::make_unique<::arena_fb::MatchStateT>();
        ms->score_team1 = s.match.score_team1;
        ms->score_team2 = s.match.score_team2;
        ms->round = s.match.round;
        ms->phase = s.match.phase;
        ms->time_remaining = s.match.time_remaining;
        fd->match_state = std::move(ms);

        replay.frames.push_back(std::move(fd));
    }

    flatbuffers::FlatBufferBuilder fbb;
    fbb.Finish(::arena_fb::ArenaReplay::Pack(fbb, &replay));

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
}


// ===================================================================
int main() {
    const std::string schema_path = "content/writer/arena/arena_schema.json";
    const std::string writer_dir = "content/writer/arena";
    const std::string reader_dir = "content/reader/arena";

    std::filesystem::create_directories(writer_dir);
    std::filesystem::create_directories(reader_dir);

    VTX_INFO("=== Arena Replay Generator ===");
    VTX_INFO("Simulating {} frames ({:.1f}s @ {:.0f} FPS)...", TOTAL_FRAMES, double(TOTAL_FRAMES) / FPS, FPS);

    // ---- Phase 1: Simulate ----
    // Use a fixed historical timestamp so the data is reproducible across
    // runs.  The writer accepts any strictly-increasing UTC sequence now.
    const int64_t base_utc = 1'745'000'000LL * 10'000'000LL; // 2025-04-19 UTC
    auto sim_frames = RunSimulation(base_utc);
    VTX_INFO("Simulation complete: {} frames, {} players.", int(sim_frames.size()), NUM_PLAYERS);

    // ---- Phase 2: Export 4 data-source files ----
    const std::string json_src = writer_dir + "/arena_replay_data.json";
    const std::string proto_src = writer_dir + "/arena_replay_data.proto.bin";
    const std::string bin_src = writer_dir + "/arena_replay_data.bin";
    const std::string fbs_src = writer_dir + "/arena_replay_data.fbs.bin";

    ExportJsonSource(sim_frames, json_src);
    VTX_INFO("Exported data source: {}", json_src);

    ExportProtoSource(sim_frames, proto_src);
    VTX_INFO("Exported data source: {}", proto_src);

    ExportBinarySource(sim_frames, bin_src);
    VTX_INFO("Exported data source: {}", bin_src);

    ExportFbsSource(sim_frames, fbs_src);
    VTX_INFO("Exported data source: {}", fbs_src);


    // ---- Summary ----
    VTX_INFO("=== Generation Complete ===");
    VTX_INFO("  Data sources (content/writer/arena/):");
    VTX_INFO("    arena_replay_data.json       (JSON)");
    VTX_INFO("    arena_replay_data.proto.bin  (Protobuf)");
    VTX_INFO("    arena_replay_data.bin        (Raw binary -- BinaryCursor sample)");
    VTX_INFO("    arena_replay_data.fbs.bin    (FlatBuffers)");


    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
