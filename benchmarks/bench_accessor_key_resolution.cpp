// VTX SDK -- key resolution benchmark.
//
// Measures the one-time setup cost a consumer pays: calling
// `accessor.Get<T>(structName, propName)` to turn a (struct, property)
// pair into a fast PropertyKey<T>.  This is the cost the consumer amortises
// over every subsequent EntityView::Get(key) call.
//
// Complements bench_property_cache.cpp which probes the internal hash maps
// directly.  This file probes the public-facing API instead.

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/reader/core/vtx_frame_accessor.h"
#include "vtx/common/vtx_logger.h"

#include <benchmark/benchmark.h>

#include <array>
#include <filesystem>
#include <string>

namespace {

std::string ArenaReplayPath() {
    return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path()
            / "samples" / "content" / "reader" / "arena" / "arena_from_fbs_ds.vtx")
        .string();
}

struct SilenceDebugLogsOnce {
    SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
};
const SilenceDebugLogsOnce silence_keyres_debug_logs_once{};

// A representative spread of property lookups a real integration performs.
struct PropSpec { const char* struct_name; const char* prop_name; };
constexpr std::array<PropSpec, 9> kProps = {{
    {"Player",     "Position"},
    {"Player",     "Health"},
    {"Player",     "UniqueID"},
    {"Player",     "Velocity"},
    {"Player",     "Score"},
    {"Projectile", "Position"},
    {"Projectile", "Damage"},
    {"MatchState", "ScoreTeam1"},
    {"MatchState", "ScoreTeam2"},
}};

}  // namespace

// Resolve a full set of PropertyKey<T> via the public accessor API.  If
// the underlying cache is O(1) then time-per-resolution should stay
// constant regardless of how many properties the schema defines.
static void BM_AccessorKeyResolution(benchmark::State& state) {
    auto result = VTX::OpenReplayFile(ArenaReplayPath());
    if (!result) { state.SkipWithError("OpenReplayFile failed"); return; }

    const auto accessor = result.reader->CreateAccessor();

    int64_t resolutions = 0;
    for (auto _ : state) {
        // 3 scalar + 3 vector + 3 string lookups per iteration.
        benchmark::DoNotOptimize(accessor.Get<VTX::Vector>(kProps[0].struct_name, kProps[0].prop_name));
        benchmark::DoNotOptimize(accessor.Get<float>(kProps[1].struct_name, kProps[1].prop_name));
        benchmark::DoNotOptimize(accessor.Get<std::string>(kProps[2].struct_name, kProps[2].prop_name));
        benchmark::DoNotOptimize(accessor.Get<VTX::Vector>(kProps[3].struct_name, kProps[3].prop_name));
        benchmark::DoNotOptimize(accessor.Get<int32_t>(kProps[4].struct_name, kProps[4].prop_name));
        benchmark::DoNotOptimize(accessor.Get<VTX::Vector>(kProps[5].struct_name, kProps[5].prop_name));
        benchmark::DoNotOptimize(accessor.Get<float>(kProps[6].struct_name, kProps[6].prop_name));
        benchmark::DoNotOptimize(accessor.Get<int32_t>(kProps[7].struct_name, kProps[7].prop_name));
        benchmark::DoNotOptimize(accessor.Get<int32_t>(kProps[8].struct_name, kProps[8].prop_name));
        resolutions += static_cast<int64_t>(kProps.size());
    }

    state.SetItemsProcessed(resolutions);
    state.counters["props_per_iter"] = static_cast<double>(kProps.size());
}
BENCHMARK(BM_AccessorKeyResolution)->Unit(benchmark::kMicrosecond);
