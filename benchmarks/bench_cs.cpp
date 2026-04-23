// VTX SDK -- Counter-Strike replay benchmarks.
//
// Fixtures
//   samples/content/reader/cs/cs_fbs.vtx    ~91 MB, FlatBuffers backend
//   samples/content/reader/cs/cs_proto.vtx  ~83 MB, Protobuf backend
//   Both replays have the same frame count (~10,656) -- the size delta is
//   just encoding + compression, not content.
//
// Why CS is interesting: far richer than the arena synthetic data --
//   * 3 buckets per frame ("entity", "bonedata", "economy")
//   * Nested structs (Player -> Equipment -> Weapons[])
//   * ~100+ bones per character via OptimizedBoneData (int8 array, ~KB/entity)
//   * Much bigger per-frame payload -> realistic worst-case for chunk I/O
//
// Reliability plumbing (see bench_utils.h):
//   * Pre-warm the OS file cache before every measured iteration to remove
//     cold-cache outliers
//   * Report ns_per_frame directly (CPU time / frames) instead of leaving
//     readers to divide items_per_second by hand
//   * Heavy scans always run with 5 repetitions so mean/median/stddev/CV
//     show up without CLI flags

#include "bench_utils.h"

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/reader/core/vtx_frame_accessor.h"
#include "vtx/differ/core/vtx_differ_facade.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_types.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

namespace {

    std::string CsReplayPath(const char* backend_filename) {
        return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path() / "samples" / "content" /
                "reader" / "cs" / backend_filename)
            .string();
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_cs_debug_logs_once {};

    // Opens + iterates every frame + touches the bucket list.  Mirrors the
    // BM_ReaderSequentialScan shape but against a realistic fixture.
    void SequentialScanImpl(benchmark::State& state, const char* filename) {
        const std::string path = CsReplayPath(filename);
        if (!std::filesystem::exists(path)) {
            state.SkipWithError("CS fixture missing (expected under samples/content/reader/cs/)");
            return;
        }
        VtxBench::WarmFileCache(path);

        int64_t total_frames = 0;
        for (auto _ : state) {
            auto result = VTX::OpenReplayFile(path);
            if (!result) {
                state.SkipWithError("OpenReplayFile failed");
                break;
            }
            total_frames = result.reader->GetTotalFrames();
            int64_t bucket_count = 0;
            for (int32_t i = 0; i < total_frames; ++i) {
                if (const auto* frame = result.reader->GetFrameSync(i)) {
                    bucket_count += static_cast<int64_t>(frame->GetBuckets().size());
                }
            }
            benchmark::DoNotOptimize(bucket_count);
        }

        state.SetItemsProcessed(state.iterations() * total_frames);
        VtxBench::SetNsPerFrame(state, total_frames);
    }

} // namespace

static void BM_CS_ReaderSequentialScan_FBS(benchmark::State& state) {
    SequentialScanImpl(state, "cs_fbs.vtx");
}
BENCHMARK(BM_CS_ReaderSequentialScan_FBS)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

static void BM_CS_ReaderSequentialScan_Proto(benchmark::State& state) {
    SequentialScanImpl(state, "cs_proto.vtx");
}
BENCHMARK(BM_CS_ReaderSequentialScan_Proto)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

// Full user-facing path: accessor + read Player::Transform + Player::Health
// + Player::UniqueID per Player entity across every frame.
static void BM_CS_AccessorSequentialScan_FBS(benchmark::State& state) {
    const std::string path = CsReplayPath("cs_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("CS fixture missing");
        return;
    }
    VtxBench::WarmFileCache(path);

    int64_t total_frames = 0;
    for (auto _ : state) {
        auto result = VTX::OpenReplayFile(path);
        if (!result) {
            state.SkipWithError("OpenReplayFile failed");
            break;
        }
        auto& reader = result.reader;

        auto accessor = reader->CreateAccessor();
        auto key_transform = accessor.Get<VTX::Transform>("Player", "Transform");
        auto key_health = accessor.Get<int32_t>("Player", "Health");
        auto key_unique_id = accessor.Get<std::string>("Player", "UniqueID");
        auto key_steam_id = accessor.Get<int64_t>("Player", "SteamId");

        if (!key_transform.IsValid() || !key_health.IsValid() || !key_unique_id.IsValid() || !key_steam_id.IsValid()) {
            state.SkipWithError("Player keys did not resolve");
            break;
        }

        total_frames = reader->GetTotalFrames();
        double tx_accum = 0.0;
        int64_t hp_accum = 0;
        size_t id_count = 0;
        int64_t sid_sum = 0;

        for (int32_t i = 0; i < total_frames; ++i) {
            const auto* frame = reader->GetFrameSync(i);
            if (!frame)
                continue;
            for (const auto& bucket : frame->GetBuckets()) {
                for (const auto& entity : bucket.entities) {
                    VTX::EntityView view(entity);
                    tx_accum += view.Get(key_transform).translation.x;
                    hp_accum += view.Get(key_health);
                    id_count += view.Get(key_unique_id).size();
                    sid_sum += view.Get(key_steam_id);
                }
            }
        }
        benchmark::DoNotOptimize(tx_accum);
        benchmark::DoNotOptimize(hp_accum);
        benchmark::DoNotOptimize(id_count);
        benchmark::DoNotOptimize(sid_sum);
    }

    state.SetItemsProcessed(state.iterations() * total_frames);
    VtxBench::SetNsPerFrame(state, total_frames);
}
BENCHMARK(BM_CS_AccessorSequentialScan_FBS)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

// Random access through the accessor: open + keep the reader alive, then
// pick uniformly random frames and read Player::Transform + Health per
// Player entity.  Models a scrubbing UI jumping around the replay.
static void BM_CS_AccessorRandomAccess_FBS(benchmark::State& state) {
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
}
BENCHMARK(BM_CS_AccessorRandomAccess_FBS)->Unit(benchmark::kMicrosecond);

// Consecutive-frame diff on a realistic CS payload.  Bone data changes
// every frame so the xxHash short-circuit cannot prune as aggressively as
// in the arena case -- this is the "real" diff cost on non-trivial frames.
static void BM_CS_DifferConsecutiveFrames_FBS(benchmark::State& state) {
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
BENCHMARK(BM_CS_DifferConsecutiveFrames_FBS)->Unit(benchmark::kMicrosecond);
