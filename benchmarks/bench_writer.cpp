// VTX SDK -- writer benchmarks.
//
// Scenarios
//   BM_WriterThroughput  record N synthetic frames into a fresh .vtx file
//
// Each iteration creates a new writer, records N frames, calls Stop().  The
// per-iteration includes file create + chunk finalisation + footer write, so
// the amortised frames/sec number is end-to-end, not just in-memory throughput.

#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_logger.h"

#include <benchmark/benchmark.h>

#include <filesystem>
#include <string>

namespace {

    constexpr int kFramesPerIteration = 1'000;

    std::string TempOutputPath() {
        return (std::filesystem::temp_directory_path() / "vtx_bench_writer_out.vtx").string();
    }

    std::string ArenaSchemaPath() {
        // benchmarks/ is two levels above the schema in samples/content/writer/arena/.
        // VTX_BENCH_FIXTURES_DIR = <repo>/benchmarks/fixtures so we walk up twice.
        return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path() / "samples" / "content" /
                "writer" / "arena" / "arena_schema.json")
            .string();
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_writer_debug_logs_once {};

} // namespace

// End-to-end throughput: create writer, record N frames, finalize.
static void BM_WriterThroughput(benchmark::State& state) {
    const std::string schema_path = ArenaSchemaPath();
    const std::string out_path = TempOutputPath();

    for (auto _ : state) {
        VTX::WriterFacadeConfig config;
        config.output_filepath = out_path;
        config.schema_json_path = schema_path;
        config.replay_name = "BenchmarkWriter";
        config.replay_uuid = "bench-0001";
        config.default_fps = 60.0f;
        config.chunk_max_frames = 500;
        config.use_compression = true;

        auto writer = VTX::CreateFlatBuffersWriterFacade(config);
        if (!writer) {
            state.SkipWithError("writer creation failed");
            break;
        }

        for (int i = 0; i < kFramesPerIteration; ++i) {
            VTX::Frame frame;
            auto& bucket = frame.CreateBucket("Players");

            VTX::PropertyContainer entity;
            entity.entity_type_id = 0;
            entity.float_properties.push_back(static_cast<float>(i) * 1.5f);

            VTX::Transform t;
            t.translation = {static_cast<double>(i), 0.0, 50.0};
            t.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            t.scale = {1.0, 1.0, 1.0};
            entity.transform_properties.push_back(t);

            bucket.unique_ids.push_back("player_" + std::to_string(i % 10));
            bucket.entities.push_back(std::move(entity));

            VTX::GameTime::GameTimeRegister game_time;
            game_time.game_time = static_cast<float>(i) / 60.0f;

            writer->RecordFrame(frame, game_time);
        }

        writer->Flush();
        writer->Stop();
    }

    state.SetItemsProcessed(state.iterations() * kFramesPerIteration);
    std::filesystem::remove(out_path);
}
BENCHMARK(BM_WriterThroughput)->Unit(benchmark::kMillisecond);
