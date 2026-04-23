// VTX SDK -- accessor-layer benchmarks.
//
// These measure the canonical consumer path: FrameAccessor + PropertyKey<T>
// resolved once, then EntityView::Get(key) hot-loop per entity.  This is
// what a real integration looks like, as opposed to bench_reader.cpp which
// only enumerates frames and buckets without touching properties.
//
// Fixture
//   samples/content/reader/arena/arena_from_fbs_ds.vtx  (3600 frames of
//   arena-simulator data with schema-consistent Player / Projectile /
//   MatchState entities, committed to the repo by the sample pipeline).
//
// Scenarios
//   BM_AccessorSequentialScan  open + accessor + enumerate frames + read
//                              Position (Vector) + Health (float) + UniqueID
//                              (string) per Player entity
//   BM_AccessorHotLoopPreloaded  pre-load all frames into RAM, then measure
//                                the property-access hot path in isolation
//                                (no disk I/O, no chunk cache effects)
//   BM_AccessorRandomWithinBucket  shuffled entity indices inside a single
//                                  bucket -- forces entity-level cache miss
//                                  patterns while keeping everything RAM-
//                                  resident

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/reader/core/vtx_frame_accessor.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_types.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

    // The arena replay lives under samples/content/reader/arena/.  Benchmarks
    // run from various working dirs, so we resolve the path relative to the
    // fixtures dir that CMake exposes via VTX_BENCH_FIXTURES_DIR.
    std::string ArenaReplayPath() {
        return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path() / "samples" / "content" /
                "reader" / "arena" / "arena_from_fbs_ds.vtx")
            .string();
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_accessor_debug_logs_once {};

    // Small bundle that holds the reader + accessor + resolved keys.  Moving
    // this construction out of the measured loop is intentional -- a real
    // integration resolves keys once at setup, not per frame.
    struct AccessorFixture {
        VTX::ReaderContext reader_result;
        VTX::FrameAccessor accessor;
        VTX::PropertyKey<VTX::Vector> key_position;
        VTX::PropertyKey<float> key_health;
        VTX::PropertyKey<std::string> key_unique_id;

        static AccessorFixture Load(benchmark::State& state) {
            AccessorFixture f;
            f.reader_result = VTX::OpenReplayFile(ArenaReplayPath());
            if (!f.reader_result) {
                state.SkipWithError("OpenReplayFile failed (run vtx_sample_generate + vtx_sample_advance_write first)");
                return f;
            }
            f.accessor = f.reader_result.reader->CreateAccessor();
            f.key_position = f.accessor.Get<VTX::Vector>("Player", "Position");
            f.key_health = f.accessor.Get<float>("Player", "Health");
            f.key_unique_id = f.accessor.Get<std::string>("Player", "UniqueID");

            if (!f.key_position.IsValid() || !f.key_health.IsValid() || !f.key_unique_id.IsValid()) {
                state.SkipWithError("one or more Player keys did not resolve");
            }
            return f;
        }
    };

} // namespace

// Open the replay, create the accessor, resolve three keys, then linearly
// enumerate every frame, iterate every entity in the "entity" bucket, and
// read Position + Health + UniqueID via EntityView per Player entity.
// Measures the end-to-end user-facing throughput.
static void BM_AccessorSequentialScan(benchmark::State& state) {
    int32_t total_frames = 0;
    for (auto _ : state) {
        auto fixture = AccessorFixture::Load(state);
        if (!fixture.reader_result)
            break;
        auto& reader = fixture.reader_result.reader;

        total_frames = reader->GetTotalFrames();
        double pos_accum = 0.0;
        float hp_accum = 0.0f;
        size_t id_count = 0;

        for (int32_t i = 0; i < total_frames; ++i) {
            const auto* frame = reader->GetFrameSync(i);
            if (!frame)
                continue;
            for (const auto& bucket : frame->GetBuckets()) {
                for (const auto& entity : bucket.entities) {
                    if (entity.entity_type_id != 0)
                        continue; // 0 = Player
                    VTX::EntityView view(entity);
                    pos_accum += view.Get(fixture.key_position).x;
                    hp_accum += view.Get(fixture.key_health);
                    id_count += view.Get(fixture.key_unique_id).size();
                }
            }
        }
        benchmark::DoNotOptimize(pos_accum);
        benchmark::DoNotOptimize(hp_accum);
        benchmark::DoNotOptimize(id_count);
    }

    state.SetItemsProcessed(state.iterations() * total_frames);
}
BENCHMARK(BM_AccessorSequentialScan)->Unit(benchmark::kMillisecond);

// Pre-load every frame into RAM (via GetFrameRange), then hot-loop reading
// Position from every Player entity.  Isolates the FrameAccessor cost from
// I/O + chunk cache entirely -- the number is what a game that already has
// the replay in memory would observe when scrubbing over the properties.
static void BM_AccessorHotLoopPreloaded(benchmark::State& state) {
    auto result = VTX::OpenReplayFile(ArenaReplayPath());
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;

    auto accessor = reader->CreateAccessor();
    auto key_position = accessor.Get<VTX::Vector>("Player", "Position");
    auto key_health = accessor.Get<float>("Player", "Health");
    if (!key_position.IsValid() || !key_health.IsValid()) {
        state.SkipWithError("keys did not resolve");
        return;
    }

    // Pre-load every frame into a RAM vector using GetFrameSync (blocking),
    // which waits for chunks to load.  GetFrameRange uses the non-blocking
    // GetFrame internally and would return empty on a cold cache.
    const int32_t total = reader->GetTotalFrames();
    std::vector<VTX::Frame> ram_cache;
    ram_cache.reserve(total);
    for (int32_t i = 0; i < total; ++i) {
        if (const auto* f = reader->GetFrameSync(i)) {
            ram_cache.push_back(*f);
        }
    }
    if (ram_cache.empty()) {
        state.SkipWithError("pre-load returned empty");
        return;
    }

    // Pre-count how many Player entities are covered per full sweep so the
    // items_per_second counter reflects real entity reads, not frames.
    int64_t entities_per_sweep = 0;
    for (const auto& frame : ram_cache) {
        for (const auto& bucket : frame.GetBuckets()) {
            for (const auto& entity : bucket.entities) {
                if (entity.entity_type_id == 0)
                    ++entities_per_sweep;
            }
        }
    }

    double sink = 0.0;
    for (auto _ : state) {
        for (const auto& frame : ram_cache) {
            for (const auto& bucket : frame.GetBuckets()) {
                for (const auto& entity : bucket.entities) {
                    if (entity.entity_type_id != 0)
                        continue;
                    VTX::EntityView view(entity);
                    sink += view.Get(key_position).x + view.Get(key_health);
                }
            }
        }
    }
    benchmark::DoNotOptimize(sink);
    state.SetItemsProcessed(state.iterations() * entities_per_sweep);
}
BENCHMARK(BM_AccessorHotLoopPreloaded)->Unit(benchmark::kMillisecond);

// Same pre-loaded frames, but shuffle the entity indices inside each bucket
// before reading.  Mimics a worst-case access pattern where entity slots
// are visited in random order -- probes the SoA layout for CPU-cache
// behaviour under non-sequential access.
static void BM_AccessorRandomWithinBucket(benchmark::State& state) {
    auto result = VTX::OpenReplayFile(ArenaReplayPath());
    if (!result) {
        state.SkipWithError("OpenReplayFile failed");
        return;
    }
    auto& reader = result.reader;

    auto accessor = reader->CreateAccessor();
    auto key_position = accessor.Get<VTX::Vector>("Player", "Position");
    if (!key_position.IsValid()) {
        state.SkipWithError("Player::Position did not resolve");
        return;
    }

    // Same pre-load pattern as BM_AccessorHotLoopPreloaded -- blocking
    // GetFrameSync instead of GetFrameRange to warm up the chunk cache.
    const int32_t total = reader->GetTotalFrames();
    std::vector<VTX::Frame> ram_cache;
    ram_cache.reserve(total);
    for (int32_t i = 0; i < total; ++i) {
        if (const auto* f = reader->GetFrameSync(i)) {
            ram_cache.push_back(*f);
        }
    }
    if (ram_cache.empty()) {
        state.SkipWithError("pre-load returned empty");
        return;
    }

    // Pre-shuffle indices once per bucket so the measured loop doesn't pay
    // RNG cost.
    std::mt19937 rng(42);
    struct PreparedBucket {
        const std::vector<VTX::PropertyContainer>* entities;
        std::vector<size_t> shuffled;
    };
    std::vector<PreparedBucket> prepared;
    for (const auto& frame : ram_cache) {
        for (const auto& bucket : frame.GetBuckets()) {
            std::vector<size_t> idxs;
            idxs.reserve(bucket.entities.size());
            for (size_t i = 0; i < bucket.entities.size(); ++i) {
                if (bucket.entities[i].entity_type_id == 0)
                    idxs.push_back(i);
                if (bucket.entities[i].entity_type_id == 0)
                    idxs.push_back(i);
            }
            if (idxs.size() < 2)
                continue;
            std::shuffle(idxs.begin(), idxs.end(), rng);
            prepared.push_back({&bucket.entities, std::move(idxs)});
        }
    }
    int64_t ops_per_sweep = 0;
    for (const auto& pb : prepared)
        ops_per_sweep += static_cast<int64_t>(pb.shuffled.size());

    double sink = 0.0;
    for (auto _ : state) {
        for (const auto& pb : prepared) {
            for (size_t idx : pb.shuffled) {
                VTX::EntityView view((*pb.entities)[idx]);
                sink += view.Get(key_position).x;
            }
        }
    }
    benchmark::DoNotOptimize(sink);
    state.SetItemsProcessed(state.iterations() * ops_per_sweep);
}
BENCHMARK(BM_AccessorRandomWithinBucket)->Unit(benchmark::kMillisecond);
