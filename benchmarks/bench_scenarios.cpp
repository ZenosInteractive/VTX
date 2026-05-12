// VTX SDK -- realistic scenario benchmarks.
//
// The bench_reader / bench_cs / bench_rl files cover the raw "open + scan
// every frame" path.  Real integrations rarely do that.  This file covers
// the shapes that actually show up in product code:
//
//   * Preview / thumbnail        -- read the first N frames of a replay.
//   * Seek + play window         -- jump to 50% and play the next N frames.
//   * Strided timeline scrub     -- read every Kth frame for a timeline UI.
//   * Cache-window sensitivity   -- random access with different cache sizes.
//   * Differ edge cases          -- identical frames (xxHash short-circuit)
//                                   vs. first-vs-last (max-change case).
//
// All CS scenarios pre-warm the OS file cache and use the heavy-fixture
// repetitions suffix so mean/median/stddev/CV show up in the output.
//
// Why this matters for open-source posture: the top-line numbers in the
// README come from BM_CS_ReaderSequentialScan_* (full-file worst case).
// Downstream users rarely hit that path -- they scrub, they preview, they
// sample.  Without these scenarios the benchmark suite overstates the
// cost of everyday use.

#include "bench_utils.h"

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_frame_accessor.h"
#include "vtx/differ/core/vtx_differ_facade.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_types.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

    std::string CsReplayPath(const char* backend_filename) {
        return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path() / "samples" / "content" /
                "reader" / "cs" / backend_filename)
            .string();
    }

    std::string ArenaReplayPath() {
        return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path() / "samples" / "content" /
                "reader" / "arena" / "arena_from_fbs_ds.vtx")
            .string();
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_scenarios_debug_logs_once {};

} // namespace

// ============================================================================
// Partial / windowed scans (CS fixture)
// ============================================================================
//
// These model the canonical "I don't need the whole replay" paths.  Each
// opens the fixture inside the measured loop so the reported time includes
// header parsing + seek-table hydration -- the honest cost of a cold open.

// "Generate a preview" -- read the first 1000 frames, touch the bucket list.
// Accounts for ~9% of the full CS fixture (10,656 frames).
static void BM_CS_PreviewFirst1000Frames_FBS(benchmark::State& state) {
    const std::string path = CsReplayPath("cs_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("CS fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    constexpr int32_t kPreviewFrames = 1000;
    for (auto _ : state) {
        auto result = VTX::OpenReplayFile(path);
        if (!result) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }
        const int32_t limit = std::min(kPreviewFrames, result.reader->GetTotalFrames());
        int64_t bucket_count = 0;
        for (int32_t i = 0; i < limit; ++i) {
            if (const auto* frame = result.reader->GetFrameSync(i)) {
                bucket_count += static_cast<int64_t>(frame->GetBuckets().size());
            }
        }
        benchmark::DoNotOptimize(bucket_count);
    }
    state.SetItemsProcessed(state.iterations() * kPreviewFrames);
    VtxBench::SetNsPerFrame(state, kPreviewFrames);
}
BENCHMARK(BM_CS_PreviewFirst1000Frames_FBS)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

// "User seeks to 50% and plays 300 frames" -- typical scrubbing-UI shape.
// The middle-of-file seek forces a cold chunk load even on a warm OS cache,
// so this reveals the steady-state chunk fetch cost rather than the
// first-frame-after-open tax.
static void BM_CS_SeekMiddlePlay300Frames_FBS(benchmark::State& state) {
    const std::string path = CsReplayPath("cs_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("CS fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    constexpr int32_t kPlayFrames = 300;
    for (auto _ : state) {
        auto result = VTX::OpenReplayFile(path);
        if (!result) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }
        const int32_t total = result.reader->GetTotalFrames();
        const int32_t start = total / 2;
        const int32_t end = std::min(total, start + kPlayFrames);
        int64_t bucket_count = 0;
        for (int32_t i = start; i < end; ++i) {
            if (const auto* frame = result.reader->GetFrameSync(i)) {
                bucket_count += static_cast<int64_t>(frame->GetBuckets().size());
            }
        }
        benchmark::DoNotOptimize(bucket_count);
    }
    state.SetItemsProcessed(state.iterations() * kPlayFrames);
    VtxBench::SetNsPerFrame(state, kPlayFrames);
}
BENCHMARK(BM_CS_SeekMiddlePlay300Frames_FBS)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

// "Timeline thumbnails" -- one frame every 100, covering the whole replay.
// Each sample is in a different chunk, so this is effectively a chunk-miss
// benchmark (unlike sequential scan which amortizes the miss across a
// chunk's worth of frames).
static void BM_CS_StridedScan_Every100th_FBS(benchmark::State& state) {
    const std::string path = CsReplayPath("cs_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("CS fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    constexpr int32_t kStride = 100;
    int64_t samples_total = 0;
    for (auto _ : state) {
        auto result = VTX::OpenReplayFile(path);
        if (!result) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }
        const int32_t total = result.reader->GetTotalFrames();
        int64_t bucket_count = 0;
        int64_t samples = 0;
        for (int32_t i = 0; i < total; i += kStride) {
            if (const auto* frame = result.reader->GetFrameSync(i)) {
                bucket_count += static_cast<int64_t>(frame->GetBuckets().size());
                ++samples;
            }
        }
        samples_total = samples;
        benchmark::DoNotOptimize(bucket_count);
    }
    state.SetItemsProcessed(state.iterations() * samples_total);
    VtxBench::SetNsPerFrame(state, samples_total);
}
BENCHMARK(BM_CS_StridedScan_Every100th_FBS)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

// ============================================================================
// Cache-window sweep (CS fixture)
// ============================================================================
//
// SetCacheWindow(backward, forward) controls how many chunks the reader
// keeps resident around the current position.  Random access with a small
// window thrashes the cache (every jump misses); with a large window most
// jumps hit.  This benchmark parameterises over the window size via
// google/benchmark's ->Arg(N), so the output is a table:
//
//   BM_CS_AccessorRandomAccess_CacheSweep_FBS/0    <cold>
//   BM_CS_AccessorRandomAccess_CacheSweep_FBS/2
//   BM_CS_AccessorRandomAccess_CacheSweep_FBS/5
//   BM_CS_AccessorRandomAccess_CacheSweep_FBS/10
//   BM_CS_AccessorRandomAccess_CacheSweep_FBS/20
//
// The argument is passed symmetrically: SetCacheWindow(N, N).  That matches
// the shape of a scrubbing UI that can jump in either direction.
static void BM_CS_AccessorRandomAccess_CacheSweep_FBS(benchmark::State& state) {
    const int32_t cache_window = static_cast<int32_t>(state.range(0));

    const std::string path = CsReplayPath("cs_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("CS fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    auto result = VTX::OpenReplayFile(path);
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;
    reader->SetCacheWindow(static_cast<uint32_t>(cache_window), static_cast<uint32_t>(cache_window));

    auto accessor = reader->CreateAccessor();
    auto key_transform = accessor.Get<VTX::Transform>("Player", "Transform");
    auto key_health = accessor.Get<int32_t>("Player", "Health");
    if (!key_transform.IsValid() || !key_health.IsValid()) {
        state.SkipWithError("Player keys did not resolve");
        return;
    }

    const int32_t total = reader->GetTotalFrames();
    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> dist(0, total - 1);

    constexpr int kAccessesPerIter = 50;
    double tx_accum = 0.0;
    int64_t hp_accum = 0;

    for (auto _ : state) {
        for (int i = 0; i < kAccessesPerIter; ++i) {
            const int32_t idx = dist(rng);
            const auto* frame = reader->GetFrameSync(idx);
            if (!frame)
                continue;
            for (const auto& bucket : frame->GetBuckets()) {
                for (const auto& entity : bucket.entities) {
                    VTX::EntityView view(entity);
                    tx_accum += view.Get(key_transform).translation.x;
                    hp_accum += view.Get(key_health);
                }
            }
        }
    }
    benchmark::DoNotOptimize(tx_accum);
    benchmark::DoNotOptimize(hp_accum);
    state.SetItemsProcessed(state.iterations() * kAccessesPerIter);
    state.counters["cache_window"] = benchmark::Counter(static_cast<double>(cache_window));
}
// NOTE: Arg(2) and Arg(5) are ~4x slower than Arg(0) on CS because the
// symmetric (N,N) window is smaller than the random-access span but large
// enough to thrash -- every jump triggers async prefetches + evictions of
// recently-loaded chunks.  Arg(10) / Arg(20) cover all ~10 CS chunks so
// steady state is all-cache-hits.  The middle windows *are* the story --
// they document the trap of "some cache is better than no cache" (it's not).
//
// Reps aren't useful here: the RNG is seeded (seed=42) so the access
// sequence is identical across runs.  Single iteration is deterministic.
BENCHMARK(BM_CS_AccessorRandomAccess_CacheSweep_FBS)
    ->Arg(0)
    ->Arg(2)
    ->Arg(5)
    ->Arg(10)
    ->Arg(20)
    ->Unit(benchmark::kMicrosecond);

// ============================================================================
// Differ edge cases
// ============================================================================
//
// Arena fixture is tiny (~1.8 MB) so repeating open/accessor/differ per
// iteration is cheap.  We use the CS fixture for the "realistic heavy" case.

// Helper: snapshot a raw frame into an owning buffer so subsequent cache
// eviction can't invalidate our pointer.  GetRawFrameBytes returns a span
// that aliases chunk_cache_[chunk].raw_frames_spans[i].  Calling
// GetRawFrameBytes for a *different* frame can trigger cache-window
// eviction that silently frees the previous chunk, leaving earlier spans
// dangling.  Copying the bytes once, pre-measured loop, keeps the diff
// path free of that footgun.  (It also makes the measured number "pure
// diff cost" instead of "diff + cache lookup + potential chunk reload".)
static std::vector<std::byte> SnapshotFrame(VTX::IVtxReaderFacade* reader, int32_t frame_index) {
    auto span = reader->GetRawFrameBytes(frame_index);
    return std::vector<std::byte>(span.begin(), span.end());
}

// Identical-frame diff: exercises the xxHash short-circuit at the top of
// DiffRawFrames.  Should be near-zero work -- the span hash comparison
// aborts before any tree walk.
static void BM_Arena_DifferIdenticalFrames_FBS(benchmark::State& state) {
    const std::string path = ArenaReplayPath();
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("Arena fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    auto result = VTX::OpenReplayFile(path);
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;

    auto differ = VtxDiff::CreateDifferFacade(VTX::VtxFormat::FlatBuffers);
    if (!differ) {
        state.SkipWithError("CreateDifferFacade failed");
        return;
    }

    auto frame_a = SnapshotFrame(reader.get(), 0);
    if (frame_a.empty()) {
        state.SkipWithError("GetRawFrameBytes returned empty");
        return;
    }

    for (auto _ : state) {
        auto patch = differ->DiffRawFrames(frame_a, frame_a);
        benchmark::DoNotOptimize(patch);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Arena_DifferIdenticalFrames_FBS)->Unit(benchmark::kNanosecond);

// First-vs-last diff: largest reasonable content delta inside one replay --
// different player positions, different game state.  Upper bound on the
// arena differ cost on real data.
static void BM_Arena_DifferFirstVsLast_FBS(benchmark::State& state) {
    const std::string path = ArenaReplayPath();
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("Arena fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    auto result = VTX::OpenReplayFile(path);
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;

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

    // Copy *both* frames out before the measured loop.  The first span
    // becomes invalid the moment the second call evicts its chunk.
    auto frame_a = SnapshotFrame(reader.get(), 0);
    auto frame_b = SnapshotFrame(reader.get(), total - 1);
    if (frame_a.empty() || frame_b.empty()) {
        state.SkipWithError("GetRawFrameBytes returned empty");
        return;
    }

    for (auto _ : state) {
        auto patch = differ->DiffRawFrames(frame_a, frame_b);
        benchmark::DoNotOptimize(patch);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Arena_DifferFirstVsLast_FBS)->Unit(benchmark::kMicrosecond);

// CS identical-frame diff: the xxHash short-circuit on a much larger
// payload.  If the hash path is correctly implemented this should be
// roughly flat with payload size -- the pay is in hashing, not walking.
static void BM_CS_DifferIdenticalFrames_FBS(benchmark::State& state) {
    const std::string path = CsReplayPath("cs_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("CS fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    auto result = VTX::OpenReplayFile(path);
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;

    auto differ = VtxDiff::CreateDifferFacade(VTX::VtxFormat::FlatBuffers);
    if (!differ) {
        state.SkipWithError("CreateDifferFacade failed");
        return;
    }

    auto frame_a = SnapshotFrame(reader.get(), 0);
    if (frame_a.empty()) {
        state.SkipWithError("GetRawFrameBytes returned empty");
        return;
    }

    for (auto _ : state) {
        auto patch = differ->DiffRawFrames(frame_a, frame_a);
        benchmark::DoNotOptimize(patch);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CS_DifferIdenticalFrames_FBS)->Unit(benchmark::kMicrosecond);

// CS first-vs-last diff: worst-case content delta on the heavy fixture.
// Bone data, positions, equipment -- everything changes.  This is the
// upper bound on what DiffRawFrames costs for realistic payloads.
static void BM_CS_DifferFirstVsLast_FBS(benchmark::State& state) {
    const std::string path = CsReplayPath("cs_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("CS fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    auto result = VTX::OpenReplayFile(path);
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;

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

    auto frame_a = SnapshotFrame(reader.get(), 0);
    auto frame_b = SnapshotFrame(reader.get(), total - 1);
    if (frame_a.empty() || frame_b.empty()) {
        state.SkipWithError("GetRawFrameBytes returned empty");
        return;
    }

    for (auto _ : state) {
        auto patch = differ->DiffRawFrames(frame_a, frame_b);
        benchmark::DoNotOptimize(patch);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CS_DifferFirstVsLast_FBS)->Unit(benchmark::kMicrosecond);
