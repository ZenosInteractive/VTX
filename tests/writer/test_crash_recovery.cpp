// Crash-recovery tests: a writer that dies before Stop() leaves a footerless .vtx
// plus a ".recovery" journal; RepairReplayFile() must reconstruct a valid, readable
// file from it, recovering every durable chunk AND every in-flight (un-flushed)
// frame, while dropping any torn/corrupt tail.
//
// Crash states are fabricated byte-faithfully rather than by racing a real process
// death: a valid recording is written and captured (header bytes, real per-chunk
// payloads/offsets/checksums, per-frame times), then a footerless .vtx + a
// ".recovery" journal are rebuilt through the *same* RecoveryJournal API the sink
// uses. That is byte-for-byte what the sink leaves on disk after a mid-write crash,
// and it lets each crash window (between chunks, between frames, torn chunk, torn
// frame record, corrupt chunk, mid-footer, leftover footer) be reproduced exactly.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // CreateProcessA / TerminateProcess for the real-kill tests
#endif

#include "vtx_schema_generated.h" // complete fbsvtx/cppvtx types before the policy headers
#include "vtx_schema.pb.h"

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/reader/core/vtx_replay_validation.h"
#include "vtx/writer/core/vtx_frame_post_processor.h"
#include "vtx/writer/core/vtx_replay_recovery.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/core/writer.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"
#include "vtx/writer/policies/sinks/async_sink_adapter.h"
#include "vtx/writer/policies/sinks/file_sink.h"
#include "vtx/writer/policies/sinks/recovery_journal.h"

#include "util/test_fixtures.h"

namespace {

    enum class Fmt { Flat, Proto };

    VTX::Frame BuildFrame(int i) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {"player_0", "Alpha"};
        pc.int32_properties = {1, i, 0}; // Team, Score(=i), Deaths
        pc.float_properties = {100.0f, 50.0f};
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

    // A captured valid recording: everything needed to rebuild an equivalent
    // footerless .vtx and its recovery journal, byte-for-byte.
    struct Recording {
        std::string format_magic;                // "VTXF" / "VTXP"
        std::string header;                      // file bytes [0 .. first_chunk_offset)
        std::vector<VTX::ChunkIndexData> chunks; // real seek table (offsets/sizes/checksums)
        std::vector<std::string> payloads;       // per-chunk on-disk payload (len == chunk_size_bytes - 4)
        std::vector<int64_t> game_times;         // per-frame, from the footer
        std::vector<int64_t> created_utc;        // per-frame, from the footer
        int total_frames = 0;
    };

    // Write a valid recording (footer + no journal) and capture it. `per_chunk`
    // frames are flushed per chunk; use per_chunk==1 when the payloads must double as
    // single-frame (F-record) payloads for pending-frame fabrication.
    Recording CaptureRecording(const std::string& suffix, int frames, int per_chunk, Fmt fmt) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("cap_" + suffix + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "CrashRecoveryTest";
        cfg.default_fps = 60.0f;
        cfg.use_compression = true;

        auto writer =
            (fmt == Fmt::Flat) ? VTX::CreateFlatBuffersWriterFacade(cfg) : VTX::CreateProtobufWriterFacade(cfg);
        EXPECT_NE(writer, nullptr);
        int in_batch = 0;
        for (int i = 0; i < frames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
            if (++in_batch >= per_chunk) {
                writer->Flush();
                in_batch = 0;
            }
        }
        if (in_batch > 0)
            writer->Flush();
        writer->Stop();
        writer.reset(); // release the file handle before reading it back

        Recording rec;
        rec.format_magic = (fmt == Fmt::Flat) ? "VTXF" : "VTXP";
        {
            auto ctx = VTX::OpenReplayFile(cfg.output_filepath);
            EXPECT_TRUE(ctx) << ctx.error;
            ctx->WaitUntilReady();
            rec.total_frames = ctx->GetTotalFrames();
            for (const auto& e : ctx->GetSeekTable()) {
                VTX::ChunkIndexData d;
                d.chunk_index = e.chunk_index;
                d.start_frame = e.start_frame;
                d.end_frame = e.end_frame;
                d.file_offset = static_cast<int64_t>(e.file_offset);
                d.chunk_size_bytes = e.chunk_size_bytes;
                d.checksum = e.checksum;
                rec.chunks.push_back(d);
            }
            const VTX::FileFooter footer = ctx->GetFooter();
            rec.game_times.assign(frames, 0);
            rec.created_utc.assign(frames, 0);
            for (int i = 0; i < frames; ++i) {
                if (static_cast<size_t>(i) < footer.times.game_time.size())
                    rec.game_times[i] = static_cast<int64_t>(footer.times.game_time[i]);
                if (static_cast<size_t>(i) < footer.times.created_utc.size())
                    rec.created_utc[i] = static_cast<int64_t>(footer.times.created_utc[i]);
            }
        }

        std::ifstream in(cfg.output_filepath, std::ios::binary);
        const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_FALSE(rec.chunks.empty());
        const size_t first_off = static_cast<size_t>(rec.chunks.front().file_offset);
        rec.header = all.substr(0, first_off);
        for (const auto& c : rec.chunks) {
            const size_t payload_off = static_cast<size_t>(c.file_offset) + sizeof(uint32_t);
            const size_t payload_len = c.chunk_size_bytes - sizeof(uint32_t);
            EXPECT_LE(payload_off + payload_len, all.size());
            rec.payloads.push_back(all.substr(payload_off, payload_len));
        }
        return rec;
    }

    // Rebuild a footerless .vtx (header + committed chunks) plus the ".recovery"
    // journal the sink would have left: C/T records for the committed chunks and F
    // records for `pending_frames` un-flushed frames (drawn from the recording, whose
    // payloads must be single-frame -> capture with per_chunk==1). `journal_magic`
    // overrides the journal's recorded format for the mismatch test.
    std::string FabricateCrash(const std::string& out_suffix, const Recording& rec, int committed_chunks,
                               int pending_frames, const std::string& journal_magic = "") {
        const std::string magic = journal_magic.empty() ? rec.format_magic : journal_magic;
        const std::string path = VtxTest::OutputPath("fab_" + out_suffix + ".vtx");

        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(rec.header.data(), static_cast<std::streamsize>(rec.header.size()));
            for (int i = 0; i < committed_chunks; ++i) {
                const uint32_t payload_size = rec.chunks[i].chunk_size_bytes - static_cast<uint32_t>(sizeof(uint32_t));
                out.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
                out.write(rec.payloads[i].data(), static_cast<std::streamsize>(rec.payloads[i].size()));
            }
        }

        const int last_committed_frame = committed_chunks > 0 ? rec.chunks[committed_chunks - 1].end_frame : -1;

        VTX::RecoveryJournal journal;
        EXPECT_TRUE(journal.Open(VTX::RecoveryJournal::PathFor(path), magic, /*durable=*/true));
        int64_t offset = static_cast<int64_t>(rec.header.size());
        for (int i = 0; i < committed_chunks; ++i) {
            VTX::ChunkIndexData cd;
            cd.chunk_index = i;
            cd.file_offset = offset;
            cd.start_frame = rec.chunks[i].start_frame;
            cd.end_frame = rec.chunks[i].end_frame;
            cd.chunk_size_bytes = rec.chunks[i].chunk_size_bytes;
            cd.checksum = rec.chunks[i].checksum;

            std::vector<VTX::RecoveryJournal::FrameTime> times;
            for (int f = cd.start_frame; f <= cd.end_frame; ++f)
                times.push_back({f, rec.game_times[f], rec.created_utc[f]});
            journal.CommitChunk(cd, times);
            offset += rec.chunks[i].chunk_size_bytes;
        }
        for (int p = 0; p < pending_frames; ++p) {
            const int idx = last_committed_frame + 1 + p;
            journal.AppendFrame(idx, rec.game_times[idx], rec.created_utc[idx], rec.payloads[idx]);
        }
        journal.Close();
        return path;
    }

    // Byte offset just past the last committed chunk (== end of the footerless body).
    int64_t BodyEnd(const Recording& rec, int committed_chunks) {
        int64_t end = static_cast<int64_t>(rec.header.size());
        for (int i = 0; i < committed_chunks; ++i)
            end += rec.chunks[i].chunk_size_bytes;
        return end;
    }

    // Build a footerless .vtx + journal the way the sink actually does: for each
    // single-frame chunk, AppendFrame THEN CommitChunk (so each committed frame's F
    // record is superseded by its C record), then leave `pending` trailing frames
    // un-committed. `compact_threshold` drives when the append-only journal reclaims
    // superseded F records. Requires a per_chunk==1 recording.
    std::string BuildInterleaved(const std::string& suffix, const Recording& rec, int committed, int pending,
                                 uint64_t compact_threshold) {
        const std::string path = VtxTest::OutputPath("fab_" + suffix + ".vtx");
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(rec.header.data(), static_cast<std::streamsize>(rec.header.size()));
            for (int i = 0; i < committed; ++i) {
                const uint32_t payload_size = rec.chunks[i].chunk_size_bytes - static_cast<uint32_t>(sizeof(uint32_t));
                out.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
                out.write(rec.payloads[i].data(), static_cast<std::streamsize>(rec.payloads[i].size()));
            }
        }

        VTX::RecoveryJournal j;
        EXPECT_TRUE(j.Open(VTX::RecoveryJournal::PathFor(path), rec.format_magic, /*durable=*/true));
        j.SetCompactThresholdBytes(compact_threshold);
        int64_t offset = static_cast<int64_t>(rec.header.size());
        for (int i = 0; i < committed; ++i) {
            j.AppendFrame(i, rec.game_times[i], rec.created_utc[i], rec.payloads[i]);
            VTX::ChunkIndexData cd;
            cd.chunk_index = i;
            cd.file_offset = offset;
            cd.start_frame = i;
            cd.end_frame = i;
            cd.chunk_size_bytes = rec.chunks[i].chunk_size_bytes;
            cd.checksum = rec.chunks[i].checksum;
            j.CommitChunk(cd, {{i, rec.game_times[i], rec.created_utc[i]}});
            offset += rec.chunks[i].chunk_size_bytes;
        }
        for (int p = 0; p < pending; ++p) {
            const int idx = committed + p;
            j.AppendFrame(idx, rec.game_times[idx], rec.created_utc[idx], rec.payloads[idx]);
        }
        j.Close();
        return path;
    }

} // namespace

// --- Between chunks: every committed chunk is recovered -----------------------

TEST(CrashRecovery, RecoversAllCommittedChunks) {
    const Recording rec = CaptureRecording("all", 5, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("all", rec, /*committed=*/5, /*pending=*/0);

    ASSERT_TRUE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));
    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.repaired);
    EXPECT_EQ(rr.recovered_chunks, 5);
    EXPECT_EQ(rr.recovered_frames, 5);
    EXPECT_FALSE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 5);
    const VTX::Frame* f = ctx->GetFrameSync(4);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->GetBuckets().size(), 1u);
    ASSERT_EQ(f->GetBuckets()[0].entities.size(), 1u);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 4); // Score of frame 4
}

// Multi-frame committed chunks (3 frames each) recover all their frames.
TEST(CrashRecovery, RecoversMultiFrameChunks) {
    const Recording rec = CaptureRecording("multi", 6, /*per_chunk=*/3, Fmt::Flat);
    ASSERT_EQ(rec.chunks.size(), 2u);
    const std::string path = FabricateCrash("multi", rec, /*committed=*/2, /*pending=*/0);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_chunks, 2);
    EXPECT_EQ(rr.recovered_frames, 6);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 6);
    const VTX::Frame* f = ctx->GetFrameSync(5);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 5);
}

// --- Between frames: in-flight (un-flushed) frames are recovered from F records --

TEST(CrashRecovery, RecoversPendingFramesAfterLastChunk) {
    const Recording rec = CaptureRecording("pending", 6, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("pending", rec, /*committed=*/3, /*pending=*/3);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.repaired);
    EXPECT_EQ(rr.recovered_frames, 6); // 3 committed + 3 pending

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 6);
    const VTX::Frame* f = ctx->GetFrameSync(5); // a recovered pending frame
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->GetBuckets()[0].entities.size(), 1u);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 5);
}

// A crash before ANY chunk was flushed: only F records exist -> recover them all.
TEST(CrashRecovery, RecoversOnlyPendingWhenNothingFlushed) {
    const Recording rec = CaptureRecording("onlypending", 4, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("onlypending", rec, /*committed=*/0, /*pending=*/4);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 4);
    const VTX::Frame* f = ctx->GetFrameSync(0);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 0);
}

// --- Torn / corrupt tails: dropped, everything before is kept ------------------

// A crash mid-write of the last chunk (its bytes run past EOF): drop it, keep the rest.
TEST(CrashRecovery, DropsTornTailChunk) {
    const Recording rec = CaptureRecording("torn", 4, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("torn", rec, /*committed=*/4, /*pending=*/0);

    // Truncate into the middle of the last chunk so its recorded extent exceeds EOF.
    const int64_t mid_last = rec.chunks[3].file_offset + 6;
    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(mid_last));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_chunks, 3); // torn last chunk dropped
    EXPECT_EQ(rr.recovered_frames, 3);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
}

// A partial chunk written to the body before its journal (C) record existed: the
// journal lists fewer chunks than the body holds -> the un-journaled tail is dropped.
TEST(CrashRecovery, DropsPartialChunkWrittenBeforeJournalRecord) {
    const Recording rec = CaptureRecording("partial", 4, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("partial", rec, /*committed=*/2, /*pending=*/0);

    // Append a half-written chunk framing (size prefix + a few payload bytes) that no
    // C record covers -- exactly what a crash mid-chunk-write leaves after the body.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const uint32_t bogus_size = 4096;
        out.write(reinterpret_cast<const char*>(&bogus_size), sizeof(bogus_size));
        const char junk[7] = {1, 2, 3, 4, 5, 6, 7};
        out.write(junk, sizeof(junk));
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_chunks, 2); // partial tail dropped
    EXPECT_EQ(rr.recovered_frames, 2);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 2);
}

// A chunk whose on-disk bytes were corrupted (checksum mismatch) stops recovery at it.
TEST(CrashRecovery, ChecksumDetectsCorruptChunk) {
    const Recording rec = CaptureRecording("corrupt", 4, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("corrupt", rec, /*committed=*/4, /*pending=*/0);

    // Flip a byte inside chunk 1's payload (past its 4-byte size prefix).
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        const std::streamoff pos = static_cast<std::streamoff>(rec.chunks[1].file_offset + 5);
        char c = 0;
        f.seekg(pos);
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0xFF);
        f.seekp(pos);
        f.write(&c, 1);
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_chunks, 1); // chunk 0 only; chunk 1 fails checksum -> stop
    EXPECT_EQ(rr.recovered_frames, 1);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 1);
}

// A crash mid-write of a frame record: the torn last F record is detected (per-record
// checksum) and dropped; the committed chunks and the earlier pending frames survive.
TEST(CrashRecovery, DropsTornPendingFrameRecord) {
    const Recording rec = CaptureRecording("tornframe", 6, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("tornframe", rec, /*committed=*/2, /*pending=*/3);

    // Shave a few bytes off the journal tail: the last F record loses its checksum and
    // is rejected as torn, but the record before it remains intact.
    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    const auto jsz = std::filesystem::file_size(journal_path);
    std::filesystem::resize_file(journal_path, static_cast<std::uintmax_t>(jsz - 4));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4); // 2 committed + 2 intact pending (last F torn)

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 4);
}

// --- Footer windows -----------------------------------------------------------

// A crash mid-footer-write leaves garbage after the last chunk and no valid trailer:
// repair truncates the garbage and rebuilds a valid footer from the journal.
TEST(CrashRecovery, RepairsCrashDuringFooterWrite) {
    const Recording rec = CaptureRecording("footer", 3, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("footer", rec, /*committed=*/3, /*pending=*/0);

    // Simulate a footer write interrupted before the [u32 size][magic] trailer landed.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const std::string partial_footer(64, '\xAB');
        out.write(partial_footer.data(), static_cast<std::streamsize>(partial_footer.size()));
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.repaired);
    EXPECT_EQ(rr.recovered_chunks, 3);
    EXPECT_EQ(rr.recovered_frames, 3);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
}

// A crash between the footer fsync and the journal delete leaves a COMPLETE file plus
// a leftover .recovery. Repair must detect the valid footer and preserve it (incl.
// timing) rather than truncating and rewriting a timing-less one.
TEST(CrashRecovery, PreservesValidFooterWhenJournalLeftover) {
    const Recording rec = CaptureRecording("leftover", 3, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = VtxTest::OutputPath("cap_leftover.vtx"); // the valid file capture wrote
    ASSERT_TRUE(std::filesystem::exists(path));
    ASSERT_FALSE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));

    // Recreate a leftover journal (header only is enough; the valid-footer detection
    // fires before the journal contents are ever consulted).
    {
        VTX::RecoveryJournal j;
        ASSERT_TRUE(j.Open(VTX::RecoveryJournal::PathFor(path), rec.format_magic, /*durable=*/true));
        j.Close();
    }

    const auto size_before = std::filesystem::file_size(path);
    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.was_clean);                                // detected an already-complete file
    EXPECT_FALSE(rr.repaired);                                // did NOT rewrite the footer
    EXPECT_EQ(std::filesystem::file_size(path), size_before); // footer left untouched
    EXPECT_FALSE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
}

// A journal whose recorded format disagrees with the main file (a stale sidecar left
// over a file replaced with the other format) must be refused, not applied.
TEST(CrashRecovery, RefusesMismatchedJournalFormat) {
    const Recording rec = CaptureRecording("mismatch", 2, /*per_chunk=*/1, Fmt::Flat);
    // File is VTXF; journal claims VTXP.
    const std::string path = FabricateCrash("mismatch", rec, /*committed=*/2, /*pending=*/0, "VTXP");

    const auto rr = VTX::RepairReplayFile(path);
    EXPECT_FALSE(rr.ok()); // refused: journal format does not match the file
    EXPECT_FALSE(rr.repaired);
}

// A cleanly-closed file has no journal, so repair is a no-op and the file is intact.
TEST(CrashRecovery, CleanFileNeedsNoRepair) {
    const Recording rec = CaptureRecording("clean", 3, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = VtxTest::OutputPath("cap_clean.vtx");
    ASSERT_TRUE(std::filesystem::exists(path));
    ASSERT_FALSE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.was_clean);
    EXPECT_FALSE(rr.repaired);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
    (void)rec;
}

// A crash right after the header (before any chunk or frame is durable) leaves a
// header-only journal: repair produces a valid, openable 0-frame file.
TEST(CrashRecovery, RecoversHeaderOnlyCrashAsEmptyFile) {
    const Recording rec = CaptureRecording("headeronly", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("headeronly", rec, /*committed=*/0, /*pending=*/0);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 0);
    EXPECT_FALSE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 0);
}

// Defensive: if F records for already-committed frames linger in the journal (a crash
// window CommitChunk normally forecloses by truncating first), they are deduped by
// index and NOT re-appended -- the frame count stays correct.
TEST(CrashRecovery, IgnoresLingeringFRecordsForCommittedFrames) {
    const Recording rec = CaptureRecording("dedup", 4, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = VtxTest::OutputPath("fab_dedup.vtx");

    // Footerless body: 2 committed chunks (frames 0 and 1).
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(rec.header.data(), static_cast<std::streamsize>(rec.header.size()));
        for (int i = 0; i < 2; ++i) {
            const uint32_t payload_size = rec.chunks[i].chunk_size_bytes - static_cast<uint32_t>(sizeof(uint32_t));
            out.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
            out.write(rec.payloads[i].data(), static_cast<std::streamsize>(rec.payloads[i].size()));
        }
    }

    VTX::RecoveryJournal j;
    ASSERT_TRUE(j.Open(VTX::RecoveryJournal::PathFor(path), rec.format_magic, /*durable=*/true));
    int64_t offset = static_cast<int64_t>(rec.header.size());
    for (int i = 0; i < 2; ++i) {
        VTX::ChunkIndexData cd;
        cd.chunk_index = i;
        cd.file_offset = offset;
        cd.start_frame = rec.chunks[i].start_frame;
        cd.end_frame = rec.chunks[i].end_frame;
        cd.chunk_size_bytes = rec.chunks[i].chunk_size_bytes;
        cd.checksum = rec.chunks[i].checksum;
        j.CommitChunk(cd, {{i, rec.game_times[i], rec.created_utc[i]}});
        offset += rec.chunks[i].chunk_size_bytes;
    }
    // Lingering F records for the already-committed frames 0 and 1 (must be ignored)...
    j.AppendFrame(0, rec.game_times[0], rec.created_utc[0], rec.payloads[0]);
    j.AppendFrame(1, rec.game_times[1], rec.created_utc[1], rec.payloads[1]);
    // ...plus genuine pending frames 2 and 3.
    j.AppendFrame(2, rec.game_times[2], rec.created_utc[2], rec.payloads[2]);
    j.AppendFrame(3, rec.game_times[3], rec.created_utc[3], rec.payloads[3]);
    j.Close();

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4); // 0,1 committed + 2,3 pending; lingering F for 0,1 ignored

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 4);
    const VTX::Frame* f = ctx->GetFrameSync(3);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 3);
}

// Append-only journal + compaction: superseded F records are reclaimed (the journal
// stays smaller than an un-compacted one) while recovery is byte-identical. Threshold
// 1 forces a compaction (close -> rewrite temp -> atomic rename -> reopen) on every
// commit, stressing that path; recovery must still yield every frame.
TEST(CrashRecovery, CompactionReclaimsSupersededFrames) {
    const Recording rec = CaptureRecording("compact", 8, /*per_chunk=*/1, Fmt::Flat);
    const std::string compacted = BuildInterleaved("compact_on", rec, /*committed=*/6, /*pending=*/2, /*threshold=*/1);
    const std::string uncompacted =
        BuildInterleaved("compact_off", rec, /*committed=*/6, /*pending=*/2, /*threshold=*/~uint64_t(0));

    const auto sz_on = std::filesystem::file_size(VTX::RecoveryJournal::PathFor(compacted));
    const auto sz_off = std::filesystem::file_size(VTX::RecoveryJournal::PathFor(uncompacted));
    EXPECT_LT(sz_on, sz_off); // compaction dropped the 6 superseded F payloads

    for (const std::string& path : {compacted, uncompacted}) {
        const auto rr = VTX::RepairReplayFile(path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_EQ(rr.recovered_frames, 8) << path;

        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error;
        ctx->WaitUntilReady();
        EXPECT_EQ(ctx->GetTotalFrames(), 8) << path;
        const VTX::Frame* f = ctx->GetFrameSync(7); // a pending frame that survived compaction
        ASSERT_NE(f, nullptr) << path;
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 7) << path;
    }
}

// The user-driven helpers locate the sidecar and detect an unclean shutdown; after a
// successful repair the sidecar is gone so a re-check reports clean.
TEST(CrashRecovery, HelpersDetectAndLocateSidecar) {
    const Recording rec = CaptureRecording("helpers", 3, /*per_chunk=*/1, Fmt::Flat);

    const std::string clean = VtxTest::OutputPath("cap_helpers.vtx"); // capture left a clean file
    EXPECT_EQ(VTX::RecoveryJournalPath(clean), clean + ".recovery");
    EXPECT_FALSE(VTX::ReplayNeedsRecovery(clean));

    const std::string crashed = FabricateCrash("helpers", rec, /*committed=*/3, /*pending=*/0);
    EXPECT_TRUE(VTX::ReplayNeedsRecovery(crashed));

    const auto rr = VTX::RepairReplayFile(crashed);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_FALSE(VTX::ReplayNeedsRecovery(crashed)); // sidecar removed after repair
}

// --- Exact-time recovery ------------------------------------------------------

// Per-frame times survive recovery exactly, across both the committed (T record) and
// the pending (F record) reconstruction paths.
TEST(CrashRecovery, RecoveredTimesMatchOriginal) {
    const Recording rec = CaptureRecording("times", 6, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("times", rec, /*committed=*/3, /*pending=*/3);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 6);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    const VTX::FileFooter footer = ctx->GetFooter();
    ASSERT_EQ(footer.times.game_time.size(), 6u);
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(static_cast<int64_t>(footer.times.game_time[i]), rec.game_times[i]) << "game_time[" << i << "]";
    if (!footer.times.created_utc.empty()) {
        ASSERT_EQ(footer.times.created_utc.size(), 6u);
        for (int i = 0; i < 6; ++i)
            EXPECT_EQ(static_cast<int64_t>(footer.times.created_utc[i]), rec.created_utc[i])
                << "created_utc[" << i << "]";
    }
}

// --- End-to-end through the REAL writer ----------------------------------------
//
// The facade auto-Stops in its destructor, but a raw ReplayWriter does not: dropping
// it without Stop() leaves exactly what a crash leaves (header + fsync'd chunks +
// journal, no footer). These tests drive the real writer/sink/journal path -- no
// fabrication -- and require the recovered file to match a cleanly-stopped control
// byte-for-byte in every footer time field.

namespace {

    template <typename Policy>
    using RawWriterFor = VTX::ReplayWriter<VTX::ChunkedFileSink<Policy>>;

    template <typename Policy>
    std::unique_ptr<RawWriterFor<Policy>> MakeRawWriter(const std::string& path, int32_t chunk_max_frames,
                                                        bool durable = true, bool compression = true,
                                                        bool journal = true, uint64_t compact_threshold = 0,
                                                        const std::string& schema_name = "test_schema.json") {
        typename RawWriterFor<Policy>::Config cfg;
        cfg.sink_config.filename = path;
        cfg.sink_config.header_config.replay_name = "CrashRecoveryE2E";
        cfg.sink_config.durable_writes = durable;
        cfg.sink_config.b_use_compression = compression;
        cfg.sink_config.enable_recovery_journal = journal;
        cfg.sink_config.journal_compact_threshold_bytes = compact_threshold;
        cfg.schema_json_path = VtxTest::FixturePath(schema_name);
        cfg.default_fps = 60.0f;
        cfg.chunker_config.max_frames = chunk_max_frames;
        return std::make_unique<RawWriterFor<Policy>>(cfg);
    }

    // Same raw writer, but with the async decorator in front of the file sink: chunk and
    // journal I/O run on a worker thread. Dropping it without Stop() still leaves exactly
    // what a crash leaves, so every crash-recovery expectation below applies unchanged --
    // that is the point of these variants.
    template <typename Policy>
    using RawAsyncWriterFor = VTX::ReplayWriter<VTX::AsyncSinkAdapter<VTX::ChunkedFileSink<Policy>>>;

    template <typename Policy>
    std::unique_ptr<RawAsyncWriterFor<Policy>>
    MakeRawAsyncWriter(const std::string& path, int32_t chunk_max_frames, bool durable = true, bool compression = true,
                       bool journal = true, uint64_t compact_threshold = 0, size_t queue_cap = 0,
                       const std::string& schema_name = "test_schema.json") {
        typename RawAsyncWriterFor<Policy>::Config cfg;
        cfg.sink_config.inner.filename = path;
        cfg.sink_config.inner.header_config.replay_name = "CrashRecoveryE2E";
        cfg.sink_config.inner.durable_writes = durable;
        cfg.sink_config.inner.b_use_compression = compression;
        cfg.sink_config.inner.enable_recovery_journal = journal;
        cfg.sink_config.inner.journal_compact_threshold_bytes = compact_threshold;
        cfg.sink_config.async_max_queue_frames = queue_cap;
        cfg.schema_json_path = VtxTest::FixturePath(schema_name);
        cfg.default_fps = 60.0f;
        cfg.chunker_config.max_frames = chunk_max_frames;
        return std::make_unique<RawAsyncWriterFor<Policy>>(cfg);
    }

    // Records 8 frames with explicit game_time + created_utc: a UTC jump at frame 3
    // (timeline gap) and a game-time reversal at frame 5 (game segment). Chunks of 3 ->
    // crash state = 2 committed chunks + 2 in-flight frames.
    template <typename Writer>
    void RecordEightFrames(Writer& writer) {
        constexpr int64_t kBaseUtc = 1'000'000'000'000;
        constexpr int64_t kStepUtc = 166'667; // ~1/60s in ticks
        const float game_times[8] = {0.f,      1.f / 60, 2.f / 60, 3.f / 60, 4.f / 60, 2.f / 60 /*reversal*/,
                                     5.f / 60, 6.f / 60};
        int64_t utc = kBaseUtc;
        for (int i = 0; i < 8; ++i) {
            utc += (i == 3) ? 10'000'000 : kStepUtc; // 1s jump at frame 3 -> gap
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = game_times[i];
            t.created_utc_time = utc;
            const auto res = writer.TryRecordFrame(frame, t);
            ASSERT_TRUE(res.written) << "frame " << i << ": " << res.error.message;
        }
    }

    // Full crash-vs-control comparison through the real writer for one policy.
    template <typename Policy>
    void RunRealWriterCrashVsControl(const std::string& prefix, bool async_crash = false) {
        // Control: identical inputs, clean Stop(). Always the SYNCHRONOUS writer, so an async
        // crash side is held to the exact output a synchronous clean run would have produced.
        const std::string control_path = VtxTest::OutputPath(prefix + "_e2e_control.vtx");
        {
            auto w = MakeRawWriter<Policy>(control_path, 3);
            RecordEightFrames(*w);
            w->Stop();
        }
        ASSERT_FALSE(VTX::ReplayNeedsRecovery(control_path));

        // Crash: same inputs, writer dropped without Stop().
        const std::string crash_path = VtxTest::OutputPath(prefix + "_e2e_crash.vtx");
        {
            if (async_crash) {
                // The adapter's destructor drains the queue and closes WITHOUT a footer, so an
                // in-process drop leaves the same footerless .vtx + journal a crash leaves --
                // and, having drained, must still account for every recorded frame.
                auto w = MakeRawAsyncWriter<Policy>(crash_path, 3);
                RecordEightFrames(*w);
            } else {
                auto w = MakeRawWriter<Policy>(crash_path, 3);
                RecordEightFrames(*w);
            }
            // dropped here -- no Stop(), no footer
        }
        ASSERT_TRUE(VTX::ReplayNeedsRecovery(crash_path));

        const auto rr = VTX::RepairReplayFile(crash_path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_TRUE(rr.repaired);
        EXPECT_EQ(rr.recovered_frames, 8); // 6 committed (2 chunks of 3) + 2 in-flight

        auto control = VTX::OpenReplayFile(control_path);
        ASSERT_TRUE(control) << control.error;
        control->WaitUntilReady();
        auto recovered = VTX::OpenReplayFile(crash_path);
        ASSERT_TRUE(recovered) << recovered.error;
        recovered->WaitUntilReady();

        EXPECT_EQ(recovered->GetTotalFrames(), control->GetTotalFrames());

        // Every frame's content survives -- compare the full per-entity content_hash
        // against the control, not just one property.
        for (int i = 0; i < 8; ++i) {
            const VTX::Frame* rfme = recovered->GetFrameSync(i);
            const VTX::Frame* cfme = control->GetFrameSync(i);
            ASSERT_NE(rfme, nullptr) << "frame " << i;
            ASSERT_NE(cfme, nullptr) << "frame " << i;
            EXPECT_EQ(rfme->GetBuckets()[0].entities[0].int32_properties[1], i) << "frame " << i;
            EXPECT_EQ(rfme->GetBuckets()[0].entities[0].content_hash, cfme->GetBuckets()[0].entities[0].content_hash)
                << "content_hash mismatch at frame " << i;
        }

        // Seeks across the committed-chunk / recovered-chunk boundary in cache-hostile
        // order (frames 0-5 live in 3-frame chunks, 6-7 in repair-appended 1-frame
        // chunks): every jump must land on the right frame. GetFrameSync blocks until
        // the chunk is loaded, so this is deterministic (GetFrameRange is best-effort
        // by design -- it only returns already-cached frames).
        for (int i : {7, 0, 6, 2, 5, 3, 7, 1}) {
            const VTX::Frame* f = recovered->GetFrameSync(i);
            ASSERT_NE(f, nullptr) << "seek to frame " << i;
            EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i) << "seek to frame " << i;
        }

        // The recovered file passes whole-replay validation (schema + every frame).
        const VTX::ValidationReport report = VTX::ValidateReplayFile(crash_path);
        EXPECT_FALSE(report.HasErrors()) << report.ToString();

        // Every footer time field matches the clean control exactly: per-frame
        // timestamps, duration, timeline gaps, and game segments.
        const VTX::FileFooter cf = control->GetFooter();
        const VTX::FileFooter rf = recovered->GetFooter();
        ASSERT_EQ(cf.times.game_time.size(), 8u);
        EXPECT_EQ(rf.times.game_time, cf.times.game_time);
        ASSERT_EQ(cf.times.created_utc.size(), 8u);
        EXPECT_EQ(rf.times.created_utc, cf.times.created_utc);
        EXPECT_FALSE(cf.times.gaps.empty()); // the UTC jump must register in the control...
        EXPECT_EQ(rf.times.gaps, cf.times.gaps);
        EXPECT_FALSE(cf.times.segments.empty()); // ...and so must the game-time reversal
        EXPECT_EQ(rf.times.segments, cf.times.segments);
        EXPECT_FLOAT_EQ(rf.duration_seconds, cf.duration_seconds);
        EXPECT_GT(rf.duration_seconds, 0.0f);
    }

} // namespace

TEST(CrashRecoveryE2E, RealWriterCrashMatchesCleanStopExactly) {
    RunRealWriterCrashVsControl<VTX::FlatBuffersVtxPolicy>("fb");
}

TEST(CrashRecoveryE2E, RealWriterCrashMatchesCleanStopExactly_Protobuf) {
    RunRealWriterCrashVsControl<VTX::ProtobufVtxPolicy>("pb");
}

// Same contract with async I/O: the recovered output must be indistinguishable from a clean
// SYNCHRONOUS run -- every frame, every content hash, and every footer time field (per-frame
// times, duration, timeline gaps, game segments). Nothing about the journal or the repair
// path may behave differently just because the writes came off a worker thread.
TEST(CrashRecoveryE2E, AsyncWriterCrashMatchesCleanStopExactly) {
    RunRealWriterCrashVsControl<VTX::FlatBuffersVtxPolicy>("fb_async", /*async_crash=*/true);
}

TEST(CrashRecoveryE2E, AsyncWriterCrashMatchesCleanStopExactly_Protobuf) {
    RunRealWriterCrashVsControl<VTX::ProtobufVtxPolicy>("pb_async", /*async_crash=*/true);
}

namespace {

    // A frame big enough (~4KB) that its serialized chunk clears the 512-byte floor and
    // zstd compression actually engages -- the small frames used elsewhere are always
    // stored raw, which would leave the compressed-payload path through the journal's F
    // records and the repair checksums untested.
    VTX::Frame BuildBigFrame(int i, std::string& out_blob) {
        out_blob.clear();
        out_blob.reserve(4096);
        while (out_blob.size() < 4000)
            out_blob += "payload-" + std::to_string(i) + "-";
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {"player_0", out_blob};
        pc.int32_properties = {1, i, 0};
        pc.float_properties = {100.0f, 50.0f};
        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

} // namespace

// Crash + recovery across the sink's whole configuration matrix (durable_writes x
// b_use_compression), with frames large enough that compression genuinely engages:
// committed chunks AND journaled F payloads take the zstd path, and repair's checksum
// verification runs over compressed bytes.
TEST(CrashRecoveryE2E, ConfigMatrixWithCompressedPayloads) {
    struct Cfg {
        bool durable;
        bool compression;
        const char* tag;
    };
    const Cfg configs[] = {{true, true, "d1c1"}, {true, false, "d1c0"}, {false, true, "d0c1"}, {false, false, "d0c0"}};

    for (const auto& c : configs) {
        SCOPED_TRACE(c.tag);
        const std::string path = VtxTest::OutputPath(std::string("matrix_") + c.tag + ".vtx");
        std::vector<std::string> blobs(5);
        {
            auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2, c.durable, c.compression);
            for (int i = 0; i < 5; ++i) {
                auto frame = BuildBigFrame(i, blobs[static_cast<size_t>(i)]);
                VTX::GameTime::GameTimeRegister t;
                t.game_time = float(i) / 60.0f;
                const auto res = w->TryRecordFrame(frame, t);
                ASSERT_TRUE(res.written) << res.error.message;
            }
            // dropped without Stop(): 2 committed chunks (0-1, 2-3) + frame 4 in flight
        }
        ASSERT_TRUE(VTX::ReplayNeedsRecovery(path));

        const auto rr = VTX::RepairReplayFile(path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_EQ(rr.recovered_frames, 5);

        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error;
        ctx->WaitUntilReady();
        EXPECT_EQ(ctx->GetTotalFrames(), 5);
        for (int i : {0, 3, 4}) { // a committed frame, a chunk-1 frame, the recovered pending frame
            const VTX::Frame* f = ctx->GetFrameSync(i);
            ASSERT_NE(f, nullptr) << "frame " << i;
            EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i);
            EXPECT_EQ(f->GetBuckets()[0].entities[0].string_properties[1], blobs[static_cast<size_t>(i)])
                << "big payload mismatch at frame " << i;
        }
    }
}

// A crash DURING a previous repair (file already truncated, one pending frame already
// re-appended, footer not yet written, journal still present) must be repairable by
// simply running the repair again -- it re-truncates to the last committed chunk and
// re-appends everything.
TEST(CrashRecoveryE2E, InterruptedRepairRerunsCleanly) {
    const Recording rec = CaptureRecording("interrupted", 5, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("interrupted", rec, /*committed=*/3, /*pending=*/2);

    // Simulate the first repair dying mid-way: it had appended pending frame 3 as a
    // chunk (no C record covers it) and was killed before writing the footer.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const uint32_t payload_size = rec.chunks[3].chunk_size_bytes - static_cast<uint32_t>(sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
        out.write(rec.payloads[3].data(), static_cast<std::streamsize>(rec.payloads[3].size()));
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 5); // 3 committed + both pending, no duplicates

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 5);
    const VTX::Frame* f = ctx->GetFrameSync(4);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 4);
}

// Repair is terminal: a second invocation reports clean and does not modify the file.
TEST(CrashRecoveryE2E, SecondRepairIsCleanNoOp) {
    const Recording rec = CaptureRecording("secondrep", 3, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("secondrep", rec, /*committed=*/3, /*pending=*/0);

    const auto first = VTX::RepairReplayFile(path);
    ASSERT_TRUE(first.ok()) << first.error;
    EXPECT_TRUE(first.repaired);
    const auto size_after = std::filesystem::file_size(path);

    const auto second = VTX::RepairReplayFile(path);
    ASSERT_TRUE(second.ok()) << second.error;
    EXPECT_TRUE(second.was_clean);
    EXPECT_FALSE(second.repaired);
    EXPECT_EQ(std::filesystem::file_size(path), size_after); // untouched

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
}

// With the journal opted out, a crash leaves a footerless file and NO sidecar: repair
// must report clean and leave the bytes untouched (no journal -> nothing to apply),
// and the reader must reject the footerless file gracefully rather than crash.
TEST(CrashRecoveryE2E, JournalDisabledCrashIsLeftUntouched) {
    const std::string path = VtxTest::OutputPath("nojournal_crash.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2, /*durable=*/true,
                                                          /*compression=*/true, /*journal=*/false);
        RecordEightFrames(*w);
        // dropped without Stop()
    }
    ASSERT_FALSE(VTX::ReplayNeedsRecovery(path)); // opted out -> no sidecar

    const auto bytes_before = VtxTest::ReadAllBytes(path);
    ASSERT_FALSE(bytes_before.empty());

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.was_clean); // no journal -> repair has nothing to do
    EXPECT_FALSE(rr.repaired);
    EXPECT_EQ(VtxTest::ReadAllBytes(path), bytes_before); // byte-identical

    // The footerless file is rejected cleanly by the reader (no crash, no reader).
    auto ctx = VTX::OpenReplayFile(path);
    EXPECT_FALSE(ctx.Loaded());
}

// Crash in the middle of CommitChunk: the chunk's bytes are fsync'd in the .vtx but
// its C record is torn. Append-only journaling means the batch's F records are still
// present, so the frames are recovered from them -- nothing durable is lost.
TEST(CrashRecoveryE2E, TornCommitRecordFallsBackToFrameRecords) {
    const Recording rec = CaptureRecording("torncommit", 4, /*per_chunk=*/1, Fmt::Flat);
    // 3 committed chunks + F record for frame 3 (its chunk not yet committed).
    const std::string path =
        BuildInterleaved("torncommit", rec, /*committed=*/3, /*pending=*/1, /*threshold=*/~uint64_t(0));

    // Simulate the crash window: chunk 3's bytes reached the .vtx (data-before-journal)...
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const uint32_t payload_size = rec.chunks[3].chunk_size_bytes - static_cast<uint32_t>(sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size));
        out.write(rec.payloads[3].data(), static_cast<std::streamsize>(rec.payloads[3].size()));
    }
    // ...but its C record tore mid-write (header promises 32 payload bytes; EOF cuts it).
    {
        std::ofstream j(VTX::RecoveryJournal::PathFor(path), std::ios::binary | std::ios::app);
        const char head[5] = {'C', 32, 0, 0, 0};
        j.write(head, sizeof(head));
        const char partial[12] = {3, 0, 0, 0, 3, 0, 0, 0, 3, 0, 0, 0};
        j.write(partial, sizeof(partial));
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4); // frame 3 recovered from its F record

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 4);
    const VTX::Frame* f = ctx->GetFrameSync(3);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 3);
}

// A crash during journal compaction can leave a stale ".recovery.compact" temp next
// to the (still intact) journal. Repair must ignore it, succeed from the journal, and
// clean both up.
TEST(CrashRecoveryE2E, StaleCompactTempIsIgnoredAndCleaned) {
    const Recording rec = CaptureRecording("staletemp", 3, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("staletemp", rec, /*committed=*/3, /*pending=*/0);

    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    const std::string temp_path = VTX::RecoveryJournal::CompactTempFor(journal_path);
    {
        std::ofstream tmp(temp_path, std::ios::binary);
        tmp << "half-written compaction temp";
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 3);
    EXPECT_FALSE(std::filesystem::exists(journal_path));
    EXPECT_FALSE(std::filesystem::exists(temp_path));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
}

// A crash at the very instant the session opened: the .vtx holds only its header and
// the journal's 'S' record tore mid-write. Per design, "no chunks yet" recovers to a
// valid, openable 0-frame file (the journal header is intact, so repair proceeds with
// an empty record set).
TEST(CrashRecoveryE2E, SessionStartCrashTornTimingRecordYieldsEmptyFile) {
    const Recording rec = CaptureRecording("sessionstart", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("sessionstart", rec, /*committed=*/0, /*pending=*/0);

    // Cut into the 'S' record, 3 bytes past the journal header: [VTXR|ver|magic| S..
    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    std::filesystem::resize_file(journal_path, VTX::RecoveryJournal::kHeaderSize + 3);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 0);
    EXPECT_FALSE(std::filesystem::exists(journal_path));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 0);
}

// A crash tearing the T records of a commit (C durable, T torn): the frame times must
// fall back to the still-present F records (append-only keeps them), so the recovered
// footer's timestamps stay exact -- not zeros.
TEST(CrashRecoveryE2E, TornTimeRecordsFallBackToFrameRecordTimes) {
    const Recording rec = CaptureRecording("torntime", 2, /*per_chunk=*/1, Fmt::Flat);
    // 1 committed chunk (frame 0): journal = [S][F0][C0][T0].
    const std::string path = BuildInterleaved("torntime", rec, /*committed=*/1, /*pending=*/0,
                                              /*threshold=*/~uint64_t(0));

    // Tear into T0 (the last record: [u8 'T'][u32 20][20-byte payload][u64 checksum]).
    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    const auto jsz = std::filesystem::file_size(journal_path);
    std::filesystem::resize_file(journal_path, jsz - 20);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 1);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 1);
    const VTX::FileFooter footer = ctx->GetFooter();
    ASSERT_EQ(footer.times.game_time.size(), 1u);
    // Exact time recovered from the F record despite the torn T record.
    EXPECT_EQ(static_cast<int64_t>(footer.times.game_time[0]), rec.game_times[0]);
    ASSERT_EQ(footer.times.created_utc.size(), 1u);
    EXPECT_EQ(static_cast<int64_t>(footer.times.created_utc[0]), rec.created_utc[0]);
}

// A 0-byte journal (crash between the sidecar's creation and its header write) is
// refused conservatively -- and the main file must not be touched.
TEST(CrashRecoveryE2E, EmptyJournalIsRefusedWithoutTouchingFile) {
    const Recording rec = CaptureRecording("emptyjournal", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("emptyjournal", rec, /*committed=*/2, /*pending=*/0);

    // Replace the journal with an empty file.
    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    { std::ofstream truncate_it(journal_path, std::ios::binary | std::ios::trunc); }
    ASSERT_EQ(std::filesystem::file_size(journal_path), 0u);

    const auto bytes_before = VtxTest::ReadAllBytes(path);
    const auto rr = VTX::RepairReplayFile(path);
    EXPECT_FALSE(rr.ok()); // unreadable journal header -> refused
    EXPECT_FALSE(rr.repaired);
    EXPECT_EQ(VtxTest::ReadAllBytes(path), bytes_before); // main file untouched
}

// Compaction through the REAL sink (not just the journal API): with the sink's
// compaction threshold forced to 1 byte, every commit triggers a full compaction
// cycle (close -> rewrite -> atomic rename -> reopen) during recording; a crash after
// that must still recover every frame with exact times.
TEST(CrashRecoveryE2E, RealSinkCompactionCrashRecovery) {
    const std::string path = VtxTest::OutputPath("sink_compact.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2, /*durable=*/true,
                                                          /*compression=*/true, /*journal=*/true,
                                                          /*compact_threshold=*/1);
        for (int i = 0; i < 7; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            const auto res = w->TryRecordFrame(frame, t);
            ASSERT_TRUE(res.written) << res.error.message;
        }
        // dropped: 3 committed chunks (each triggering a compaction) + frame 6 in flight
    }
    ASSERT_TRUE(VTX::ReplayNeedsRecovery(path));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 7);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 7);
    for (int i : {0, 5, 6}) {
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr) << "frame " << i;
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i);
    }
    const VTX::FileFooter footer = ctx->GetFooter();
    ASSERT_EQ(footer.times.game_time.size(), 7u); // exact per-frame times survived compaction
}

namespace {

    // Little-endian append helpers for hand-rolled journal records.
    void PushI32(std::vector<uint8_t>& b, int32_t v) {
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    }
    void PushI64(std::vector<uint8_t>& b, int64_t v) {
        for (int i = 0; i < 8; ++i)
            b.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i)));
    }

    // Append one well-formed (checksummed) record to an existing journal file --
    // used to inject hostile-but-valid records the write API would never produce.
    void AppendRawJournalRecord(const std::string& journal_path, char type, const std::vector<uint8_t>& payload) {
        uint8_t head[5];
        head[0] = static_cast<uint8_t>(type);
        const uint32_t len = static_cast<uint32_t>(payload.size());
        std::memcpy(head + 1, &len, sizeof(len));
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        XXH3_64bits_update(&state, head, 5);
        if (!payload.empty())
            XXH3_64bits_update(&state, payload.data(), payload.size());
        const uint64_t checksum = XXH3_64bits_digest(&state);
        std::ofstream j(journal_path, std::ios::binary | std::ios::app);
        j.write(reinterpret_cast<const char*>(head), 5);
        if (!payload.empty())
            j.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        j.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    }

} // namespace

// Checksummed-but-hostile C records (an offset pointing inside the header; a size no
// bigger than its own length prefix) must be dropped by the repair guards, degrading
// to a valid 0-frame file rather than corrupting or crashing.
TEST(CrashRecoveryE2E, BogusJournalChunkRecordsAreDroppedSafely) {
    const Recording rec = CaptureRecording("boguschunk", 2, /*per_chunk=*/1, Fmt::Flat);

    struct Bogus {
        const char* tag;
        int64_t offset;
        uint32_t size;
    };
    const Bogus cases[] = {
        {"offset_inside_header", 4, 64}, // points into the .vtx header region
        {"size_below_prefix", 100, 4},   // chunk cannot even hold its length prefix
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.tag);
        // Header-only body + a journal whose only C record is hostile.
        const std::string path = FabricateCrash(std::string("bogus_") + c.tag, rec, /*committed=*/0, /*pending=*/0);
        VTX::ChunkIndexData cd;
        cd.chunk_index = 0;
        cd.file_offset = c.offset;
        cd.start_frame = 0;
        cd.end_frame = 0;
        cd.chunk_size_bytes = c.size;
        cd.checksum = 0;
        {
            // Rebuild the journal with the hostile record (Open truncates the sidecar).
            VTX::RecoveryJournal j;
            ASSERT_TRUE(j.Open(VTX::RecoveryJournal::PathFor(path), rec.format_magic, /*durable=*/true));
            j.CommitChunk(cd, {{0, rec.game_times[0], rec.created_utc[0]}});
            j.Close();
        }

        const auto rr = VTX::RepairReplayFile(path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_EQ(rr.recovered_chunks, 0); // hostile record dropped
        EXPECT_EQ(rr.recovered_frames, 0);

        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error;
        ctx->WaitUntilReady();
        EXPECT_EQ(ctx->GetTotalFrames(), 0); // degraded to a valid empty file
    }
}

// An F record with an EMPTY frame payload (valid checksum, nothing to append) must
// stop the pending-frame walk at that hole without corrupting what came before.
TEST(CrashRecoveryE2E, EmptyPendingFramePayloadStopsPendingRecovery) {
    const Recording rec = CaptureRecording("emptyf", 3, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("emptyf", rec, /*committed=*/1, /*pending=*/0);

    // Hand-append: F(frame 1, EMPTY payload), then a well-formed F(frame 2). The empty
    // one is a hole, so frame 2 must NOT be applied either (indices would gap).
    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    {
        std::vector<uint8_t> empty_f;
        PushI32(empty_f, 1);
        PushI64(empty_f, rec.game_times[1]);
        PushI64(empty_f, rec.created_utc[1]); // no payload bytes after the 20-byte prefix
        AppendRawJournalRecord(journal_path, 'F', empty_f);

        std::vector<uint8_t> good_f;
        PushI32(good_f, 2);
        PushI64(good_f, rec.game_times[2]);
        PushI64(good_f, rec.created_utc[2]);
        good_f.insert(good_f.end(), rec.payloads[2].begin(), rec.payloads[2].end());
        AppendRawJournalRecord(journal_path, 'F', good_f);
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 1); // committed chunk only; the empty F is a hole

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 1);
}

// A main file whose own header tore (smaller than magic + size prefix) with a valid
// journal alongside: repair must refuse gracefully and leave the bytes alone.
TEST(CrashRecoveryE2E, TornMainHeaderWithJournalIsRefused) {
    const Recording rec = CaptureRecording("tornhdr", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("tornhdr", rec, /*committed=*/1, /*pending=*/0);
    std::filesystem::resize_file(path, 6); // magic + half the header_size field

    const auto bytes_before = VtxTest::ReadAllBytes(path);
    const auto rr = VTX::RepairReplayFile(path);
    EXPECT_FALSE(rr.ok()); // header framing not intact -> refused
    EXPECT_FALSE(rr.repaired);
    EXPECT_EQ(VtxTest::ReadAllBytes(path), bytes_before); // untouched
}

namespace {

    // Records 8 frames with DECREASING game time (is_increasing = false) and one
    // increase at frame 5 -> a game segment in decreasing mode. Only game_time is
    // supplied (UTC is faked by the timer), matching how a rewind-style recording
    // would drive the writer.
    template <typename Writer>
    void RecordEightDecreasingFrames(Writer& writer) {
        const float game_times[8] = {8.f / 60, 7.f / 60, 6.f / 60, 5.f / 60, 4.f / 60, 6.f / 60 /*reversal*/,
                                     3.f / 60, 2.f / 60};
        for (int i = 0; i < 8; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = game_times[i];
            const auto res = writer.TryRecordFrame(frame, t);
            ASSERT_TRUE(res.written) << "frame " << i << ": " << res.error.message;
        }
    }

} // namespace

// DECREASING-time recordings (is_increasing = false) exercise the other branch of the
// segment reconstruction: after a crash, the recovered game times, game segments and
// duration must match a cleanly-stopped control. (Absolute created_utc values are
// faked from the wall clock and differ run-to-run, so only their count is compared.)
TEST(CrashRecoveryE2E, DecreasingTimeCrashMatchesCleanStop) {
    auto make_writer = [](const std::string& path) {
        using Policy = VTX::FlatBuffersVtxPolicy;
        typename RawWriterFor<Policy>::Config cfg;
        cfg.sink_config.filename = path;
        cfg.sink_config.header_config.replay_name = "CrashRecoveryE2E";
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.default_fps = 60.0f;
        cfg.is_increasing = false;
        cfg.chunker_config.max_frames = 3;
        return std::make_unique<RawWriterFor<Policy>>(cfg);
    };

    const std::string control_path = VtxTest::OutputPath("dec_control.vtx");
    {
        auto w = make_writer(control_path);
        RecordEightDecreasingFrames(*w);
        w->Stop();
    }
    const std::string crash_path = VtxTest::OutputPath("dec_crash.vtx");
    {
        auto w = make_writer(crash_path);
        RecordEightDecreasingFrames(*w);
        // dropped without Stop()
    }

    const auto rr = VTX::RepairReplayFile(crash_path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 8);

    auto control = VTX::OpenReplayFile(control_path);
    ASSERT_TRUE(control) << control.error;
    control->WaitUntilReady();
    auto recovered = VTX::OpenReplayFile(crash_path);
    ASSERT_TRUE(recovered) << recovered.error;
    recovered->WaitUntilReady();

    const VTX::FileFooter cf = control->GetFooter();
    const VTX::FileFooter rf = recovered->GetFooter();
    ASSERT_EQ(cf.times.game_time.size(), 8u);
    EXPECT_EQ(rf.times.game_time, cf.times.game_time);
    EXPECT_EQ(rf.times.created_utc.size(), cf.times.created_utc.size());
    EXPECT_FALSE(cf.times.segments.empty()); // the mid-run increase registers in decreasing mode
    EXPECT_EQ(rf.times.segments, cf.times.segments);
    EXPECT_FLOAT_EQ(rf.duration_seconds, cf.duration_seconds);
}

namespace {

    // A Player carrying a Map field (AmmoByWeapon: weapon -> AmmoEntry), per
    // test_schema_map.json. Mirrors the differ's map fixture.
    VTX::PropertyContainer MakeMapPlayer(int i) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0; // Player
        pc.string_properties = {"map_player", "name"};
        pc.int32_properties = {1, i, 0};
        pc.float_properties = {100.0f, 50.0f};
        pc.vector_properties = {VTX::Vector {}, VTX::Vector {}};
        pc.quat_properties = {VTX::Quat {}};
        pc.bool_properties = {true};

        VTX::MapContainer ammo;
        ammo.keys.push_back("Rifle");
        VTX::PropertyContainer rifle;
        rifle.entity_type_id = 5; // AmmoEntry
        rifle.string_properties = {"Rifle"};
        rifle.int32_properties = {30 - i, 90}; // clip drains per frame
        rifle.content_hash = VTX::Helpers::CalculateContainerHash(rifle);
        ammo.values.push_back(std::move(rifle));
        pc.map_properties.push_back(std::move(ammo));

        pc.content_hash = VTX::Helpers::CalculateContainerHash(pc);
        return pc;
    }

} // namespace

namespace {

    // Frames with MAP containers survive crash + recovery intact -- including a frame
    // that only ever existed as a journaled F record (the pending one).
    template <typename Policy>
    void RunMapCrashRecovery(const std::string& prefix) {
        const std::string path = VtxTest::OutputPath(prefix + "_map_crash.vtx");
        {
            auto w = MakeRawWriter<Policy>(path, /*chunk_max_frames=*/2, /*durable=*/true,
                                           /*compression=*/true, /*journal=*/true,
                                           /*compact_threshold=*/0, "test_schema_map.json");
            for (int i = 0; i < 3; ++i) {
                VTX::Frame frame;
                auto& b = frame.CreateBucket("entity");
                b.unique_ids.push_back("map_player");
                b.entities.push_back(MakeMapPlayer(i));
                VTX::GameTime::GameTimeRegister t;
                t.game_time = float(i) / 60.0f;
                const auto res = w->TryRecordFrame(frame, t);
                ASSERT_TRUE(res.written) << res.error.message;
            }
            // dropped: 1 committed chunk (frames 0-1) + frame 2 in flight
        }

        const auto rr = VTX::RepairReplayFile(path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_EQ(rr.recovered_frames, 3);

        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error;
        ctx->WaitUntilReady();
        EXPECT_EQ(ctx->GetTotalFrames(), 3);
        for (int i : {0, 2}) { // a committed frame and the F-record-recovered pending frame
            const VTX::Frame* f = ctx->GetFrameSync(i);
            ASSERT_NE(f, nullptr) << "frame " << i;
            const auto& entity = f->GetBuckets()[0].entities[0];
            ASSERT_FALSE(entity.map_properties.empty()) << "frame " << i;
            const auto& ammo = entity.map_properties[0];
            ASSERT_EQ(ammo.keys.size(), 1u) << "frame " << i;
            EXPECT_EQ(ammo.keys[0], "Rifle");
            ASSERT_EQ(ammo.values.size(), 1u);
            EXPECT_EQ(ammo.values[0].int32_properties[0], 30 - i) << "frame " << i;
        }
    }

} // namespace

TEST(CrashRecoveryE2E, MapFramesSurviveCrashRecovery) {
    RunMapCrashRecovery<VTX::FlatBuffersVtxPolicy>("fb");
}

TEST(CrashRecoveryE2E, MapFramesSurviveCrashRecovery_Protobuf) {
    RunMapCrashRecovery<VTX::ProtobufVtxPolicy>("pb");
}

// A crashed session leaves a sidecar; a LATER session to the same path that OPTS OUT
// of journaling must clear it at session start -- otherwise the stale journal would
// masquerade as recovery state for the new recording.
TEST(CrashRecoveryE2E, StaleSidecarRemovedWhenJournalingOptedOut) {
    const std::string path = VtxTest::OutputPath("stale_sidecar.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2);
        for (int i = 0; i < 3; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            ASSERT_TRUE(w->TryRecordFrame(frame, t).written);
        }
        // session 1 crashes -> sidecar left behind
    }
    ASSERT_TRUE(VTX::ReplayNeedsRecovery(path));

    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2, /*durable=*/true,
                                                          /*compression=*/true, /*journal=*/false);
        // The opted-out session must have removed the stale sidecar at start.
        EXPECT_FALSE(VTX::ReplayNeedsRecovery(path));
        auto frame = BuildFrame(0);
        VTX::GameTime::GameTimeRegister t;
        t.game_time = 0.0f;
        ASSERT_TRUE(w->TryRecordFrame(frame, t).written);
        // session 2 crashes too -- journaling was off, so no sidecar may appear
    }
    EXPECT_FALSE(VTX::ReplayNeedsRecovery(path)); // no stale recovery signal
}

// A user file that merely shares the ".recovery" suffix (not a journal -- no "VTXR"
// magic) must never be deleted by repair, even on the clean-file path that normally
// cleans the sidecar up.
TEST(CrashRecoveryE2E, ForeignRecoveryFileIsNeverDeleted) {
    const Recording rec = CaptureRecording("foreign", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = VtxTest::OutputPath("cap_foreign.vtx"); // valid, cleanly closed
    ASSERT_TRUE(std::filesystem::exists(path));

    const std::string foreign_path = VTX::RecoveryJournalPath(path);
    const std::string foreign_content = "user notes -- definitely not a VTXR journal";
    {
        std::ofstream f(foreign_path, std::ios::binary);
        f << foreign_content;
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.was_clean);
    ASSERT_TRUE(std::filesystem::exists(foreign_path)); // preserved, not cleaned up
    {
        std::ifstream f(foreign_path, std::ios::binary);
        std::string readback((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(readback, foreign_content);
    }
    (void)rec;
}

// A checksummed T record whose frame index is out of range (beyond any recovered
// frame) must be ignored by the bounds check, leaving the valid times untouched.
TEST(CrashRecoveryE2E, HostileTimeRecordIndexOutOfRangeIsIgnored) {
    const Recording rec = CaptureRecording("hostilet", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("hostilet", rec, /*committed=*/2, /*pending=*/0);

    std::vector<uint8_t> hostile_t;
    PushI32(hostile_t, 9999); // far beyond total_frames
    PushI64(hostile_t, 123456789);
    PushI64(hostile_t, 987654321);
    AppendRawJournalRecord(VTX::RecoveryJournal::PathFor(path), 'T', hostile_t);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 2);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    const VTX::FileFooter footer = ctx->GetFooter();
    ASSERT_EQ(footer.times.game_time.size(), 2u); // not resized by the hostile index
    for (int i = 0; i < 2; ++i)
        EXPECT_EQ(static_cast<int64_t>(footer.times.game_time[static_cast<size_t>(i)]), rec.game_times[i]);
}

// A stale journal from a DIFFERENT recording (same format, e.g. the file was replaced
// out-of-band) must degrade gracefully: the per-chunk checksums reject the foreign
// chunks and repair yields a valid (empty) file instead of corrupt data or a crash.
TEST(CrashRecoveryE2E, StaleJournalFromDifferentRecordingDegradesGracefully) {
    const Recording rec_a = CaptureRecording("stalex_a", 2, /*per_chunk=*/1, Fmt::Flat);
    // Same shape, different content (scores 50/51 vs 0/1) -> same offsets, different bytes.
    Recording rec_b;
    {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("cap_stalex_b.vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "CrashRecoveryTest";
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_NE(writer, nullptr);
        for (int i = 0; i < 2; ++i) {
            auto frame = BuildFrame(50 + i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
            writer->Flush();
        }
        writer->Stop();
    }

    // Crash state whose BODY is from recording B...
    const std::string path = VtxTest::OutputPath("fab_stalex.vtx");
    {
        const std::string b_path = VtxTest::OutputPath("cap_stalex_b.vtx");
        std::ifstream in(b_path, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // keep header + everything up to the footer region intact enough: reuse A's
        // chunk extents (identical layout) to compute the footerless length
        int64_t body_end = static_cast<int64_t>(rec_a.header.size());
        for (const auto& c : rec_a.chunks)
            body_end += c.chunk_size_bytes;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(all.data(), std::min<std::streamsize>(static_cast<std::streamsize>(all.size()), body_end));
    }
    // ...but whose journal carries recording A's chunk records (stale metadata).
    {
        VTX::RecoveryJournal j;
        ASSERT_TRUE(j.Open(VTX::RecoveryJournal::PathFor(path), "VTXF", /*durable=*/true));
        for (size_t i = 0; i < rec_a.chunks.size(); ++i)
            j.CommitChunk(rec_a.chunks[i], {{static_cast<int32_t>(i), rec_a.game_times[i], rec_a.created_utc[i]}});
        j.Close();
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_chunks, 0); // foreign chunks rejected by checksum
    EXPECT_EQ(rr.recovered_frames, 0);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error; // degraded to a valid, openable empty file
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 0);
}

// A crashed file that sits READ-ONLY (archived, restored from backup) must make
// repair fail gracefully -- and once the file is writable again, the SAME journal
// must still drive a full recovery (the refusal destroyed nothing).
TEST(CrashRecoveryE2E, ReadOnlyFileRepairFailsGracefullyThenSucceeds) {
    const Recording rec = CaptureRecording("readonly", 3, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("readonly", rec, /*committed=*/3, /*pending=*/0);

    // MSVC's std::filesystem maps the Windows READONLY attribute to the write bits
    // COLLECTIVELY -- all three must be removed for the file to become read-only.
    const auto all_write = std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                           std::filesystem::perms::others_write;
    std::filesystem::permissions(path, all_write, std::filesystem::perm_options::remove);
    const auto rr_denied = VTX::RepairReplayFile(path);
    // Restore write access BEFORE asserting so a failure can't leave a read-only
    // artifact behind for later runs.
    std::filesystem::permissions(path, all_write, std::filesystem::perm_options::add);

    EXPECT_FALSE(rr_denied.ok()); // cannot truncate/append a read-only file
    EXPECT_FALSE(rr_denied.repaired);
    EXPECT_TRUE(VTX::ReplayNeedsRecovery(path)); // journal preserved for a retry
    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 3);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
}

// A recording that never supplies ANY time registry (EGameTimeType::None -- both
// game_time and created_utc synthesized from FPS) crashes and recovers with the
// exact synthesized timeline: game times in fps_inverse steps from 0, monotonic
// created_utc, and a coherent duration.
TEST(CrashRecoveryE2E, NoTimeRegistryCrashRecovery) {
    const std::string path = VtxTest::OutputPath("notime_crash.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2);
        for (int i = 0; i < 5; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t; // both fields nullopt -> fully faked times
            ASSERT_TRUE(w->TryRecordFrame(frame, t).written);
        }
        // dropped: 2 committed chunks + 1 in-flight frame
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 5);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 5);

    const VTX::FileFooter footer = ctx->GetFooter();
    ASSERT_EQ(footer.times.game_time.size(), 5u);
    ASSERT_EQ(footer.times.created_utc.size(), 5u);
    // FakeBothTimesFromFPS: game_time starts at 0 and advances fps_inverse per frame.
    const int64_t fps_inverse =
        static_cast<int64_t>((1.0f / 60.0f) * static_cast<float>(VTX::GameTime::TICKS_PER_SECOND));
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(static_cast<int64_t>(footer.times.game_time[static_cast<size_t>(i)]), i * fps_inverse)
            << "game_time[" << i << "]";
    for (int i = 1; i < 5; ++i)
        EXPECT_EQ(footer.times.created_utc[static_cast<size_t>(i)] -
                      footer.times.created_utc[static_cast<size_t>(i) - 1],
                  static_cast<uint64_t>(fps_inverse))
            << "created_utc step at " << i;
    EXPECT_NEAR(footer.duration_seconds, 4.0f / 60.0f, 1e-4f);
}

// Chunk splitting driven by the BYTE budget (max_bytes) rather than the frame count
// -- the other branch of ThresholdChunkPolicy -- through crash + recovery.
TEST(CrashRecoveryE2E, ByteBudgetChunkingCrashRecovery) {
    const std::string path = VtxTest::OutputPath("bytebudget_crash.vtx");
    std::vector<std::string> blobs(7);
    {
        using Policy = VTX::FlatBuffersVtxPolicy;
        typename RawWriterFor<Policy>::Config cfg;
        cfg.sink_config.filename = path;
        cfg.sink_config.header_config.replay_name = "CrashRecoveryE2E";
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.default_fps = 60.0f;
        cfg.chunker_config.max_frames = 1000;  // never reached
        cfg.chunker_config.max_bytes = 10'000; // ~2 big frames per chunk
        auto w = std::make_unique<RawWriterFor<Policy>>(cfg);
        for (int i = 0; i < 7; ++i) {
            auto frame = BuildBigFrame(i, blobs[static_cast<size_t>(i)]); // ~4KB each
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            ASSERT_TRUE(w->TryRecordFrame(frame, t).written);
        }
        // dropped without Stop()
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 7);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 7);
    EXPECT_GE(ctx->GetSeekTable().size(), 3u); // byte budget actually split the chunks
    for (int i : {0, 3, 6}) {
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr) << "frame " << i;
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i);
        EXPECT_EQ(f->GetBuckets()[0].entities[0].string_properties[1], blobs[static_cast<size_t>(i)])
            << "payload mismatch at frame " << i;
    }
}

namespace {

    // The maximal assertion: a crash EXACTLY at a chunk boundary (all frames flushed,
    // nothing in flight) must recover to a file that is BYTE-IDENTICAL to one produced
    // by a clean Stop() with the same inputs -- same chunks, same seek table, same
    // footer (times, gaps, segments, duration, compression), same trailer.
    //
    // The file header embeds a SECOND-granularity recording timestamp and is
    // zstd-compressed, so a control/crash pair recorded across a second boundary gets
    // different header bytes -- and possibly a different compressed header SIZE, which
    // shifts every absolute offset in the seek table (a legitimate difference; repair
    // never rewrites the header). Full-file identity is therefore only meaningful for
    // a SAME-SECOND pair: re-record the pair (bounded retries, each takes well under a
    // second) until both headers are byte-identical, then demand total equality.
    template <typename Policy>
    void RunBoundaryByteIdentity(const std::string& prefix, int frames, int32_t chunk_max) {
        auto record_all = [frames](RawWriterFor<Policy>& w) {
            constexpr int64_t kBaseUtc = 4'000'000'000'000;
            for (int i = 0; i < frames; ++i) {
                auto frame = BuildFrame(i);
                VTX::GameTime::GameTimeRegister t;
                t.game_time = float(i) / 60.0f;
                t.created_utc_time = kBaseUtc + (i + 1) * 166'667;
                ASSERT_TRUE(w.TryRecordFrame(frame, t).written);
            }
            w.Flush(); // land the trailing batch -> crash sits exactly on a chunk boundary
        };
        auto header_region = [](const std::vector<std::byte>& bytes) {
            if (bytes.size() < 8)
                return std::vector<std::byte>();
            uint32_t header_size = 0;
            std::memcpy(&header_size, bytes.data() + 4, sizeof(header_size));
            const size_t end = static_cast<size_t>(8) + header_size;
            if (end > bytes.size())
                return std::vector<std::byte>();
            return std::vector<std::byte>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(end));
        };

        const std::string control_path = VtxTest::OutputPath(prefix + "_ident_control.vtx");
        const std::string crash_path = VtxTest::OutputPath(prefix + "_ident_crash.vtx");

        // Flush-only durability: fsync vs fflush changes NO file bytes (and the flag is
        // not journaled), but it makes each recording take milliseconds instead of
        // seconds -- essential so a same-second pair is reachable even on slow CI
        // runners, where fsync-per-frame recordings span more than a second each.
        bool same_second_pair = false;
        for (int attempt = 0; attempt < 8 && !same_second_pair; ++attempt) {
            {
                auto w = MakeRawWriter<Policy>(control_path, chunk_max, /*durable=*/false);
                record_all(*w);
                if (::testing::Test::HasFatalFailure())
                    return;
                w->Stop();
            }
            {
                auto w = MakeRawWriter<Policy>(crash_path, chunk_max, /*durable=*/false);
                record_all(*w);
                if (::testing::Test::HasFatalFailure())
                    return;
                // dropped without Stop(): every frame is in a committed chunk, none pending
            }
            const auto control_header = header_region(VtxTest::ReadAllBytes(control_path));
            const auto crash_header = header_region(VtxTest::ReadAllBytes(crash_path));
            same_second_pair = !control_header.empty() && control_header == crash_header;
        }
        ASSERT_TRUE(same_second_pair) << "could not record a same-second control/crash pair in 8 attempts";

        const auto rr = VTX::RepairReplayFile(crash_path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_EQ(rr.recovered_frames, frames);

        const auto control_bytes = VtxTest::ReadAllBytes(control_path);
        const auto recovered_bytes = VtxTest::ReadAllBytes(crash_path);
        ASSERT_FALSE(control_bytes.empty());
        EXPECT_EQ(recovered_bytes.size(), control_bytes.size());
        EXPECT_EQ(recovered_bytes, control_bytes); // bit-for-bit equal to the clean file
    }

} // namespace

TEST(CrashRecoveryE2E, BoundaryCrashRecoversByteIdenticalFile) {
    RunBoundaryByteIdentity<VTX::FlatBuffersVtxPolicy>("fb", /*frames=*/4, /*chunk_max=*/2);
}

TEST(CrashRecoveryE2E, BoundaryCrashRecoversByteIdenticalFile_Protobuf) {
    RunBoundaryByteIdentity<VTX::ProtobufVtxPolicy>("pb", /*frames=*/4, /*chunk_max=*/2);
}

// Same identity with a LARGE footer (120 frames -> multi-KB time vectors): the
// control's footer goes through zstd in Close(), and the repair must compress its
// synthesized footer identically (settings journaled in the 'S' record).
TEST(CrashRecoveryE2E, LargeFooterBoundaryCrashIsByteIdentical) {
    RunBoundaryByteIdentity<VTX::FlatBuffersVtxPolicy>("fb_big", /*frames=*/120, /*chunk_max=*/30);
}

namespace {

    // Shared harness for the brute-force sweeps: a canonical crash state (2 committed
    // chunks + 2 pending frames) captured as pristine bytes, restored before every
    // mutation so each iteration starts from the identical on-disk state.
    struct SweepState {
        std::string path;
        std::string journal_path;
        std::vector<std::byte> main_bytes;
        std::vector<std::byte> journal_bytes;
    };

    SweepState MakeSweepState(const std::string& tag) {
        const Recording rec = CaptureRecording("sweep_" + tag, 4, /*per_chunk=*/1, Fmt::Flat);
        SweepState s;
        s.path = FabricateCrash("sweep_" + tag, rec, /*committed=*/2, /*pending=*/2);
        s.journal_path = VTX::RecoveryJournal::PathFor(s.path);
        s.main_bytes = VtxTest::ReadAllBytes(s.path);
        s.journal_bytes = VtxTest::ReadAllBytes(s.journal_path);
        EXPECT_FALSE(s.main_bytes.empty());
        EXPECT_FALSE(s.journal_bytes.empty());
        return s;
    }

    void WriteBytes(const std::string& path, const std::byte* data, size_t len) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    }

    // Invariants every mutation must uphold: repair never crashes; when it claims a
    // repair, the file opens and agrees with the reported frame count (with per-frame
    // footer times to match); when it refuses, a valid journal restored over the same
    // main file can still drive a full recovery later (checked by the caller's final
    // pristine-restore pass).
    void CheckRepairInvariants(const std::string& path, size_t iteration, bool open_check) {
        const auto rr = VTX::RepairReplayFile(path);
        if (!rr.ok())
            return; // refusal is a legal outcome; the harness restores state next round
        EXPECT_GE(rr.recovered_frames, 0) << "iteration " << iteration;
        EXPECT_LE(rr.recovered_frames, 4) << "iteration " << iteration;
        if (!open_check)
            return;
        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << "iteration " << iteration << ": repaired file failed to open";
        ctx->WaitUntilReady();
        EXPECT_EQ(ctx->GetTotalFrames(), rr.recovered_frames) << "iteration " << iteration;
        const VTX::FileFooter footer = ctx->GetFooter();
        EXPECT_EQ(footer.times.game_time.size(), static_cast<size_t>(rr.recovered_frames)) << "iteration " << iteration;
    }

} // namespace

// Brute force over EVERY possible crash point in the journal: truncate it at every
// byte length from 0 to full size and repair. No length may crash, corrupt, or
// produce a file that disagrees with the reported recovery.
TEST(CrashRecoverySweep, JournalTruncationEveryByte) {
    const SweepState s = MakeSweepState("jtrunc");
    for (size_t cut = 0; cut <= s.journal_bytes.size(); ++cut) {
        WriteBytes(s.path, s.main_bytes.data(), s.main_bytes.size());
        WriteBytes(s.journal_path, s.journal_bytes.data(), cut);
        // Full open verification sampled; repair-level invariants checked every time.
        CheckRepairInvariants(s.path, cut, /*open_check=*/(cut % 7 == 0) || cut == s.journal_bytes.size());
        if (::testing::Test::HasFatalFailure())
            return;
    }
    // Final sanity: the pristine state still recovers completely.
    WriteBytes(s.path, s.main_bytes.data(), s.main_bytes.size());
    WriteBytes(s.journal_path, s.journal_bytes.data(), s.journal_bytes.size());
    const auto rr = VTX::RepairReplayFile(s.path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4);
}

// Brute force over EVERY possible crash point in the MAIN file: truncate the .vtx at
// every byte length (with the full journal alongside) and repair.
TEST(CrashRecoverySweep, MainFileTruncationEveryByte) {
    const SweepState s = MakeSweepState("mtrunc");
    for (size_t cut = 0; cut <= s.main_bytes.size(); ++cut) {
        WriteBytes(s.path, s.main_bytes.data(), cut);
        WriteBytes(s.journal_path, s.journal_bytes.data(), s.journal_bytes.size());
        CheckRepairInvariants(s.path, cut, /*open_check=*/(cut % 7 == 0) || cut == s.main_bytes.size());
        if (::testing::Test::HasFatalFailure())
            return;
    }
}

// Brute force over single-byte corruption: flip every byte of the journal, one at a
// time, and repair. The per-record checksums must contain the damage -- never a
// crash, never a repaired file that disagrees with its own report.
TEST(CrashRecoverySweep, JournalBitFlipEveryByte) {
    const SweepState s = MakeSweepState("jflip");
    std::vector<std::byte> mutated = s.journal_bytes;
    for (size_t i = 0; i < s.journal_bytes.size(); ++i) {
        mutated[i] = s.journal_bytes[i] ^ std::byte {0xFF};
        WriteBytes(s.path, s.main_bytes.data(), s.main_bytes.size());
        WriteBytes(s.journal_path, mutated.data(), mutated.size());
        CheckRepairInvariants(s.path, i, /*open_check=*/(i % 7 == 0));
        if (::testing::Test::HasFatalFailure())
            return;
        mutated[i] = s.journal_bytes[i]; // restore for the next flip
    }
}

// Truncation sweep over a POST-COMPACTION journal shape (the round-11 sweeps use the
// uncompacted layout): every byte length of a compacted journal must repair without
// crashing or self-contradiction.
TEST(CrashRecoverySweep, CompactedJournalTruncationSweep) {
    const Recording rec = CaptureRecording("sweep_compact", 4, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = BuildInterleaved("sweep_compact", rec, /*committed=*/2, /*pending=*/2, /*threshold=*/1);
    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    const auto main_bytes = VtxTest::ReadAllBytes(path);
    const auto journal_bytes = VtxTest::ReadAllBytes(journal_path);
    ASSERT_FALSE(journal_bytes.empty());

    for (size_t cut = 0; cut <= journal_bytes.size(); cut += 2) {
        WriteBytes(path, main_bytes.data(), main_bytes.size());
        WriteBytes(journal_path, journal_bytes.data(), cut);
        CheckRepairInvariants(path, cut, /*open_check=*/(cut % 8 == 0));
        if (::testing::Test::HasFatalFailure())
            return;
    }
    // Pristine compacted journal still recovers everything.
    WriteBytes(path, main_bytes.data(), main_bytes.size());
    WriteBytes(journal_path, journal_bytes.data(), journal_bytes.size());
    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4);
}

#ifdef _WIN32

// --- REAL process kill --------------------------------------------------------
//
// Everything above simulates crashes in-process (dropping the writer still closes
// the FILE* handles). These tests spawn vtx_tests.exe AS A CHILD in a hidden
// writer-loop mode and TerminateProcess() it mid-recording: no destructors, no
// fclose, handles abandoned to the kernel -- a genuine process death. This is also
// the only honest way to validate the flush-only (durable_writes=false) claim that
// data pushed to the OS survives a process crash.

namespace {

    // Endless recording loop for the child process; terminated externally by the parent.
    template <typename Writer>
    void ChildRecordLoop(Writer& w) {
        for (int i = 0;; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            (void)w.TryRecordFrame(frame, t);
        }
    }

} // namespace

// Child mode: an endless writer loop. Skipped unless spawned with the env var set.
TEST(CrashRecoveryProcess, ChildWriterLoop) {
    const char* path = std::getenv("VTX_CHILD_WRITE_PATH");
    if (!path)
        GTEST_SKIP() << "child-mode body; only runs when spawned by the KilledMidRecording tests";
    const char* durable_env = std::getenv("VTX_CHILD_DURABLE");
    const bool durable = !durable_env || std::string(durable_env) != "0";
    const char* compact_env = std::getenv("VTX_CHILD_COMPACT_THRESHOLD");
    const uint64_t compact_threshold = compact_env ? std::strtoull(compact_env, nullptr, 10) : 0;
    const char* async_env = std::getenv("VTX_CHILD_ASYNC");
    const bool async = async_env && std::string(async_env) == "1";

    if (async) {
        // Chunk/journal I/O on a worker thread. TerminateProcess kills the worker mid-write
        // exactly like the main thread -- no unwinding, no drain, handles abandoned.
        auto w =
            MakeRawAsyncWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/10, durable,
                                                          /*compression=*/true, /*journal=*/true, compact_threshold);
        ChildRecordLoop(*w);
    } else {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/10, durable,
                                                          /*compression=*/true, /*journal=*/true, compact_threshold);
        ChildRecordLoop(*w);
    }
}

namespace {

    void RunKilledProcessTest(bool durable, const char* tag, uint64_t compact_threshold = 0,
                              uint64_t progress_threshold = 20'000, bool async = false) {
        const std::string path = VtxTest::OutputPath(std::string("killed_") + tag + ".vtx");
        const std::string journal_path = VTX::RecoveryJournalPath(path);
        std::filesystem::remove(path);
        std::filesystem::remove(journal_path);

        char exe[MAX_PATH] = {};
        ASSERT_GT(GetModuleFileNameA(nullptr, exe, MAX_PATH), 0u);
        SetEnvironmentVariableA("VTX_CHILD_WRITE_PATH", path.c_str());
        SetEnvironmentVariableA("VTX_CHILD_DURABLE", durable ? "1" : "0");
        SetEnvironmentVariableA("VTX_CHILD_ASYNC", async ? "1" : "0");
        if (compact_threshold > 0)
            SetEnvironmentVariableA("VTX_CHILD_COMPACT_THRESHOLD", std::to_string(compact_threshold).c_str());
        std::string cmd = std::string("\"") + exe + "\" --gtest_filter=CrashRecoveryProcess.ChildWriterLoop";
        STARTUPINFOA si {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi {};
        const BOOL created =
            CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        SetEnvironmentVariableA("VTX_CHILD_WRITE_PATH", nullptr);
        SetEnvironmentVariableA("VTX_CHILD_DURABLE", nullptr);
        SetEnvironmentVariableA("VTX_CHILD_ASYNC", nullptr);
        SetEnvironmentVariableA("VTX_CHILD_COMPACT_THRESHOLD", nullptr);
        ASSERT_TRUE(created);

        // Let the child journal a healthy amount of frames, then KILL it cold. (With an
        // aggressive compaction threshold the journal shrinks on every commit, so gate
        // on the MAIN file's growth instead -- it only ever grows.)
        const std::string& progress_file = (compact_threshold > 0) ? path : journal_path;
        uint64_t journal_size = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            std::error_code ec;
            const auto s = std::filesystem::file_size(progress_file, ec);
            if (!ec && s > progress_threshold) {
                journal_size = static_cast<uint64_t>(s);
                break;
            }
            Sleep(5);
        }
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 10'000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ASSERT_GT(journal_size, 0u) << "child process never made journaling progress";

        // The kernel released the child's handles; recover and verify integrity.
        ASSERT_TRUE(VTX::ReplayNeedsRecovery(path));
        const auto rr = VTX::RepairReplayFile(path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_GT(rr.recovered_frames, 0);

        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error;
        ctx->WaitUntilReady();
        ASSERT_EQ(ctx->GetTotalFrames(), rr.recovered_frames);
        if (rr.recovered_frames > 0) {
            const VTX::Frame* first = ctx->GetFrameSync(0);
            ASSERT_NE(first, nullptr);
            EXPECT_EQ(first->GetBuckets()[0].entities[0].int32_properties[1], 0);
            const int last = rr.recovered_frames - 1;
            const VTX::Frame* last_frame = ctx->GetFrameSync(last);
            ASSERT_NE(last_frame, nullptr);
            EXPECT_EQ(last_frame->GetBuckets()[0].entities[0].int32_properties[1], last);
        }
        const VTX::FileFooter footer = ctx->GetFooter();
        EXPECT_EQ(footer.times.game_time.size(), static_cast<size_t>(rr.recovered_frames));
        EXPECT_EQ(footer.times.created_utc.size(), static_cast<size_t>(rr.recovered_frames));
    }

} // namespace

// fsync-per-operation mode: even a cold kill loses nothing that was recorded.
TEST(CrashRecoveryProcess, KilledMidRecordingDurable) {
    RunKilledProcessTest(/*durable=*/true, "durable");
}

// Flush-only mode: data handed to the OS (fflush) must survive a PROCESS death (the
// documented durability tier below power-loss safety).
TEST(CrashRecoveryProcess, KilledMidRecordingFlushOnly) {
    RunKilledProcessTest(/*durable=*/false, "flushonly");
}

// Aggressive compaction (threshold 1 -> a full close/rewrite/atomic-rename cycle on
// EVERY commit): a real kill lands mid-compaction traffic sooner or later, and the
// atomic-rename design must leave either the old or the new journal -- always
// recoverable.
TEST(CrashRecoveryProcess, KilledMidRecordingWhileCompacting) {
    RunKilledProcessTest(/*durable=*/true, "compacting", /*compact_threshold=*/1);
}

// Soak: kill the child at a SPREAD of progress points -- just after the first frames,
// mid-first-chunk, after a few commits, deep into the session -- alternating durability
// and compaction cadence. Every kill timing must recover to a self-consistent file.
TEST(CrashRecoveryProcess, KilledAtVariedProgressPointsSoak) {
    struct Round {
        bool durable;
        uint64_t compact_threshold;
        uint64_t progress_threshold;
        const char* tag;
    };
    const Round rounds[] = {
        {true, 0, 800, "soak_early"},         // only a few F records journaled
        {false, 0, 3'000, "soak_firstchunk"}, // around the first flush
        {true, 1, 8'000, "soak_compact"},     // amid compaction cycles
        {false, 0, 25'000, "soak_deep"},      // several chunks in
    };
    for (const auto& r : rounds) {
        SCOPED_TRACE(r.tag);
        RunKilledProcessTest(r.durable, r.tag, r.compact_threshold, r.progress_threshold);
        if (::testing::Test::HasFatalFailure())
            return;
    }
}

// --- The same real-kill matrix, with ASYNC I/O ---------------------------------
//
// With async_io the chunk/journal writes happen on a worker thread, so a cold kill lands
// mid-write on a thread the recording loop never synchronized with. The recovery contract
// is unchanged: whatever became durable must recover as a CLEAN CONTIGUOUS PREFIX (frames
// 0..N-1, correct per-frame content, footer times sized to match). Only the durability LAG
// differs -- the not-yet-drained queue is lost, which is equivalent to crashing earlier.

TEST(CrashRecoveryProcess, KilledMidRecordingDurableAsync) {
    RunKilledProcessTest(/*durable=*/true, "durable_async", /*compact_threshold=*/0,
                         /*progress_threshold=*/20'000, /*async=*/true);
}

TEST(CrashRecoveryProcess, KilledMidRecordingFlushOnlyAsync) {
    RunKilledProcessTest(/*durable=*/false, "flushonly_async", /*compact_threshold=*/0,
                         /*progress_threshold=*/20'000, /*async=*/true);
}

TEST(CrashRecoveryProcess, KilledMidRecordingWhileCompactingAsync) {
    RunKilledProcessTest(/*durable=*/true, "compacting_async", /*compact_threshold=*/1,
                         /*progress_threshold=*/20'000, /*async=*/true);
}

TEST(CrashRecoveryProcess, KilledAtVariedProgressPointsSoakAsync) {
    struct Round {
        bool durable;
        uint64_t compact_threshold;
        uint64_t progress_threshold;
        const char* tag;
    };
    const Round rounds[] = {
        {true, 0, 800, "soak_async_early"},
        {false, 0, 3'000, "soak_async_firstchunk"},
        {true, 1, 8'000, "soak_async_compact"},
        {false, 0, 25'000, "soak_async_deep"},
    };
    for (const auto& r : rounds) {
        SCOPED_TRACE(r.tag);
        RunKilledProcessTest(r.durable, r.tag, r.compact_threshold, r.progress_threshold, /*async=*/true);
        if (::testing::Test::HasFatalFailure())
            return;
    }
}

#endif // _WIN32

// A record whose length field claims far more than the journal holds must be
// rejected up front (bounded by the file's real size -- no giant transient
// allocation), recovering everything before it.
TEST(CrashRecoverySweep, HostileRecordLengthIsBoundedByFileSize) {
    const Recording rec = CaptureRecording("hostilelen", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("hostilelen", rec, /*committed=*/2, /*pending=*/0);

    // Append a record header claiming a 400MB payload the 1-KB journal cannot hold.
    {
        std::ofstream j(VTX::RecoveryJournal::PathFor(path), std::ios::binary | std::ios::app);
        const char type = 'F';
        const uint32_t huge_len = 400u * 1024u * 1024u;
        j.write(&type, 1);
        j.write(reinterpret_cast<const char*>(&huge_len), sizeof(huge_len));
        const char few[16] = {};
        j.write(few, sizeof(few));
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 2); // hostile tail rejected, committed chunks intact

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 2);
}

// A journal from an incompatible version (here: a patched version field) must be
// refused outright -- never applied -- leaving the main file untouched.
TEST(CrashRecoveryE2E, JournalVersionMismatchIsRefused) {
    const Recording rec = CaptureRecording("verjournal", 2, /*per_chunk=*/1, Fmt::Flat);
    const std::string path = FabricateCrash("verjournal", rec, /*committed=*/2, /*pending=*/0);

    // Patch the journal's u32 version field (bytes 4..7 after "VTXR") to 99.
    {
        std::fstream j(VTX::RecoveryJournal::PathFor(path), std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(j.is_open());
        const uint32_t bogus_version = 99;
        j.seekp(4);
        j.write(reinterpret_cast<const char*>(&bogus_version), sizeof(bogus_version));
    }

    const auto bytes_before = VtxTest::ReadAllBytes(path);
    const auto rr = VTX::RepairReplayFile(path);
    EXPECT_FALSE(rr.ok()); // incompatible journal -> refused
    EXPECT_FALSE(rr.repaired);
    EXPECT_EQ(VtxTest::ReadAllBytes(path), bytes_before); // main file untouched
}

// The documented normalization workflow for salvaged files: a recovered file whose
// tail is many one-frame chunks (slower to read) can be transcoded into a first-class
// file with proper chunking using only the public API -- open it, drain the frames
// with their footer times, and re-record. created_utc (int64) round-trips exactly;
// game_time re-enters through the float-seconds register, whose truncating
// ticks->float->ticks conversion can lose 1 tick (100 ns) per frame -- the documented
// precision caveat of the transcode path.
TEST(CrashRecoveryE2E, RecoveredFileTranscodesToCleanChunks) {
    // A crash with EVERYTHING in flight -> repair yields 50 one-frame chunks.
    const std::string salvaged = VtxTest::OutputPath("transcode_salvaged.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(salvaged, /*chunk_max_frames=*/100);
        constexpr int64_t kBaseUtc = 5'000'000'000'000;
        for (int i = 0; i < 50; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            t.created_utc_time = kBaseUtc + (i + 1) * 166'667;
            ASSERT_TRUE(w->TryRecordFrame(frame, t).written);
        }
        // dropped without Stop(): nothing committed, 50 pending F records
    }
    const auto rr = VTX::RepairReplayFile(salvaged);
    ASSERT_TRUE(rr.ok()) << rr.error;
    ASSERT_EQ(rr.recovered_frames, 50);

    auto salvage = VTX::OpenReplayFile(salvaged);
    ASSERT_TRUE(salvage) << salvage.error;
    salvage->WaitUntilReady();
    ASSERT_EQ(salvage->GetSeekTable().size(), 50u); // one-frame chunks, as repaired

    // Transcode: drain into a fresh writer with normal chunking.
    const std::string clean = VtxTest::OutputPath("transcode_clean.vtx");
    {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = clean;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "TranscodedSalvage";
        cfg.chunk_max_frames = 100;
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        ASSERT_NE(writer, nullptr);

        const VTX::FileFooter footer = salvage->GetFooter();
        for (int i = 0; i < 50; ++i) {
            VTX::Frame frame;
            ASSERT_TRUE(salvage->GetFrame(i, frame) || (salvage->GetFrameSync(i) && salvage->GetFrame(i, frame)));
            VTX::GameTime::GameTimeRegister t;
            t.game_time = static_cast<float>(static_cast<double>(footer.times.game_time[static_cast<size_t>(i)]) /
                                             VTX::GameTime::TICKS_PER_SECOND);
            t.created_utc_time = static_cast<int64_t>(footer.times.created_utc[static_cast<size_t>(i)]);
            const auto res = writer->TryRecordFrame(frame, t);
            ASSERT_TRUE(res.written) << "frame " << i << ": " << res.error.message;
        }
        writer->Stop();
    }

    auto normalized = VTX::OpenReplayFile(clean);
    ASSERT_TRUE(normalized) << normalized.error;
    normalized->WaitUntilReady();
    EXPECT_EQ(normalized->GetTotalFrames(), 50);
    EXPECT_EQ(normalized->GetSeekTable().size(), 1u); // back to one proper 50-frame chunk

    const VTX::FileFooter sf = salvage->GetFooter();
    const VTX::FileFooter nf = normalized->GetFooter();
    EXPECT_EQ(nf.times.created_utc, sf.times.created_utc); // int64 register -> exact round-trip
    ASSERT_EQ(nf.times.game_time.size(), sf.times.game_time.size());
    for (size_t i = 0; i < sf.times.game_time.size(); ++i) {
        const int64_t drift = static_cast<int64_t>(nf.times.game_time[i]) - static_cast<int64_t>(sf.times.game_time[i]);
        // Float-seconds register: truncating ticks->float->ticks loses at most 1 tick.
        EXPECT_LE(std::abs(drift), 1) << "game_time drift beyond 1 tick at frame " << i;
    }
    for (int i : {0, 25, 49}) {
        const VTX::Frame* f = normalized->GetFrameSync(i);
        ASSERT_NE(f, nullptr) << "frame " << i;
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i);
    }
}

// Full lifecycle twice over the same path: crash -> repair -> re-record -> crash ->
// repair. The second recovery must reflect only the second session.
TEST(CrashRecoveryE2E, DoubleCrashLifecycleOnSamePath) {
    const std::string path = VtxTest::OutputPath("double_crash.vtx");

    // Session 1: 3 frames, crash, repair.
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2);
        for (int i = 0; i < 3; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            ASSERT_TRUE(w->TryRecordFrame(frame, t).written);
        }
    }
    {
        const auto rr = VTX::RepairReplayFile(path);
        ASSERT_TRUE(rr.ok()) << rr.error;
        EXPECT_EQ(rr.recovered_frames, 3);
    }

    // Session 2: same path, 5 frames with distinct scores, crash, repair.
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2);
        for (int i = 0; i < 5; ++i) {
            auto frame = BuildFrame(100 + i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            ASSERT_TRUE(w->TryRecordFrame(frame, t).written);
        }
    }
    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 5);
    EXPECT_FALSE(VTX::ReplayNeedsRecovery(path));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 5);
    for (int i = 0; i < 5; ++i) {
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 100 + i); // session-2 content only
    }
}

// The ".recovery" sidecar naming and repair I/O must work wherever the main file
// does -- including non-ASCII paths.
TEST(CrashRecoveryE2E, NonAsciiPathCrashRecovery) {
    // "reproducción_dañada.vtx" (UTF-8 bytes split so \x escapes don't swallow hex digits)
    const std::string path = VtxTest::OutputPath("reproducci\xC3\xB3"
                                                 "n_da\xC3\xB1"
                                                 "ada.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/2);
        for (int i = 0; i < 3; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            const auto res = w->TryRecordFrame(frame, t);
            ASSERT_TRUE(res.written) << res.error.message;
        }
        // dropped: 1 committed chunk + 1 in-flight frame
    }
    ASSERT_TRUE(VTX::ReplayNeedsRecovery(path));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 3);
    EXPECT_FALSE(VTX::ReplayNeedsRecovery(path));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 3);
}

// Scale check for the motivating scenario (a long recording): thousands of frames
// with multiple entities each, dozens of committed chunks, and a LARGE in-flight
// batch at crash time. Everything must come back -- committed and pending -- with
// exact timestamps, and the recovered file must pass whole-replay validation.
TEST(CrashRecoveryE2E, StressManyChunksAndLargePendingBatch) {
    constexpr int kFrames = 2030; // 40 committed chunks of 50 + 30 in-flight frames
    constexpr int kEntities = 5;
    constexpr int64_t kBaseUtc = 2'000'000'000'000;
    constexpr int64_t kStepUtc = 166'667;

    const std::string path = VtxTest::OutputPath("stress_crash.vtx");
    {
        // Flush-only durability keeps the test fast; the journal/recovery logic path
        // is identical to fsync mode.
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/50, /*durable=*/false);
        for (int i = 0; i < kFrames; ++i) {
            VTX::Frame frame;
            auto& bucket = frame.CreateBucket("entity");
            for (int k = 0; k < kEntities; ++k) {
                VTX::PropertyContainer pc;
                pc.entity_type_id = 0;
                pc.string_properties = {"p" + std::to_string(k), "Alpha"};
                pc.int32_properties = {1, i * 10 + k, 0};
                pc.float_properties = {100.0f, 50.0f};
                bucket.unique_ids.push_back("p" + std::to_string(k));
                bucket.entities.push_back(std::move(pc));
            }
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            t.created_utc_time = kBaseUtc + (i + 1) * kStepUtc;
            const auto res = w->TryRecordFrame(frame, t);
            ASSERT_TRUE(res.written) << "frame " << i << ": " << res.error.message;
        }
        // dropped without Stop()
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, kFrames);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), kFrames);

    // Spot frames across the file: first, mid-chunk, last committed, deep in the
    // recovered pending batch -- with every entity intact.
    for (int i : {0, 999, 1999, 2029}) {
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr) << "frame " << i;
        ASSERT_EQ(f->GetBuckets()[0].entities.size(), static_cast<size_t>(kEntities)) << "frame " << i;
        for (int k = 0; k < kEntities; ++k)
            EXPECT_EQ(f->GetBuckets()[0].entities[static_cast<size_t>(k)].int32_properties[1], i * 10 + k)
                << "frame " << i << " entity " << k;
    }

    // Exact timestamps for every frame, committed (T records) and pending (F records).
    const VTX::FileFooter footer = ctx->GetFooter();
    ASSERT_EQ(footer.times.created_utc.size(), static_cast<size_t>(kFrames));
    ASSERT_EQ(footer.times.game_time.size(), static_cast<size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i)
        ASSERT_EQ(static_cast<int64_t>(footer.times.created_utc[static_cast<size_t>(i)]), kBaseUtc + (i + 1) * kStepUtc)
            << "created_utc[" << i << "]";
    for (int i = 1; i < kFrames; ++i)
        ASSERT_LT(footer.times.game_time[static_cast<size_t>(i) - 1], footer.times.game_time[static_cast<size_t>(i)])
            << "game_time monotonicity at " << i;

    const VTX::ValidationReport report = VTX::ValidateReplayFile(path);
    EXPECT_FALSE(report.HasErrors()) << report.ToString();
}

// Frames REJECTED mid-recording (here: a stale created_utc the timer refuses) must
// not desync the journal's frame indexing -- a realistic occurrence over a long
// session. The accepted frames recover completely, with their exact times.
TEST(CrashRecoveryE2E, RejectedFramesDoNotDesyncJournal) {
    constexpr int64_t kBaseUtc = 3'000'000'000'000;
    const int64_t utcs[5] = {kBaseUtc + 1000, kBaseUtc + 2000, kBaseUtc + 3000, kBaseUtc + 4000, kBaseUtc + 5000};

    const std::string path = VtxTest::OutputPath("rejected_crash.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/3);
        int accepted = 0;
        for (int step = 0; step < 6; ++step) {
            const bool poison = (step == 3); // 4th record attempt reuses the last UTC
            auto frame = BuildFrame(accepted);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(accepted) / 60.0f;
            t.created_utc_time = poison ? utcs[accepted - 1] : utcs[accepted];
            const auto res = w->TryRecordFrame(frame, t);
            if (poison) {
                ASSERT_FALSE(res.written); // duplicate UTC -> timer rejects, writer rolls back
            } else {
                ASSERT_TRUE(res.written) << res.error.message;
                ++accepted;
            }
        }
        ASSERT_EQ(accepted, 5);
        // dropped without Stop(): 1 committed chunk (0-2) + 2 in-flight frames
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 5); // the rejected attempt left no trace

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 5);
    const VTX::FileFooter footer = ctx->GetFooter();
    ASSERT_EQ(footer.times.created_utc.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(static_cast<int64_t>(footer.times.created_utc[static_cast<size_t>(i)]), utcs[i]) << "utc " << i;
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], i);
    }
}

namespace {

    // Stamps every Player's Health with a marker so a recovered frame can prove it
    // was journaled AFTER post-processing (i.e., the journal holds what would have
    // been written to disk, not the raw input).
    class HealthStamper : public VTX::IFramePostProcessor {
    public:
        void Init(const VTX::FramePostProcessorInitContext& ctx) override {
            health_key_ = ctx.frame_accessor->Get<float>("Player", "Health");
        }
        void Process(VTX::FrameMutationView& view, const VTX::FramePostProcessContext&) override {
            if (!health_key_.IsValid())
                return;
            auto bucket = view.GetBucket("entity");
            for (auto entity : bucket)
                entity.Set(health_key_, 777.0f);
        }
        void Clear() override {}

    private:
        VTX::PropertyKey<float> health_key_ {-1};
    };

} // namespace

// The journal must capture the frame AS IT WOULD HIT DISK -- i.e., after the writer's
// post-processor ran. A pending frame that only ever existed as an F record must come
// back with the post-processed values, identical to its committed siblings.
TEST(CrashRecoveryE2E, PostProcessedFramesAreJournaledPostMutation) {
    const std::string path = VtxTest::OutputPath("postproc_crash.vtx");
    {
        auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, /*chunk_max_frames=*/3);
        w->SetPostProcessor(std::make_shared<HealthStamper>());
        for (int i = 0; i < 4; ++i) {
            auto frame = BuildFrame(i); // input Health = 100.0f
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            const auto res = w->TryRecordFrame(frame, t);
            ASSERT_TRUE(res.written) << res.error.message;
        }
        // dropped without Stop(): chunk (0-2) committed + frame 3 in flight
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    for (int i : {0, 3}) { // a committed frame and the F-record-only pending frame
        const VTX::Frame* f = ctx->GetFrameSync(i);
        ASSERT_NE(f, nullptr) << "frame " << i;
        EXPECT_FLOAT_EQ(f->GetBuckets()[0].entities[0].float_properties[0], 777.0f)
            << "frame " << i << " missing the post-processed value";
    }
}

#ifdef _WIN32
// Repairing a recording that is STILL BEING WRITTEN must be refused without touching
// the file (the writer holds a deny-write handle, so repair's truncate fails), and the
// live writer must be able to finish cleanly afterwards.
TEST(CrashRecoveryE2E, LiveRecordingRepairIsRefused) {
    const std::string path = VtxTest::OutputPath("e2e_live.vtx");
    auto w = MakeRawWriter<VTX::FlatBuffersVtxPolicy>(path, 3);
    RecordEightFrames(*w); // 2 chunks committed, journal present, writer still open

    ASSERT_TRUE(VTX::ReplayNeedsRecovery(path)); // sidecar exists while recording
    const auto rr = VTX::RepairReplayFile(path);
    EXPECT_FALSE(rr.ok()); // refused: file is held by the live writer
    EXPECT_FALSE(rr.repaired);

    w->Stop(); // the recording finishes untouched
    w.reset();
    ASSERT_FALSE(VTX::ReplayNeedsRecovery(path));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 8);
}
#endif

// --- Protobuf format ----------------------------------------------------------

TEST(CrashRecovery, RecoversAllCommittedChunks_Protobuf) {
    const Recording rec = CaptureRecording("pb_all", 4, /*per_chunk=*/1, Fmt::Proto);
    const std::string path = FabricateCrash("pb_all", rec, /*committed=*/4, /*pending=*/0);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_chunks, 4);
    EXPECT_EQ(rr.recovered_frames, 4);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 4);
    const VTX::Frame* f = ctx->GetFrameSync(3);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 3);
}

TEST(CrashRecovery, RecoversPendingFrames_Protobuf) {
    const Recording rec = CaptureRecording("pb_pending", 4, /*per_chunk=*/1, Fmt::Proto);
    const std::string path = FabricateCrash("pb_pending", rec, /*committed=*/2, /*pending=*/2);

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_frames, 4);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    ctx->WaitUntilReady();
    EXPECT_EQ(ctx->GetTotalFrames(), 4);
    const VTX::Frame* f = ctx->GetFrameSync(3);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 3);
}
