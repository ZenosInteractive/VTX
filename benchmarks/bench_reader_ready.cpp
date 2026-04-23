// VTX SDK -- reader "ready" benchmarks.
//
// What the eager-chunk-0 warm changes
//   OpenReplayFile() used to return as soon as header + footer were parsed;
//   the first GetFrame* call then paid the full ZSTD decompress +
//   deserialise cost synchronously.  Now OpenReplayFile() kicks off an
//   async load of chunk 0 as part of opening, so the decompress runs on a
//   background thread and typically overlaps with caller initialisation.
//
// Scenarios
//   BM_ReaderOpenOnly             OpenReplayFile + return (no wait)
//   BM_ReaderOpenToReady          OpenReplayFile + WaitUntilReady
//   BM_ReaderOpenToFirstFrame     OpenReplayFile + GetFrameSync(0)
//
//   The gap between BM_ReaderOpenOnly and BM_ReaderOpenToReady is the
//   "how much chunk-0 work is already visible to the caller" -- low when
//   the OS file cache is warm, larger on first open.
//   BM_ReaderOpenToFirstFrame measures the same path a 0.1-style caller
//   still takes (no explicit wait); it should match BM_ReaderOpenToReady
//   closely because GetFrameSync falls through to the same sync path
//   when the async load is not yet in cache.
//
// Fixture
//   synth_10k.vtx, same fixture as bench_reader.cpp.  VTX_BENCH_FIXTURES_DIR
//   is set by benchmarks/CMakeLists.txt via target_compile_definitions.

#include "vtx/common/vtx_logger.h"
#include "vtx/reader/core/vtx_reader_facade.h"

#include "bench_utils.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <string>

namespace {

    std::string FixturePath(const char* name) { return std::string(VTX_BENCH_FIXTURES_DIR) + "/" + name; }

    struct SilenceDebugLogsAtInit {
        SilenceDebugLogsAtInit() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsAtInit silence_debug_logs_at_init {};

} // namespace

// Baseline: just open the file and immediately drop the context.  Measures
// the synchronous cost on the calling thread -- header + footer parse,
// property-address cache build, seek-table ingestion, plus the one-shot
// std::async spawn for the eager chunk-0 warm.  Should be sub-millisecond.
static void BM_ReaderOpenOnly(benchmark::State& state) {
    const std::string path = FixturePath("synth_10k.vtx");
    VtxBench::WarmFileCache(path);

    for (auto _ : state) {
        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }
        benchmark::DoNotOptimize(ctx.reader.get());
        // ctx goes out of scope here: reader dtor cancels the in-flight
        // chunk-0 load, so the measured cost here does not pay the
        // decompress.  That is intentional -- this benchmark isolates
        // the synchronous open path.
    }
}
BENCHMARK(BM_ReaderOpenOnly)->Unit(benchmark::kMicrosecond);

// OpenReplayFile + WaitUntilReady.  Measures the end-to-end "file is
// fully usable" latency, i.e. open + chunk-0 ZSTD decompress + FB /
// protobuf deserialise, serialised onto the calling thread via the cv
// wait.  This is the number to quote as "time to first frame".
static void BM_ReaderOpenToReady(benchmark::State& state) {
    const std::string path = FixturePath("synth_10k.vtx");
    VtxBench::WarmFileCache(path);

    for (auto _ : state) {
        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }
        const bool ready = ctx.WaitUntilReady(std::chrono::seconds(5));
        if (!ready) {
            state.SkipWithError("WaitUntilReady timed out");
            break;
        }
        benchmark::DoNotOptimize(ctx.IsReady());
    }
}
BENCHMARK(BM_ReaderOpenToReady)->Unit(benchmark::kMicrosecond);

// OpenReplayFile + GetFrameSync(0).  Mirrors what a pre-ready-API caller
// would do: no explicit ready wait, just ask for frame 0.  Under the
// eager-warm pipeline GetFrameSync either hits the cache (async worker
// finished first) or falls through to the sync load path; the caller
// sees a non-null Frame* in both cases.  The measured cost is dominated
// by ZSTD + deserialise, same as BM_ReaderOpenToReady -- by design, the
// two should track each other within noise.
static void BM_ReaderOpenToFirstFrame(benchmark::State& state) {
    const std::string path = FixturePath("synth_10k.vtx");
    VtxBench::WarmFileCache(path);

    for (auto _ : state) {
        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }
        const VTX::Frame* f = ctx.reader->GetFrameSync(0);
        if (!f) {
            state.SkipWithError("GetFrameSync(0) returned null");
            break;
        }
        benchmark::DoNotOptimize(f);
    }
}
BENCHMARK(BM_ReaderOpenToFirstFrame)->Unit(benchmark::kMicrosecond);
