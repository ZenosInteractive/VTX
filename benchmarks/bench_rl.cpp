// VTX SDK -- Rocket League replay benchmarks.
//
// Fixtures
//   samples/content/reader/rl/rl_fbs.vtx    ~4.8 MB, FlatBuffers backend
//   samples/content/reader/rl/rl_proto.vtx  ~5.1 MB, Protobuf backend
//   Both replays have the same frame count (~20,364) -- size delta is
//   encoding + compression only.
//
// Why RL matters: middle-tier fixture between tiny arena (1.8 MB) and
// heavy CS (91 MB).  Schema has Car / Ball / FX_Car / FX_Ball / Material /
// VehiclePickup_Boost / ... but no bone data so payloads per frame are an
// order of magnitude smaller than CS.
//
// Reliability plumbing shared with bench_cs.cpp via bench_utils.h.

#include "bench_utils.h"

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_frame_accessor.h"
#include "vtx/differ/core/vtx_differ_facade.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_types.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

namespace {

    std::string RlReplayPath(const char* backend_filename) {
        return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path() / "samples" / "content" /
                "reader" / "rl" / backend_filename)
            .string();
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_rl_debug_logs_once {};

    // Raw reader scan: open + iterate every frame + touch bucket list.
    void SequentialScanImpl(benchmark::State& state, const char* filename) {
        const std::string path = RlReplayPath(filename);
        if (!std::filesystem::exists(path)) {
            state.SkipWithError("RL fixture missing (expected under samples/content/reader/rl/)");
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

static void BM_RL_ReaderSequentialScan_FBS(benchmark::State& state) {
    SequentialScanImpl(state, "rl_fbs.vtx");
}
BENCHMARK(BM_RL_ReaderSequentialScan_FBS)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

static void BM_RL_ReaderSequentialScan_Proto(benchmark::State& state) {
    SequentialScanImpl(state, "rl_proto.vtx");
}
BENCHMARK(BM_RL_ReaderSequentialScan_Proto)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

// Full user-facing path: accessor + read 4 keys per entity (Transform,
// ActorId, UniqueId, BoostAmount).  Entity struct properties are inlined
// across Car / Ball / FX_Car / FX_Ball so a lookup against "Entity" works
// for all of them.
static void BM_RL_AccessorSequentialScan_FBS(benchmark::State& state) {
    const std::string path = RlReplayPath("rl_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("RL fixture missing");
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
        auto key_transform = accessor.Get<VTX::Transform>("Entity", "Transform");
        auto key_actor_id = accessor.Get<int64_t>("Entity", "ActorId");
        auto key_unique_id = accessor.Get<std::string>("Entity", "UniqueId");
        auto key_boost = accessor.Get<float>("VehiclePickup_Boost", "BoostAmount");

        if (!key_transform.IsValid() || !key_actor_id.IsValid() || !key_unique_id.IsValid()) {
            state.SkipWithError("Entity keys did not resolve");
            break;
        }

        total_frames = reader->GetTotalFrames();
        double tx_accum = 0.0;
        int64_t aid_accum = 0;
        size_t id_count = 0;
        float boost_accum = 0.0f;

        for (int32_t i = 0; i < total_frames; ++i) {
            const auto* frame = reader->GetFrameSync(i);
            if (!frame)
                continue;
            for (const auto& bucket : frame->GetBuckets()) {
                for (const auto& entity : bucket.entities) {
                    VTX::EntityView view(entity);
                    tx_accum += view.Get(key_transform).translation.x;
                    aid_accum += view.Get(key_actor_id);
                    id_count += view.Get(key_unique_id).size();
                    boost_accum += view.Get(key_boost);
                }
            }
        }
        benchmark::DoNotOptimize(tx_accum);
        benchmark::DoNotOptimize(aid_accum);
        benchmark::DoNotOptimize(id_count);
        benchmark::DoNotOptimize(boost_accum);
    }

    state.SetItemsProcessed(state.iterations() * total_frames);
    VtxBench::SetNsPerFrame(state, total_frames);
}
BENCHMARK(BM_RL_AccessorSequentialScan_FBS)->Unit(benchmark::kMillisecond) BENCH_HEAVY_FIXTURE_SUFFIX;

// Random access via accessor: simulates a replay-viewer scrub jumping
// around arbitrary frames and reading Transform + ActorId per entity.
static void BM_RL_AccessorRandomAccess_FBS(benchmark::State& state) {
    const std::string path = RlReplayPath("rl_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("RL fixture missing");
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
    auto key_transform = accessor.Get<VTX::Transform>("Entity", "Transform");
    auto key_actor_id = accessor.Get<int64_t>("Entity", "ActorId");
    if (!key_transform.IsValid() || !key_actor_id.IsValid()) {
        state.SkipWithError("Entity keys did not resolve");
        return;
    }

    const int32_t total = reader->GetTotalFrames();
    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> dist(0, total - 1);

    constexpr int kAccessesPerIter = 50;
    double tx_accum = 0.0;
    int64_t aid_accum = 0;

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
                    aid_accum += view.Get(key_actor_id);
                }
            }
        }
    }
    benchmark::DoNotOptimize(tx_accum);
    benchmark::DoNotOptimize(aid_accum);
    state.SetItemsProcessed(state.iterations() * kAccessesPerIter);
}
BENCHMARK(BM_RL_AccessorRandomAccess_FBS)->Unit(benchmark::kMicrosecond);

// Consecutive-frame diff on RL payload -- Transforms change every frame
// for all moving actors but the schema is much smaller than CS.
static void BM_RL_DifferConsecutiveFrames_FBS(benchmark::State& state) {
    const std::string path = RlReplayPath("rl_fbs.vtx");
    if (!std::filesystem::exists(path)) {
        state.SkipWithError("RL fixture missing");
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
BENCHMARK(BM_RL_DifferConsecutiveFrames_FBS)->Unit(benchmark::kMicrosecond);
