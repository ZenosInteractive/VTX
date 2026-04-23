// VTX SDK -- reader benchmarks.
//
// Scenarios
//   BM_ReaderSequentialScan  open + iterate every frame, touch each bucket list
//   BM_ReaderRandomAccess    random-access 100 frames per iteration
//
// Reports
//   time per iteration + items_per_second.  Items = frames visited for
//   sequential scan, random accesses for the random-access benchmark.
//
// Fixture
//   synth_10k.vtx is generated at build time by `vtx_sample_write` (10 000
//   frames of arena-schema data).  VTX_BENCH_FIXTURES_DIR is defined by
//   benchmarks/CMakeLists.txt via target_compile_definitions.

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_logger.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <string>

namespace {

    constexpr int32_t kFixtureFrameCount = 10'000;

    std::string FixturePath(const char* name) {
        return std::string(VTX_BENCH_FIXTURES_DIR) + "/" + name;
    }

    // Silence VTX::Logger Debug messages once (e.g. "Chunk N loaded into RAM").
    // Info/Warning/Error still print so real problems remain visible.  This runs
    // once at static init, before google/benchmark starts its measured loops.
    struct SilenceDebugLogsAtInit {
        SilenceDebugLogsAtInit() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsAtInit silence_debug_logs_at_init {};

} // namespace

// Open the .vtx and linearly iterate every frame, touching the bucket list
// so the reader is forced to deserialize every chunk along the way.  This
// measures end-to-end throughput of the chunked read path.
static void BM_ReaderSequentialScan(benchmark::State& state) {
    const std::string path = FixturePath("synth_10k.vtx");

    for (auto _ : state) {
        auto result = VTX::OpenReplayFile(path);
        if (!result) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }

        const int32_t total = result.reader->GetTotalFrames();
        int64_t bucket_count = 0;
        for (int32_t i = 0; i < total; ++i) {
            if (const auto* frame = result.reader->GetFrameSync(i)) {
                bucket_count += static_cast<int64_t>(frame->GetBuckets().size());
            }
        }
        benchmark::DoNotOptimize(bucket_count);
    }

    state.SetItemsProcessed(state.iterations() * kFixtureFrameCount);
}
BENCHMARK(BM_ReaderSequentialScan)->Unit(benchmark::kMillisecond);

// Open once (outside the measured loop), then random-access K frames per
// iteration.  Measures the seek-table lookup + chunk cache hit pattern under
// uniformly distributed access.  A real game-server rewind pattern would
// likely show better locality, so treat this as a worst-case datapoint.
static void BM_ReaderRandomAccess(benchmark::State& state) {
    const std::string path = FixturePath("synth_10k.vtx");
    auto result = VTX::OpenReplayFile(path);
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;

    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> dist(0, kFixtureFrameCount - 1);

    constexpr int kAccessesPerIter = 100;

    for (auto _ : state) {
        for (int i = 0; i < kAccessesPerIter; ++i) {
            if (const auto* frame = reader->GetFrameSync(dist(rng))) {
                benchmark::DoNotOptimize(frame);
            }
        }
    }

    state.SetItemsProcessed(state.iterations() * kAccessesPerIter);
}
BENCHMARK(BM_ReaderRandomAccess)->Unit(benchmark::kMicrosecond);
