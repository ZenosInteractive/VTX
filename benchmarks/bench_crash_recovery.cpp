// VTX SDK -- crash-recovery durability-tier benchmarks.
//
// Scenarios (same recording, three sink configurations)
//   BM_WriterDurabilityTier/journal_off   no .recovery sidecar (pre-feature baseline)
//   BM_WriterDurabilityTier/flush_only    journal on, fflush per operation
//                                         (process-crash safe)
//   BM_WriterDurabilityTier/fsync         journal on, fsync per operation -- the
//                                         DEFAULT (power-loss safe)
//
// Quantifies what the crash-recovery defaults cost on the writer hot path: with
// the journal enabled every recorded frame is serialized twice (once into its F
// record, once into its chunk) and, in fsync mode, hits physical media per frame.
// items_per_second is frames/sec end-to-end (writer create + record + Stop).

#include "vtx_schema_generated.h" // complete fbsvtx types before the policy header
#include "vtx_schema.pb.h"

#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_replay_recovery.h"
#include "vtx/writer/core/vtx_writer_facade.h" // brings SchemaRegistry/SchemaSanitizer for writer.h
#include "vtx/writer/core/writer.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/sinks/file_sink.h"

#include <benchmark/benchmark.h>

#include <filesystem>
#include <string>

namespace {

    constexpr int kFramesPerIteration = 200;

    std::string TempOutputPath() {
        return (std::filesystem::temp_directory_path() / "vtx_bench_recovery_out.vtx").string();
    }

    std::string ArenaSchemaPath() {
        return (std::filesystem::path(VTX_BENCH_FIXTURES_DIR).parent_path().parent_path() / "samples" / "content" /
                "writer" / "arena" / "arena_schema.json")
            .string();
    }

    struct SilenceDebugLogsOnce {
        SilenceDebugLogsOnce() { VTX::Logger::Instance().SetDebugEnabled(false); }
    };
    const SilenceDebugLogsOnce silence_recovery_bench_logs_once {};

} // namespace

// arg0: enable_recovery_journal, arg1: durable_writes
static void BM_WriterDurabilityTier(benchmark::State& state) {
    using RawWriter = VTX::ReplayWriter<VTX::ChunkedFileSink<VTX::FlatBuffersVtxPolicy>>;
    const std::string schema_path = ArenaSchemaPath();
    const std::string out_path = TempOutputPath();
    const bool journal = state.range(0) != 0;
    const bool durable = state.range(1) != 0;

    for (auto _ : state) {
        RawWriter::Config config;
        config.sink_config.filename = out_path;
        config.sink_config.header_config.replay_name = "BenchRecovery";
        config.sink_config.enable_recovery_journal = journal;
        config.sink_config.durable_writes = durable;
        config.schema_json_path = schema_path;
        config.default_fps = 60.0f;
        config.chunker_config.max_frames = 100;

        RawWriter writer(config);
        for (int i = 0; i < kFramesPerIteration; ++i) {
            VTX::Frame frame;
            auto& bucket = frame.CreateBucket("entity");
            VTX::PropertyContainer entity;
            entity.entity_type_id = 0;
            entity.float_properties.push_back(static_cast<float>(i) * 1.5f);
            bucket.unique_ids.push_back("player_" + std::to_string(i % 10));
            bucket.entities.push_back(std::move(entity));

            VTX::GameTime::GameTimeRegister game_time;
            game_time.game_time = static_cast<float>(i) / 60.0f;
            writer.RecordFrame(frame, game_time);
        }
        writer.Stop();
    }

    state.SetItemsProcessed(state.iterations() * kFramesPerIteration);
    std::filesystem::remove(out_path);
    std::filesystem::remove(out_path + ".recovery");
}
BENCHMARK(BM_WriterDurabilityTier)
    ->Unit(benchmark::kMillisecond)
    ->Args({0, 1})
    ->ArgNames({"journal", "fsync"})
    ->Args({1, 0})
    ->Args({1, 1});

// ---------------------------------------------------------------------------
// Read cost of a recovered file's tail: pending frames are re-appended by
// RepairReplayFile as ONE-FRAME chunks, so a recovery with a large in-flight
// batch yields many tiny chunks. This pair quantifies the sequential-read
// penalty versus an identically-sized cleanly-written file (100-frame chunks).
// For hot-path use, transcode a salvaged file by re-recording it (see docs).
// ---------------------------------------------------------------------------

namespace {

    constexpr int kReadFrames = 300;

    void RecordFramesInto(VTX::ReplayWriter<VTX::ChunkedFileSink<VTX::FlatBuffersVtxPolicy>>& writer, int frames) {
        for (int i = 0; i < frames; ++i) {
            VTX::Frame frame;
            auto& bucket = frame.CreateBucket("entity");
            VTX::PropertyContainer entity;
            entity.entity_type_id = 0;
            entity.float_properties.push_back(static_cast<float>(i) * 1.5f);
            bucket.unique_ids.push_back("player_" + std::to_string(i % 10));
            bucket.entities.push_back(std::move(entity));
            VTX::GameTime::GameTimeRegister game_time;
            game_time.game_time = static_cast<float>(i) / 60.0f;
            writer.RecordFrame(frame, game_time);
        }
    }

    // arg: pending frames at crash time (0 = clean Stop, all 100-frame chunks).
    std::string MakeReadSubject(int pending) {
        using RawWriter = VTX::ReplayWriter<VTX::ChunkedFileSink<VTX::FlatBuffersVtxPolicy>>;
        const std::string path =
            (std::filesystem::temp_directory_path() / ("vtx_bench_recovery_read_" + std::to_string(pending) + ".vtx"))
                .string();
        RawWriter::Config config;
        config.sink_config.filename = path;
        config.sink_config.header_config.replay_name = "BenchRecoveryRead";
        config.schema_json_path = ArenaSchemaPath();
        config.default_fps = 60.0f;
        config.chunker_config.max_frames = 100;
        {
            RawWriter writer(config);
            RecordFramesInto(writer, kReadFrames - pending);
            writer.Flush(); // committed portion lands in 100-frame chunks
            RecordFramesInto(writer, pending);
            if (pending == 0)
                writer.Stop();
            // else: dropped without Stop -> `pending` in-flight frames
        }
        if (pending > 0) {
            const auto rr = VTX::RepairReplayFile(path);
            if (!rr.ok() || rr.recovered_frames != kReadFrames)
                return {};
        }
        return path;
    }

} // namespace

static void BM_ReaderRecoveredTail(benchmark::State& state) {
    const int pending = static_cast<int>(state.range(0));
    const std::string path = MakeReadSubject(pending);
    if (path.empty()) {
        state.SkipWithError("failed to prepare read subject");
        return;
    }

    for (auto _ : state) {
        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            state.SkipWithError("open failed");
            break;
        }
        ctx->WaitUntilReady();
        for (int i = 0; i < kReadFrames; ++i)
            benchmark::DoNotOptimize(ctx->GetFrameSync(i));
    }

    state.SetItemsProcessed(state.iterations() * kReadFrames);
    std::filesystem::remove(path);
}
BENCHMARK(BM_ReaderRecoveredTail)
    ->Unit(benchmark::kMillisecond)
    ->Arg(0)   // clean file: 3 chunks of 100
    ->Arg(200) // recovered: 1 chunk of 100 + 200 one-frame chunks
    ->ArgName("pending");
