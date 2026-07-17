// Crash-recovery tests: a writer that dies before Stop() leaves a footerless .vtx
// plus a ".recovery" journal; RepairReplayFile() must reconstruct a valid, readable
// file from it, dropping any torn/corrupt tail.
//
// The crash state is reproduced faithfully without racing a real process death:
// write a valid file, read its seek table (the exact per-chunk index the sink
// journals, checksums included), truncate the footer off, and re-create the
// ".recovery" journal from those records. That is byte-for-byte what the sink
// leaves on disk after a mid-write crash (chunks + journal, no footer).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_replay_recovery.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/policies/sinks/recovery_journal.h"

#include "util/test_fixtures.h"

namespace {

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

    // Writes `chunks` single-frame chunks to a valid .vtx (footer + no journal).
    std::string WriteValidFile(const std::string& suffix, int chunks) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = VtxTest::OutputPath("crash_" + suffix + ".vtx");
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "CrashRecoveryTest";
        cfg.default_fps = 60.0f;
        cfg.use_compression = true;

        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        EXPECT_NE(writer, nullptr);
        for (int i = 0; i < chunks; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
            writer->Flush(); // one chunk per frame
        }
        writer->Stop();
        return cfg.output_filepath;
    }

    // Reproduce the on-disk crash state: [header + chunks] with NO footer, plus a
    // ".recovery" journal listing the chunks. `truncate_extra` shaves extra bytes
    // off the last chunk to simulate a torn final write.
    std::vector<VTX::ChunkIndexData> MakeCrashState(const std::string& path, int64_t truncate_extra = 0) {
        std::vector<VTX::ChunkIndexData> records;
        int64_t last_chunk_end = 0;
        {
            auto ctx = VTX::OpenReplayFile(path);
            EXPECT_TRUE(ctx) << ctx.error;
            for (const auto& e : ctx.reader->GetSeekTable()) {
                VTX::ChunkIndexData d;
                d.chunk_index = e.chunk_index;
                d.start_frame = e.start_frame;
                d.end_frame = e.end_frame;
                d.file_offset = static_cast<int64_t>(e.file_offset);
                d.chunk_size_bytes = e.chunk_size_bytes;
                d.checksum = e.checksum;
                records.push_back(d);
                last_chunk_end =
                    std::max<int64_t>(last_chunk_end, static_cast<int64_t>(e.file_offset) + e.chunk_size_bytes);
            }
        }
        // Truncate off the footer (and optionally part of the last chunk).
        std::filesystem::resize_file(path, static_cast<std::uintmax_t>(last_chunk_end - truncate_extra));
        // Re-create the recovery journal exactly as the sink would have left it.
        VTX::RecoveryJournal journal;
        EXPECT_TRUE(journal.Open(VTX::RecoveryJournal::PathFor(path), "VTXF", true));
        for (const auto& d : records)
            journal.AppendChunk(d);
        journal.Close();
        return records;
    }

} // namespace

// A crash after N committed chunks (footerless file + journal) recovers all N.
TEST(CrashRecovery, RecoversAllChunksAfterCrash) {
    const std::string path = WriteValidFile("all", 3);
    MakeCrashState(path);

    const std::string journal_path = VTX::RecoveryJournal::PathFor(path);
    ASSERT_TRUE(std::filesystem::exists(journal_path));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.repaired);
    EXPECT_EQ(rr.recovered_chunks, 3);
    EXPECT_EQ(rr.recovered_frames, 3);
    EXPECT_FALSE(std::filesystem::exists(journal_path)); // deleted after repair

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 3);
    const VTX::Frame* f = ctx.reader->GetFrameSync(2);
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->GetBuckets().size(), 1u);
    ASSERT_EQ(f->GetBuckets()[0].entities.size(), 1u);
    EXPECT_EQ(f->GetBuckets()[0].entities[0].int32_properties[1], 2); // Score of frame 2
}

// A torn final chunk (bytes missing) is dropped; the earlier chunks are recovered.
TEST(CrashRecovery, DropsTornTailChunk) {
    const std::string path = WriteValidFile("torn", 3);
    const auto records = MakeCrashState(path, /*truncate_extra=*/0);
    // Shave the last chunk so its recorded extent runs past EOF.
    const int64_t mid_last = records.back().file_offset + 4; // just past the size prefix
    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(mid_last));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.repaired);
    EXPECT_EQ(rr.recovered_chunks, 2); // last chunk dropped
    EXPECT_EQ(rr.recovered_frames, 2);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 2);
}

// A chunk whose bytes were corrupted (checksum mismatch) stops recovery at it.
TEST(CrashRecovery, ChecksumDetectsCorruptChunk) {
    const std::string path = WriteValidFile("corrupt", 3);
    const auto records = MakeCrashState(path);
    // Flip a byte inside chunk 1's payload (after its 4-byte size prefix).
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(f.is_open());
        const std::streamoff pos = static_cast<std::streamoff>(records[1].file_offset + 5);
        char c = 0;
        f.seekg(pos);
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0xFF);
        f.seekp(pos);
        f.write(&c, 1);
        f.flush();
    }

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_EQ(rr.recovered_chunks, 1); // chunk 0 only; chunk 1 fails checksum -> stop

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 1);
}

// A crash between the footer fsync and the journal delete leaves a COMPLETE file
// plus a leftover .recovery. Repair must detect the valid footer and preserve it
// (incl. timing) rather than truncating and rewriting a timing-less one.
TEST(CrashRecovery, PreservesValidFooterWhenJournalLeftover) {
    const std::string path = WriteValidFile("leftover", 3);

    // Recreate the leftover journal WITHOUT truncating the (valid) footer.
    {
        std::vector<VTX::ChunkIndexData> records;
        {
            auto ctx = VTX::OpenReplayFile(path);
            ASSERT_TRUE(ctx) << ctx.error;
            for (const auto& e : ctx.reader->GetSeekTable()) {
                VTX::ChunkIndexData d;
                d.chunk_index = e.chunk_index;
                d.start_frame = e.start_frame;
                d.end_frame = e.end_frame;
                d.file_offset = static_cast<int64_t>(e.file_offset);
                d.chunk_size_bytes = e.chunk_size_bytes;
                d.checksum = e.checksum;
                records.push_back(d);
            }
        }
        VTX::RecoveryJournal journal;
        ASSERT_TRUE(journal.Open(VTX::RecoveryJournal::PathFor(path), "VTXF", true));
        for (const auto& d : records)
            journal.AppendChunk(d);
        journal.Close();
    }

    const auto size_before = std::filesystem::file_size(path);
    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.was_clean);  // detected an already-complete file
    EXPECT_FALSE(rr.repaired);  // did NOT rewrite the footer
    EXPECT_EQ(std::filesystem::file_size(path), size_before); // footer left untouched
    EXPECT_FALSE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 3);
}

// A journal whose recorded format magic disagrees with the main file (e.g. a stale
// sidecar left over a file replaced with the other format) must be refused, not
// applied (which would truncate a valid file).
TEST(CrashRecovery, RefusesMismatchedJournalFormat) {
    const std::string path = WriteValidFile("mismatch", 2);

    std::vector<VTX::ChunkIndexData> records;
    int64_t last_end = 0;
    {
        auto ctx = VTX::OpenReplayFile(path);
        ASSERT_TRUE(ctx) << ctx.error;
        for (const auto& e : ctx.reader->GetSeekTable()) {
            VTX::ChunkIndexData d;
            d.chunk_index = e.chunk_index;
            d.start_frame = e.start_frame;
            d.end_frame = e.end_frame;
            d.file_offset = static_cast<int64_t>(e.file_offset);
            d.chunk_size_bytes = e.chunk_size_bytes;
            d.checksum = e.checksum;
            records.push_back(d);
            last_end = std::max<int64_t>(last_end, static_cast<int64_t>(e.file_offset) + e.chunk_size_bytes);
        }
    }
    std::filesystem::resize_file(path, static_cast<std::uintmax_t>(last_end)); // footerless
    {
        VTX::RecoveryJournal journal;
        // Wrong format magic: the file is VTXF (FlatBuffers).
        ASSERT_TRUE(journal.Open(VTX::RecoveryJournal::PathFor(path), "VTXP", true));
        for (const auto& d : records)
            journal.AppendChunk(d);
        journal.Close();
    }

    const auto rr = VTX::RepairReplayFile(path);
    EXPECT_FALSE(rr.ok()); // refused: journal format does not match the file
    EXPECT_FALSE(rr.repaired);
}

// A cleanly-closed file has no journal, so repair is a no-op and the file is intact.
TEST(CrashRecovery, CleanFileNeedsNoRepair) {
    const std::string path = WriteValidFile("clean", 3);
    ASSERT_FALSE(std::filesystem::exists(VTX::RecoveryJournal::PathFor(path)));

    const auto rr = VTX::RepairReplayFile(path);
    ASSERT_TRUE(rr.ok()) << rr.error;
    EXPECT_TRUE(rr.was_clean);
    EXPECT_FALSE(rr.repaired);

    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 3);
}
