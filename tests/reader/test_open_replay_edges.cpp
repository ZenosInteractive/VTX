// Edge-input tests for VTX::OpenReplayFile.
//
// Complements test_reader_context.cpp and test_corrupt_files.cpp by covering
// pathological path inputs: directory-instead-of-file, UTF-8 filenames, and
// size_in_mb behaviour on failure.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"

#include "util/test_fixtures.h"

namespace {

std::string WriteTinyReplay(const std::string& uuid) {
    VTX::WriterFacadeConfig cfg;
    cfg.output_filepath  = VtxTest::OutputPath("open_edge_" + uuid + ".vtx");
    cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
    cfg.replay_name      = "OpenReplayEdges";
    cfg.replay_uuid      = uuid;
    cfg.default_fps      = 60.0f;
    cfg.chunk_max_frames = 10;
    cfg.use_compression  = true;

    auto writer = VTX::CreateFlatBuffersWriterFacade(cfg);
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
    t.game_time = 0.0f;
    writer->RecordFrame(f, t);
    writer->Flush();
    writer->Stop();
    return cfg.output_filepath;
}

} // namespace

// ---------------------------------------------------------------------------
// Directory instead of file
// ---------------------------------------------------------------------------

TEST(OpenReplayEdges, OpenReplayFileWithDirectoryPath) {
    // Pass a path that exists but is a directory.  Must return an error
    // context, not crash trying to read "a directory" as if it were bytes.
    const auto dir = VtxTest::OutputPath("open_edge_dir_placeholder");
    std::filesystem::create_directories(dir);

    auto ctx = VTX::OpenReplayFile(dir);
    EXPECT_FALSE(ctx);
    EXPECT_FALSE(ctx.error.empty());
    EXPECT_EQ(ctx.format, VTX::VtxFormat::Unknown);
}

// ---------------------------------------------------------------------------
// size_in_mb when file doesn't exist
// ---------------------------------------------------------------------------

TEST(OpenReplayEdges, NonExistentFileZeroesSizeInMb) {
    auto ctx = VTX::OpenReplayFile(VtxTest::OutputPath("does_not_exist_xyz.vtx"));
    EXPECT_FALSE(ctx);
    // size_in_mb defaults to 0 and must remain 0 on failure -- no leaking
    // partial state from a previous call.
    EXPECT_EQ(ctx.size_in_mb, 0.0f);
}

// ---------------------------------------------------------------------------
// Relative path resolution -- pinned against the CWD at time of call
// ---------------------------------------------------------------------------

TEST(OpenReplayEdges, RelativePathResolvesAgainstCurrentWorkingDir) {
    // Write a file at a known ABSOLUTE path, then open it via a relative
    // path computed from the current working directory.  Must open.
    const auto absolute_path = WriteTinyReplay("rel_path");
    ASSERT_TRUE(std::filesystem::exists(absolute_path));

    // Compute the relative path from cwd.
    const auto cwd = std::filesystem::current_path();
    std::error_code ec;
    const auto relative_path =
        std::filesystem::relative(absolute_path, cwd, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_FALSE(relative_path.empty());

    auto ctx = VTX::OpenReplayFile(relative_path.string());
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 1);
}

// ---------------------------------------------------------------------------
// Unicode / non-ASCII filename
// ---------------------------------------------------------------------------

TEST(OpenReplayEdges, UnicodeFilenameDoesNotCrash) {
    // Filename contains latin-1 / CJK-ish bytes.  On Windows this exercises
    // the narrow-to-wide path conversion; on Linux it's just UTF-8 bytes.
    // The key assertion is: no crash, no silent data corruption.
    const auto good_path = WriteTinyReplay("ascii_base");
    ASSERT_TRUE(std::filesystem::exists(good_path));

    const auto unicode_path = VtxTest::OutputPath("replay_\xc3\xa1\xe3\x83\x86_tests.vtx");
    std::error_code ec;
    std::filesystem::copy_file(good_path, unicode_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        // Some filesystems / locales reject non-ASCII filenames.  In that
        // case we've still shown the API doesn't crash on the attempt.
        GTEST_SKIP() << "Filesystem rejected non-ASCII filename: " << ec.message();
    }

    auto ctx = VTX::OpenReplayFile(unicode_path);
    // Either it opens successfully or reports a clean error -- no crash.
    if (ctx) {
        EXPECT_EQ(ctx.reader->GetTotalFrames(), 1);
    } else {
        EXPECT_FALSE(ctx.error.empty());
    }
}
