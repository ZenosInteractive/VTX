// VTX SDK -- differ benchmarks.
//
// Scenarios
//   BM_DifferConsecutiveFrames  diff frame i vs i+1 using DiffRawFrames
//
// The arena simulation is deterministic and has high temporal locality --
// consecutive frames differ in a handful of transforms plus game-time.  This
// exercises the xxHash short-circuit path and typical tree-diff cost.

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/differ/core/vtx_differ_facade.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_logger.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>

namespace {

    std::string FixturePath(const char* name) {
        return std::string(VTX_BENCH_FIXTURES_DIR) + "/" + name;
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_differ_debug_logs_once {};

} // namespace

// Diff cost between two consecutive frames.  Opens the fixture once and the
// differ once; the measured loop only does DiffRawFrames over sliding pairs.
static void BM_DifferConsecutiveFrames(benchmark::State& state) {
    const std::string path = FixturePath("synth_10k.vtx");

    auto reader_result = VTX::OpenReplayFile(path);
    if (!reader_result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = reader_result.reader;

    auto differ = VtxDiff::CreateDifferFacade(VTX::VtxFormat::FlatBuffers);
    if (!differ) {
        state.SkipWithError("CreateDifferFacade failed");
        return;
    }

    const int32_t total = reader->GetTotalFrames();
    if (total < 2) {
        state.SkipWithError("fixture has fewer than 2 frames");
        return;
    }

    int32_t pair_index = 0;
    const int32_t max_pair = total - 1;

    for (auto _ : state) {
        const int32_t i = pair_index;
        pair_index = (pair_index + 1) % max_pair;

        auto frame_a = reader->GetRawFrameBytes(i);
        auto frame_b = reader->GetRawFrameBytes(i + 1);
        if (frame_a.empty() || frame_b.empty()) {
            state.SkipWithError("GetRawFrameBytes returned empty span");
            break;
        }

        auto patch = differ->DiffRawFrames(frame_a, frame_b);
        benchmark::DoNotOptimize(patch);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DifferConsecutiveFrames)->Unit(benchmark::kMicrosecond);
