// VTX SDK -- shared benchmark utilities.
//
// Pulls the repetitive plumbing out of individual bench files:
//   - WarmFileCache(path): forces the OS to page-cache the whole file so the
//     first measured iteration doesn't eat a cold-cache tax (main driver of
//     variance on big fixtures like cs_*.vtx).
//   - SetNsPerFrame(state, frames): exposes a per-frame CPU-time counter
//     that's easier to read than items_per_second and doesn't depend on
//     google/benchmark's implicit wall-vs-CPU choice.
//   - BENCH_HEAVY_FIXTURE(name): helper that pins a benchmark to 5
//     repetitions so mean/median/stddev/CV always show up without a CLI
//     flag.
//
// Header-only on purpose: each bench translation unit includes it; nothing
// lives in a separate .cpp.

#pragma once

#include <benchmark/benchmark.h>

#include <cstdint>
#include <fstream>
#include <string>

namespace VtxBench {

// Read the whole file into a throwaway buffer so the OS file cache is
// warm.  Cheap on repeats (~tens of ms even for 91 MB once cached).
// Call once in the benchmark body before the measured loop.
//
// The scratch buffer is heap-allocated on purpose: a 1 MB char array on
// the stack blows Windows' default 1 MB thread stack and the process
// terminates silently with no stderr output.  64 KB is plenty -- the OS
// read-ahead keeps I/O saturated even with a small working buffer.
inline void WarmFileCache(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    constexpr std::streamsize kChunk = 64 * 1024;  // 64 KB, safely on stack
    char buf[kChunk];
    while (in.read(buf, kChunk) || in.gcount() > 0) {
        benchmark::DoNotOptimize(buf);
    }
}

// Expose "nanoseconds of CPU time per frame" as a first-class counter.
// google/benchmark's items_per_second divides by CPU time; this flips the
// ratio and scales it to ns so the output reads directly as "X ns/frame".
inline void SetNsPerFrame(benchmark::State& state, int64_t total_frames) {
    state.counters["ns_per_frame"] = benchmark::Counter(
        static_cast<double>(total_frames),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert,
        benchmark::Counter::kIs1000);
}

}  // namespace VtxBench

// Apply to heavy fixture scans so mean/median/stddev/CV always appear.
//
//   BENCHMARK(BM_CS_ReaderSequentialScan_FBS)
//       ->Unit(benchmark::kMillisecond)
//       BENCH_HEAVY_FIXTURE_SUFFIX;
#define BENCH_HEAVY_FIXTURE_SUFFIX \
    ->Repetitions(5)->ReportAggregatesOnly(true)
