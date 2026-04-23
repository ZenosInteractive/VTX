// VTX SDK -- schema loading benchmark.
//
// SchemaRegistry::LoadFromJson runs once per replay open (inside the
// reader facade).  This file measures it in isolation so we have a
// reliable number for:
//
//   * "how much of OpenReplayFile time is schema parsing"
//   * regression detection when the JSON parser or property-cache
//     construction is changed
//
// The arena schema (samples/content/writer/arena/arena_schema.json) is the
// only schema committed to the repo right now -- ~8.7 KB with Player,
// Projectile, MatchState, Weapon, Transform, Vector entries.  Larger
// fixtures (CS, RL) embed their schema inside the .vtx file rather than
// shipping a separate JSON, so they're not reachable from this path.
//
// ELoadMethod::Both also builds the PropertyAddressCache which is what a
// real reader opens with.  We also measure LoadToBuffer-only so the JSON
// parse vs. cache construction split is visible.

#include "bench_utils.h"

#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_logger.h"

#include <benchmark/benchmark.h>

#include <filesystem>
#include <string>

namespace {

std::string ArenaSchemaPath() {
    return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path()
            / "samples" / "content" / "writer" / "arena" / "arena_schema.json")
        .string();
}

struct SilenceDebugLogsOnce {
    SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
};
const SilenceDebugLogsOnce silence_schema_debug_logs_once{};

}  // namespace

// Full load: JSON parse + SchemaStruct population + PropertyAddressCache
// construction.  This is what OpenReplayFile triggers internally.
static void BM_Schema_LoadArena_Both(benchmark::State& state) {
    const std::string path = ArenaSchemaPath();
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("arena schema missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    for (auto _ : state) {
        VTX::SchemaRegistry registry;
        const bool ok = registry.LoadFromJson(path, VTX::SchemaRegistry::ELoadMethod::Both);
        if (!ok) { state.SkipWithError("LoadFromJson failed"); break; }
        benchmark::DoNotOptimize(registry);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Schema_LoadArena_Both)->Unit(benchmark::kMicrosecond);

// JSON parse + struct map only (no PropertyAddressCache).  Delta vs. Both
// is the cache-construction cost.
static void BM_Schema_LoadArena_BufferOnly(benchmark::State& state) {
    const std::string path = ArenaSchemaPath();
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("arena schema missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    for (auto _ : state) {
        VTX::SchemaRegistry registry;
        const bool ok = registry.LoadFromJson(path, VTX::SchemaRegistry::ELoadMethod::LoadToBuffer);
        if (!ok) { state.SkipWithError("LoadFromJson failed"); break; }
        benchmark::DoNotOptimize(registry);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Schema_LoadArena_BufferOnly)->Unit(benchmark::kMicrosecond);
