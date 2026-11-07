// VTX SDK -- record-path benchmark over a real replay.
//
//   vtx_record_bench <input.vtx> [--frames N] [--record M] [--repeat R] [--out <path>] [--verify]
//
// Loads the first N frames of <input.vtx> (default 600) into memory, then records M frames (default N),
// cycling through the loaded ones, through
// ReplayWriter<ChunkedFileSink<ProtobufVtxPolicy>> configured the way a production
// capture writer configures it (zstd level 10, durable writes, recovery journal
// with chunk-only journaling, 1000-frame chunks). Only TryRecordFrame and the
// final Stop are timed; each frame is copied out of the loaded set before its
// timer starts, because a live writer is handed a freshly built frame every tick.
//
// Reports per-frame record time (mean, p50, p95, p99, max) and frames/second.
// --verify reopens the written file and checks every frame against the input.

#include "vtx_schema.pb.h"

#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h" // SchemaRegistry and SchemaSanitizer for writer.h
#include "vtx/writer/core/writer.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"
#include "vtx/writer/policies/sinks/file_sink.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

    using Clock = std::chrono::steady_clock;
    using Writer = VTX::ReplayWriter<VTX::ChunkedFileSink<VTX::ProtobufVtxPolicy>>;

    double Ms(Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    }

    double Percentile(std::vector<double> v, double p) {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
        return v[i];
    }

    size_t EntityCount(const VTX::Frame& f) {
        size_t n = 0;
        for (const auto& b : f.GetBuckets())
            n += b.entities.size();
        return n;
    }

    bool SameFrame(const VTX::Frame& a, const VTX::Frame& b, std::string& why) {
        const auto& ab = a.GetBuckets();
        const auto& bb = b.GetBuckets();
        if (ab.size() != bb.size()) {
            why = "bucket count";
            return false;
        }
        for (size_t i = 0; i < ab.size(); ++i) {
            if (ab[i].unique_ids != bb[i].unique_ids) {
                why = "unique ids of bucket " + std::to_string(i);
                return false;
            }
            if (ab[i].entities.size() != bb[i].entities.size()) {
                why = "entity count of bucket " + std::to_string(i);
                return false;
            }
            for (size_t e = 0; e < ab[i].entities.size(); ++e) {
                if (ab[i].entities[e].content_hash != bb[i].entities[e].content_hash) {
                    why = "content hash of bucket " + std::to_string(i) + " entity " + std::to_string(e);
                    return false;
                }
            }
        }
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <input.vtx> [--frames N] [--record M] [--repeat R] [--out <path>] [--verify]\n",
                     argv[0]);
        return 2;
    }
    VTX::Logger::Instance().SetDebugEnabled(false);

    const std::string input = argv[1];
    // Every loaded frame stays in memory for the run: a few hundred production frames is
    // enough to measure the record path; --frames raises it deliberately.
    int max_frames = 600;
    int repeat = 1;
    int record = -1;
    bool verify = false;
    std::string out = (std::filesystem::temp_directory_path() / "vtx_record_bench_out.vtx").string();
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
            max_frames = std::atoi(argv[++i]);
        else if (a == "--record" && i + 1 < argc)
            record = std::atoi(argv[++i]);
        else if (a == "--repeat" && i + 1 < argc)
            repeat = std::max(1, std::atoi(argv[++i]));
        else if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--verify")
            verify = true;
    }

    // --- Load the input -------------------------------------------------------------------------
    std::vector<VTX::Frame> frames;
    std::string schema;
    std::vector<int64_t> game_times;
    {
        VTX::ReaderContext ctx = VTX::OpenReplayFile(input);
        if (!ctx || !ctx.WaitUntilReady()) {
            std::fprintf(stderr, "cannot open %s: %s\n", input.c_str(), ctx.GetError().message.c_str());
            return 1;
        }
        const int32_t total = ctx->GetTotalFrames();
        const int32_t n = max_frames > 0 ? std::min(total, max_frames) : total;
        schema = ctx->GetContextualSchema().property_mapping;
        frames.reserve(static_cast<size_t>(n));
        for (int32_t f = 0; f < n; ++f) {
            const VTX::Frame* frame = ctx->GetFrameSync(f);
            if (!frame) {
                std::fprintf(stderr, "cannot read frame %d\n", f);
                return 1;
            }
            frames.push_back(*frame);
        }
    }
    if (frames.empty() || schema.empty()) {
        std::fprintf(stderr, "no frames or no schema in %s\n", input.c_str());
        return 1;
    }
    size_t entities = 0;
    for (const auto& f : frames)
        entities += EntityCount(f);
    std::printf("input: %zu frames, %.1f entities/frame\n", frames.size(),
                static_cast<double>(entities) / static_cast<double>(frames.size()));
    const size_t to_record = record > 0 ? static_cast<size_t>(record) : frames.size();

    // --- Record ---------------------------------------------------------------------------------
    std::vector<double> per_frame;
    per_frame.reserve(to_record * static_cast<size_t>(repeat));
    double stop_ms = 0.0;
    Clock::duration total {};
    VTX::FileSinkAtomicPerfObserver sink_perf; // the sink's own split of chunk, header and footer work
    for (int r = 0; r < repeat; ++r) {
        std::filesystem::remove(out);
        Writer::Config config;
        config.sink_config.filename = out;
        config.sink_config.header_config.replay_name = "record-bench";
        config.sink_config.header_config.replay_uuid = "record-bench";
        config.sink_config.header_config.recorded_utc_timestamp = 1; // byte-comparable outputs across builds
        config.sink_config.b_use_compression = true;
        config.sink_config.compression_level = 10;
        config.sink_config.durable_writes = true;
        config.sink_config.enable_recovery_journal = true;
        config.sink_config.journal_pending_frames = false;
        config.sink_config.perf_observer = &sink_perf;
        config.default_fps = 30.0f;
        config.is_increasing = true;
        config.chunker_config.max_frames = 1000;
        config.chunker_config.max_bytes = 100u * 1024u * 1024u;
        config.schema_json_content = schema;

        Writer writer(config);
        for (size_t i = 0; i < to_record; ++i) {
            VTX::Frame frame = frames[i % frames.size()]; // the tick's fresh frame, outside the timer
            VTX::GameTime::GameTimeRegister time;
            time.game_time = static_cast<float>(i) / 30.0f;
            // Timed with the frame's destruction: a writer that consumes the frame destroys it inside the
            // call, one that reads it in place leaves that to its caller -- the cost is the record's either way.
            const auto t0 = Clock::now();
            const VTX::RecordResult result = writer.TryRecordFrame(frame, time);
            frame = VTX::Frame {};
            const auto dt = Clock::now() - t0;
            if (!result.IsWritten()) {
                std::fprintf(stderr, "frame %zu refused: %s\n", i, result.error.message.c_str());
                return 1;
            }
            per_frame.push_back(Ms(dt));
            total += dt;
        }
        const auto s0 = Clock::now();
        writer.Stop();
        const auto sd = Clock::now() - s0;
        stop_ms += Ms(sd);
        total += sd;
    }

    double sum = 0.0;
    for (double v : per_frame)
        sum += v;
    const double mean = sum / static_cast<double>(per_frame.size());
    std::printf("record: mean %.3f ms  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f  (per frame, %zu frames)\n", mean,
                Percentile(per_frame, 0.50), Percentile(per_frame, 0.95), Percentile(per_frame, 0.99),
                Percentile(per_frame, 1.0), per_frame.size());
    std::printf("stop:   %.1f ms per run\n", stop_ms / repeat);
    const VTX::FileSinkPerformanceStats sink = sink_perf.Snapshot();
    std::printf("sink:   serialize %.1f ms  compress %.1f ms  write+sync %.1f ms  (all runs)\n",
                static_cast<double>(sink.serialization_us) / 1000.0, static_cast<double>(sink.compression_us) / 1000.0,
                static_cast<double>(sink.disk_write_us) / 1000.0);
    std::printf("total:  %.1f ms  ->  %.0f frames/s\n", Ms(total),
                static_cast<double>(per_frame.size()) / (Ms(total) / 1000.0));
    std::printf("output: %s (%.2f MB)\n", out.c_str(),
                static_cast<double>(std::filesystem::file_size(out)) / (1024.0 * 1024.0));

    // --- Verify ---------------------------------------------------------------------------------
    if (verify) {
        VTX::ReaderContext ctx = VTX::OpenReplayFile(out);
        if (!ctx || !ctx.WaitUntilReady()) {
            std::fprintf(stderr, "verify: cannot open the output\n");
            return 1;
        }
        if (ctx->GetTotalFrames() != static_cast<int32_t>(to_record)) {
            std::fprintf(stderr, "verify: %d frames written, %zu expected\n", ctx->GetTotalFrames(), to_record);
            return 1;
        }
        for (int32_t f = 0; f < ctx->GetTotalFrames(); ++f) {
            const VTX::Frame* back = ctx->GetFrameSync(f);
            std::string why = "unreadable";
            if (!back || !SameFrame(frames[static_cast<size_t>(f) % frames.size()], *back, why)) {
                std::fprintf(stderr, "verify: frame %d differs (%s)\n", f, why.c_str());
                return 1;
            }
        }
        std::printf("verify: all %zu frames match (ids, entity counts, content hashes)\n", to_record);
    }
    return 0;
}
