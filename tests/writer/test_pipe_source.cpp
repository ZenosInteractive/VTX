// Integration tests for VTX::PipeFrameDataSource.
//
// In-process loopback: the test thread acts as the producer (Windows named-pipe
// client / POSIX FIFO writer), VTX runs in *server* mode -- the same shape as
// the real use case (a game injector connecting to a VTX recorder).  The test
// thread retries the client connect until VTX has created the pipe, so the
// rendezvous never races.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/sources/pipe_frame_source.h"

#include "util/test_fixtures.h"

namespace {

    // ------------------------------------------------------------------
    // Trivial adapter.  Payload is a 4-byte LE int32 frame index; produces
    // a one-entity frame whose int32_property[0] is that index.  Returns
    // false on any unexpected payload size -- used by the adapter-rejects
    // test to verify that the source stops cleanly.
    // ------------------------------------------------------------------
    struct TestAdapter {
        bool ParseFrame(std::span<const std::byte> payload, VTX::Frame& out,
                        VTX::GameTime::GameTimeRegister& time) const {
            if (payload.size() != sizeof(int32_t))
                return false;
            int32_t idx = 0;
            std::memcpy(&idx, payload.data(), sizeof(idx));

            auto& bucket = out.CreateBucket("entity");
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.int32_properties.push_back(idx);
            bucket.unique_ids.push_back("e_" + std::to_string(idx));
            bucket.entities.push_back(std::move(pc));

            time.game_time = static_cast<float>(idx) / 60.0f;
            return true;
        }
    };

    // ------------------------------------------------------------------
    // Platform write helpers.  Loop on partial writes; return false on
    // error so the caller can bail.
    // ------------------------------------------------------------------
#ifdef _WIN32
    bool WriteAll(HANDLE h, const void* data, size_t n) {
        const auto* p = static_cast<const char*>(data);
        size_t total = 0;
        while (total < n) {
            DWORD got = 0;
            if (!::WriteFile(h, p + total, static_cast<DWORD>(n - total), &got, nullptr) || got == 0)
                return false;
            total += got;
        }
        return true;
    }
#else
    bool WriteAll(int fd, const void* data, size_t n) {
        const auto* p = static_cast<const char*>(data);
        size_t total = 0;
        while (total < n) {
            ssize_t w = ::write(fd, p + total, n - total);
            if (w <= 0)
                return false;
            total += static_cast<size_t>(w);
        }
        return true;
    }
#endif

    // Wire one frame as [uint32 LE size][payload].
    template <typename WriteFn>
    void WriteFrame(WriteFn write, const void* payload, uint32_t payload_size) {
        write(&payload_size, sizeof(payload_size));
        if (payload_size > 0)
            write(payload, payload_size);
    }

    // ------------------------------------------------------------------
    // Builds a Windows-namespace or POSIX-namespace pipe path unique to
    // this test process (PID-based) plus a caller-supplied tag.
    // ------------------------------------------------------------------
    std::string MakePipePath(const std::string& tag) {
#ifdef _WIN32
        const auto pid = ::GetCurrentProcessId();
        return "\\\\.\\pipe\\vtx_test_" + tag + "_" + std::to_string(pid);
#else
        return "/tmp/vtx_test_" + tag + "_" + std::to_string(::getpid()) + ".fifo";
#endif
    }

    // ------------------------------------------------------------------
    // Spawns a client thread that connects to the VTX-side pipe (server
    // mode) and emits the user-supplied frame stream via `emit_fn`.  The
    // client retries the connect for up to ~5 s, so the test is robust
    // even if the producer launches slightly before VTX's server is up.
    // ------------------------------------------------------------------
    template <typename EmitFn>
    std::thread SpawnClient(std::string pipe_path, EmitFn emit_fn) {
        return std::thread([pipe_path = std::move(pipe_path), emit_fn = std::move(emit_fn)] {
#ifdef _WIN32
            HANDLE h = INVALID_HANDLE_VALUE;
            for (int i = 0; i < 50; ++i) {
                h = ::CreateFileA(pipe_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (h == INVALID_HANDLE_VALUE)
                return;

            emit_fn([&](const void* d, size_t n) { WriteAll(h, d, n); });
            ::CloseHandle(h);
#else
            int fd = -1;
            for (int i = 0; i < 50; ++i) {
                fd = ::open(pipe_path.c_str(), O_WRONLY);
                if (fd >= 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (fd < 0)
                return;

            emit_fn([&](const void* d, size_t n) { WriteAll(fd, d, n); });
            ::close(fd);
#endif
        });
    }

    // ------------------------------------------------------------------
    // The standard pull-driven recording loop, factored to avoid copy/paste.
    // ------------------------------------------------------------------
    // Owns the writer for the duration of the drain and destroys it before
    // returning -- the file sink only flushes its ofstream in the destructor,
    // so the .vtx is not safe to re-open until the writer goes out of scope.
    size_t DrainSourceIntoWriter(VTX::PipeFrameDataSource<TestAdapter>& source,
                                 std::unique_ptr<VTX::IVtxWriterFacade> writer) {
        VTX::Frame frame;
        VTX::GameTime::GameTimeRegister time;
        size_t processed = 0;
        while (source.GetNextFrame(frame, time)) {
            writer->RecordFrame(frame, time);
            ++processed;
            frame = VTX::Frame {};
            time = VTX::GameTime::GameTimeRegister {};
        }
        writer->Flush();
        writer->Stop();
        writer.reset();
        return processed;
    }

    VTX::WriterFacadeConfig MakeWriterConfig(const std::string& out_path, const std::string& uuid) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = out_path;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "PipeSourceTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 100;
        cfg.use_compression = true;
        return cfg;
    }

} // anonymous namespace

// =========================================================================
//  Tests
// =========================================================================

// Happy path: VTX runs in server mode; the client thread streams N frames
// + a zero-size sentinel; VTX records each frame and finalises the .vtx
// when it sees the sentinel.  Re-opening the file with the reader proves
// every frame landed on disk.
TEST(PipeSource, ServerMode_ReceivesAndFinalisesVtx) {
    constexpr int kFrames = 50;
    const std::string pipe_path = MakePipePath("server_ok");
    const std::string out_path = VtxTest::OutputPath("pipe_server_ok.vtx");

    auto client = SpawnClient(pipe_path, [&](auto write) {
        for (int i = 0; i < kFrames; ++i) {
            int32_t idx = i;
            WriteFrame(write, &idx, sizeof(idx));
        }
        uint32_t sentinel = 0;
        write(&sentinel, sizeof(sentinel));
    });

    VTX::PipeFrameDataSource<TestAdapter>::Config src_cfg;
    src_cfg.pipe_path = pipe_path;
    src_cfg.as_server = true;
    VTX::PipeFrameDataSource<TestAdapter> source(src_cfg);
    ASSERT_TRUE(source.Initialize());

    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeWriterConfig(out_path, "pipe-server-ok"));
    ASSERT_TRUE(writer);
    const size_t processed = DrainSourceIntoWriter(source, std::move(writer));

    client.join();
    EXPECT_EQ(processed, static_cast<size_t>(kFrames));

    auto ctx = VTX::OpenReplayFile(out_path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), kFrames);
}

// Sentinel-only stream produces a valid, empty .vtx -- proves the server
// mode does not require any data frames before a clean shutdown.
TEST(PipeSource, ServerMode_SentinelOnlyProducesEmptyValidVtx) {
    const std::string pipe_path = MakePipePath("server_empty");
    const std::string out_path = VtxTest::OutputPath("pipe_server_empty.vtx");

    auto client = SpawnClient(pipe_path, [&](auto write) {
        uint32_t sentinel = 0;
        write(&sentinel, sizeof(sentinel));
    });

    VTX::PipeFrameDataSource<TestAdapter>::Config src_cfg;
    src_cfg.pipe_path = pipe_path;
    src_cfg.as_server = true;
    VTX::PipeFrameDataSource<TestAdapter> source(src_cfg);
    ASSERT_TRUE(source.Initialize());

    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeWriterConfig(out_path, "pipe-server-empty"));
    ASSERT_TRUE(writer);
    const size_t processed = DrainSourceIntoWriter(source, std::move(writer));

    client.join();
    EXPECT_EQ(processed, 0u);

    auto ctx = VTX::OpenReplayFile(out_path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 0);
}

// Adapter-rejected payload (wrong size) ends the stream at that frame:
// the source returns false from GetNextFrame and the writer finalises
// with exactly the frames recorded BEFORE the bad one.
TEST(PipeSource, ServerMode_AdapterFalseStopsStream) {
    constexpr int kValidFrames = 3;
    const std::string pipe_path = MakePipePath("server_reject");
    const std::string out_path = VtxTest::OutputPath("pipe_server_reject.vtx");

    auto client = SpawnClient(pipe_path, [&](auto write) {
        for (int i = 0; i < kValidFrames; ++i) {
            int32_t idx = i;
            WriteFrame(write, &idx, sizeof(idx));
        }
        // Bad payload: wrong size for our adapter -- triggers ParseFrame=>false.
        char bad[8] = {0};
        WriteFrame(write, bad, sizeof(bad));
        // Sentinel never reached (stream stops at the bad frame).
        uint32_t sentinel = 0;
        write(&sentinel, sizeof(sentinel));
    });

    VTX::PipeFrameDataSource<TestAdapter>::Config src_cfg;
    src_cfg.pipe_path = pipe_path;
    src_cfg.as_server = true;
    VTX::PipeFrameDataSource<TestAdapter> source(src_cfg);
    ASSERT_TRUE(source.Initialize());

    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeWriterConfig(out_path, "pipe-server-reject"));
    ASSERT_TRUE(writer);
    const size_t processed = DrainSourceIntoWriter(source, std::move(writer));

    client.join();
    EXPECT_EQ(processed, static_cast<size_t>(kValidFrames));

    auto ctx = VTX::OpenReplayFile(out_path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), kValidFrames);
}

// Streaming, unbounded total: GetExpectedTotalFrames() must return 0 to
// signal "unknown / streaming" to higher-level driver code (progress
// reporters etc.).
TEST(PipeSource, ReportsZeroExpectedTotalFramesForStreaming) {
    VTX::PipeFrameDataSource<TestAdapter>::Config cfg;
    // Empty pipe_path keeps construction safe -- we never call Initialize().
    VTX::PipeFrameDataSource<TestAdapter> source(cfg);
    EXPECT_EQ(source.GetExpectedTotalFrames(), 0u);
}
