// VTX SDK -- PropertyAddressCache benchmarks.
//
// Scenarios
//   BM_PropertyAddressCacheLookup  resolve struct -> property via the three
//                                  hash-map chain (name_to_id + structs +
//                                  properties).  Confirms the O(1) claim.
//
// The arena schema has ~5 struct types and ~10 properties each, so the cache
// is small by design.  Per-lookup time should stay flat if the underlying
// maps are well-sized; any super-linear growth would be visible here.

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_logger.h"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

namespace {

    std::string FixturePath(const char* name) {
        return std::string(VTX_BENCH_FIXTURES_DIR) + "/" + name;
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_pcache_debug_logs_once {};

    // Collect every (struct_name, property_name) pair present in the cache so
    // the benchmark can iterate them in a round-robin.
    struct ResolutionKey {
        std::string struct_name;
        std::string property_name;
    };

    std::vector<ResolutionKey> CollectResolutionKeys(const VTX::PropertyAddressCache& cache) {
        std::vector<ResolutionKey> keys;
        for (const auto& [struct_name, struct_id] : cache.name_to_id) {
            auto it = cache.structs.find(struct_id);
            if (it == cache.structs.end())
                continue;
            for (const auto& [property_name, _addr] : it->second.properties) {
                keys.push_back({struct_name, property_name});
            }
        }
        return keys;
    }

} // namespace

// Three hash lookups per resolution: name -> struct_id -> schema cache ->
// property address.  Per-iteration loops through every known (struct,
// property) pair so the numbers aren't dominated by hitting the same slot.
static void BM_PropertyAddressCacheLookup(benchmark::State& state) {
    const std::string path = FixturePath("synth_10k.vtx");

    auto result = VTX::OpenReplayFile(path);
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }

    const auto cache = result.reader->GetPropertyAddressCache();
    const auto keys = CollectResolutionKeys(cache);
    if (keys.empty()) {
        state.SkipWithError("empty PropertyAddressCache");
        return;
    }

    size_t hit_count = 0;

    for (auto _ : state) {
        for (const auto& key : keys) {
            const auto id_it = cache.name_to_id.find(key.struct_name);
            if (id_it == cache.name_to_id.end())
                continue;
            const auto struct_it = cache.structs.find(id_it->second);
            if (struct_it == cache.structs.end())
                continue;
            const auto prop_it = struct_it->second.properties.find(key.property_name);
            if (prop_it != struct_it->second.properties.end()) {
                benchmark::DoNotOptimize(prop_it->second);
                ++hit_count;
            }
        }
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(keys.size()));
    state.counters["keys_per_iter"] = static_cast<double>(keys.size());
    benchmark::DoNotOptimize(hit_count);
}
BENCHMARK(BM_PropertyAddressCacheLookup)->Unit(benchmark::kMicrosecond);
