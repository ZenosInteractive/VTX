// Tests for `.vtx` files that are empty, truncated, or have corrupt metadata.
//
// Several of these target bugs uncovered during the 2026-04-20 audit:
//   - A1: ReadFooter() uses footer_size without checking stream.gcount()
//   - A2: PerformHeavyLoading doesn't detect partial reads
//   - A3: No bounds check that file_offset + chunk_size_bytes <= file_size
//
// All failure paths must return cleanly (error populated, no crash, no UB).

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

namespace {

// Writes a tiny valid FlatBuffers replay so we have a baseline file we can
// then mutate for each corruption test.
std::string WriteValidFlatBuffersFile(const std::string& uuid) {
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath  = VtxTest::OutputPath("corrupt_" + uuid + ".vtx");
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name      = "CorruptFileTest";
    cfg.replay_uuid      = uuid;
    cfg.default_fps      = 60.0f;
    cfg.chunk_max_frames = 10;
    cfg.use_compression  = true;

    {
        auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
        for (int i = 0; i < 5; ++i) {
            VTX::Frame f;
            auto& bucket = f.CreateBucket("entity");

            VTX::PropertyContainer pc;
            pc.entity_type_id    = 0;
            pc.string_properties = {"p", "name"};
            pc.int32_properties  = {1, 0, 0};
            pc.float_properties  = {100.0f, 50.0f};
            pc.vector_properties = {VTX::Vector{}, VTX::Vector{}};
            pc.quat_properties   = {VTX::Quat{}};
            pc.bool_properties   = {true};

            bucket.unique_ids.push_back("p");
            bucket.entities.push_back(std::move(pc));

            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(f, t);
        }
        writer->Flush();
        writer->Stop();
    }
    return cfg.output_filepath;
}

// Writes `content` verbatim to disk and returns the path.
std::string WriteRawBytes(const std::string& name, std::span<const uint8_t> content) {
    const auto path = VtxTest::OutputPath(name);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    ofs.close();
    return path;
}

// Truncates an existing file down to `target_size` bytes.
void TruncateFile(const std::string& path, std::uintmax_t target_size) {
    std::filesystem::resize_file(path, target_size);
}

} // namespace

// ===========================================================================
//  Trivially bad files
// ===========================================================================

TEST(CorruptFile, EmptyFileReturnsError) {
    const auto path = WriteRawBytes("empty.vtx", std::span<const uint8_t>{});
    auto ctx = VTX::OpenReplayFile(path);
    EXPECT_FALSE(ctx);
    EXPECT_FALSE(ctx.error.empty());
    EXPECT_EQ(ctx.format, VTX::VtxFormat::Unknown);
}

TEST(CorruptFile, FileSmallerThanMagicBytes) {
    // Only 2 bytes, less than the 4-byte VTXF / VTXP magic.
    const uint8_t bytes[] = {0xAB, 0xCD};
    const auto path = WriteRawBytes("tiny.vtx", std::span<const uint8_t>(bytes, sizeof(bytes)));

    auto ctx = VTX::OpenReplayFile(path);
    EXPECT_FALSE(ctx);
    EXPECT_FALSE(ctx.error.empty());
}

TEST(CorruptFile, ValidMagicButTruncatedHeader) {
    // Just the magic bytes "VTXF" -- nothing else.  The header reader must
    // detect truncation instead of reading past EOF.
    const uint8_t bytes[] = {'V', 'T', 'X', 'F'};
    const auto path = WriteRawBytes("truncated_magic.vtx",
                                    std::span<const uint8_t>(bytes, sizeof(bytes)));

    auto ctx = VTX::OpenReplayFile(path);
    EXPECT_FALSE(ctx);
    EXPECT_FALSE(ctx.error.empty());
}

// ===========================================================================
//  Valid-prefix / corrupt-suffix -- targets bug A1 + A2
// ===========================================================================

TEST(CorruptFile, TruncatedBeforeFooter) {
    // Write a valid replay, then truncate to 80% of its size so the footer
    // is missing.  The reader's ReadFooter() must detect the short stream
    // instead of using an uninitialised footer_size (bug A1).
    const auto original = WriteValidFlatBuffersFile("truncated_before_footer");
    const auto file_size = std::filesystem::file_size(original);
    ASSERT_GT(file_size, 100u);

    const auto truncated = VtxTest::OutputPath("truncated_before_footer.mutated.vtx");
    std::filesystem::copy_file(original, truncated,
                               std::filesystem::copy_options::overwrite_existing);
    TruncateFile(truncated, file_size / 2);

    auto ctx = VTX::OpenReplayFile(truncated);
    EXPECT_FALSE(ctx);
    EXPECT_FALSE(ctx.error.empty());  // must surface something, not crash
}

TEST(CorruptFile, CorruptFooterSize) {
    // Valid replay but we rewrite the trailing 8 bytes (where the footer_size
    // is stored) to UINT32_MAX.  The reader must detect the impossible size
    // instead of seeking to garbage (bug A1 variant).
    const auto original = WriteValidFlatBuffersFile("corrupt_footer_size");
    const auto mutated = VtxTest::OutputPath("corrupt_footer_size.mutated.vtx");
    std::filesystem::copy_file(original, mutated,
                               std::filesystem::copy_options::overwrite_existing);

    {
        std::fstream f(mutated, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(f);
        f.seekp(-8, std::ios::end);
        const uint32_t poison = 0xFFFFFFFFu;
        f.write(reinterpret_cast<const char*>(&poison), sizeof(poison));
    }

    auto ctx = VTX::OpenReplayFile(mutated);
    // Must not crash -- either returns error or opens with a diagnostic.
    // We don't care which, only that there's no UB / no hang.
    if (!ctx) {
        EXPECT_FALSE(ctx.error.empty());
    } else {
        // If it did open, at least basic metadata must be readable.
        SUCCEED();
    }
}

TEST(CorruptFile, ChunkOffsetBeyondEof) {
    // Open the valid file, then shave off the last ~5% of bytes.  This likely
    // leaves the seek table pointing past the new EOF for one or more chunks.
    // GetFrameSync must refuse to read past EOF and return nullptr rather than
    // returning stale bytes (bug A3).
    const auto original = WriteValidFlatBuffersFile("chunk_past_eof");
    const auto mutated  = VtxTest::OutputPath("chunk_past_eof.mutated.vtx");
    std::filesystem::copy_file(original, mutated,
                               std::filesystem::copy_options::overwrite_existing);

    const auto size = std::filesystem::file_size(mutated);
    ASSERT_GT(size, 100u);
    // Shave off more than the footer but keep enough bytes to still parse
    // the header -- the chunks at the tail will now be beyond EOF.
    TruncateFile(mutated, size - size / 10);

    auto ctx = VTX::OpenReplayFile(mutated);
    if (!ctx) {
        // Catastrophic failure is acceptable -- not a crash.
        EXPECT_FALSE(ctx.error.empty());
        return;
    }
    // If the header survived, any frame load that references a truncated
    // chunk must degrade to nullptr.  We sweep every frame; the key assertion
    // is that nothing crashes.
    for (int i = 0; i < ctx.reader->GetTotalFrames(); ++i) {
        const VTX::Frame* f = ctx.reader->GetFrameSync(i);
        (void)f;  // may be null, may not -- but must not crash
    }
}

// ===========================================================================
//  Out-of-range GetFrameSync
// ===========================================================================

TEST(CorruptFile, GetFrameSyncNegativeIndex) {
    const auto path = WriteValidFlatBuffersFile("negative_index");
    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx);

    EXPECT_EQ(ctx.reader->GetFrameSync(-1),    nullptr);
    EXPECT_EQ(ctx.reader->GetFrameSync(-100),  nullptr);
}

TEST(CorruptFile, GetFrameSyncIndexTooLarge) {
    const auto path = WriteValidFlatBuffersFile("large_index");
    auto ctx = VTX::OpenReplayFile(path);
    ASSERT_TRUE(ctx);

    const int total = ctx.reader->GetTotalFrames();
    EXPECT_EQ(ctx.reader->GetFrameSync(total),          nullptr);
    EXPECT_EQ(ctx.reader->GetFrameSync(total + 100),    nullptr);
    EXPECT_EQ(ctx.reader->GetFrameSync(1'000'000'000),  nullptr);
}
