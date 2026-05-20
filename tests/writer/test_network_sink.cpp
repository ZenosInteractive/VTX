// Integration tests for ChunkedNetworkSink.
//
// Each test spins up a minimal loopback TCP server on a free ephemeral port,
// writes a replay through the network writer facade, waits for the server
// thread to drain all bytes, then validates the received stream with the
// standard VTX reader.

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/policies/sinks/network_sink.h" // brings in socket portability layer

#include "util/test_fixtures.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"

// ---------------------------------------------------------------------------
// Platform-portable loopback server
// ---------------------------------------------------------------------------

namespace {

    /// Minimal TCP server that listens on 127.0.0.1:<ephemeral port>, accepts
    /// one connection, drains all incoming bytes, and stores them in `received`.
    ///
    /// Usage:
    ///   LoopbackServer srv;          // binds + listens, starts drain thread
    ///   // ... create writer to srv.port ...
    ///   // ... write frames, stop writer ...
    ///   srv.WaitForCompletion();     // joins drain thread
    ///   // srv.received has the full byte stream
    struct LoopbackServer {
#ifdef _WIN32
        WsaScope wsa_; // Windows: must init Winsock before any socket call
#endif
        uint16_t port = 0;
        std::vector<std::byte> received;

        explicit LoopbackServer() {
            server_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (server_sock_ == kVtxInvalidSocket)
                throw std::runtime_error("LoopbackServer: socket() failed");

            int reuse = 1;
            ::setsockopt(server_sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0; // let OS pick a free port

            if (::bind(server_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
                throw std::runtime_error("LoopbackServer: bind() failed");
            if (::listen(server_sock_, 1) != 0)
                throw std::runtime_error("LoopbackServer: listen() failed");

            // Retrieve the port the OS assigned.
            sockaddr_in bound {};
            vtx_socklen_t len = sizeof(bound);
            ::getsockname(server_sock_, reinterpret_cast<sockaddr*>(&bound), &len);
            port = ntohs(bound.sin_port);

            // Launch drain thread.  The promise signals that accept() is
            // imminent so the caller can safely connect before returning.
            std::promise<void> ready;
            auto ready_future = ready.get_future();

            thread_ = std::thread([this, r = std::move(ready)]() mutable {
                r.set_value(); // server is ready to accept

                VtxSocketHandle client = ::accept(server_sock_, nullptr, nullptr);
                if (client == kVtxInvalidSocket) {
                    vtx_close_socket(server_sock_);
                    server_sock_ = kVtxInvalidSocket;
                    return;
                }

                std::array<char, 16384> buf;
                int n;
                while ((n = ::recv(client, buf.data(), static_cast<int>(buf.size()), 0)) > 0) {
                    for (int i = 0; i < n; ++i)
                        received.push_back(static_cast<std::byte>(buf[i]));
                }

                vtx_close_socket(client);
                vtx_close_socket(server_sock_);
                server_sock_ = kVtxInvalidSocket;
            });

            ready_future.wait(); // don't return until accept() is ready
        }

        void WaitForCompletion() {
            if (thread_.joinable())
                thread_.join();
        }

        ~LoopbackServer() { WaitForCompletion(); }

    private:
        VtxSocketHandle server_sock_ = kVtxInvalidSocket;
        std::thread thread_;
    };

    // -----------------------------------------------------------------------
    // Shared helpers
    // -----------------------------------------------------------------------

    VTX::Frame BuildFrame(int frame_index) {
        VTX::Frame f;
        auto& bucket = f.CreateBucket("entity");

        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        pc.string_properties = {"player_0"};
        pc.int32_properties = {frame_index};
        pc.float_properties = {100.0f - float(frame_index)};

        bucket.unique_ids.push_back("player_0");
        bucket.entities.push_back(std::move(pc));
        return f;
    }

    VTX::NetworkWriterFacadeConfig MakeNetworkConfig(const std::string& host, uint16_t port, const std::string& uuid) {
        VTX::NetworkWriterFacadeConfig cfg;
        cfg.host = host;
        cfg.port = port;
        cfg.replay_name = "NetworkSinkTest";
        cfg.replay_uuid = uuid;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 50;
        cfg.use_compression = true;
        return cfg;
    }

    /// Writes received bytes to a temp file and opens it with the VTX reader.
    std::string WriteTempVtx(const std::vector<std::byte>& bytes, const std::string& name) {
        const std::string path = VtxTest::OutputPath(name);
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path;
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test fixture — initialises Winsock once for direct socket ops in tests.
// ---------------------------------------------------------------------------

#ifdef _WIN32
class NetworkSinkTest : public ::testing::Test {
protected:
    WsaScope wsa_;
};
#else
using NetworkSinkTest = ::testing::Test;
#endif

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(NetworkSinkTest, FlatBuffers_ReceivedBytesParseableByReader) {
    constexpr int kFrames = 30;

    LoopbackServer srv;
    {
        auto writer =
            VTX::CreateFlatBuffersNetworkWriterFacade(MakeNetworkConfig("127.0.0.1", srv.port, "uuid-net-fbs"));
        ASSERT_TRUE(writer);

        for (int i = 0; i < kFrames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }
    srv.WaitForCompletion();

    ASSERT_FALSE(srv.received.empty());
    const std::string tmp = WriteTempVtx(srv.received, "net_fbs.vtx");

    auto ctx = VTX::OpenReplayFile(tmp);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.format, VTX::VtxFormat::FlatBuffers);
    EXPECT_EQ(ctx.reader->GetTotalFrames(), kFrames);
}

TEST_F(NetworkSinkTest, Protobuf_ReceivedBytesParseableByReader) {
    constexpr int kFrames = 30;

    LoopbackServer srv;
    {
        auto writer =
            VTX::CreateProtobuffNetworkWriterFacade(MakeNetworkConfig("127.0.0.1", srv.port, "uuid-net-proto"));
        ASSERT_TRUE(writer);

        for (int i = 0; i < kFrames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }
    srv.WaitForCompletion();

    ASSERT_FALSE(srv.received.empty());
    const std::string tmp = WriteTempVtx(srv.received, "net_proto.vtx");

    auto ctx = VTX::OpenReplayFile(tmp);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.format, VTX::VtxFormat::Protobuf);
    EXPECT_EQ(ctx.reader->GetTotalFrames(), kFrames);
}

TEST_F(NetworkSinkTest, ThrowsOnConnectionRefused) {
    // Bind to port 0 to get a free port, then close without listen —
    // connecting to it should be refused.
    VtxSocketHandle probe = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(probe, kVtxInvalidSocket);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    sockaddr_in bound {};
    vtx_socklen_t len = sizeof(bound);
    ::getsockname(probe, reinterpret_cast<sockaddr*>(&bound), &len);
    const uint16_t free_port = ntohs(bound.sin_port);
    vtx_close_socket(probe); // nothing is listening on this port

    VTX::ChunkedNetworkSink<VTX::FlatBuffersVtxPolicy>::Config bad_cfg;
    bad_cfg.host = "127.0.0.1";
    bad_cfg.port = free_port;

    EXPECT_THROW((VTX::ChunkedNetworkSink<VTX::FlatBuffersVtxPolicy>(bad_cfg)), std::runtime_error);
}

TEST_F(NetworkSinkTest, ZeroFrames_ProducesValidStream) {
    LoopbackServer srv;
    {
        auto writer =
            VTX::CreateFlatBuffersNetworkWriterFacade(MakeNetworkConfig("127.0.0.1", srv.port, "uuid-net-zero"));
        ASSERT_TRUE(writer);
        writer->Stop(); // no frames recorded
    }
    srv.WaitForCompletion();

    ASSERT_FALSE(srv.received.empty());
    const std::string tmp = WriteTempVtx(srv.received, "net_zero.vtx");

    auto ctx = VTX::OpenReplayFile(tmp);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), 0);
}

TEST_F(NetworkSinkTest, MultipleChunks_SeekTableHasCorrectCount) {
    constexpr int kFrames = 10;
    constexpr int kChunkLimit = 1; // one frame per chunk → 10 chunks

    LoopbackServer srv;
    {
        auto cfg = MakeNetworkConfig("127.0.0.1", srv.port, "uuid-net-chunks");
        cfg.chunk_max_frames = kChunkLimit;

        auto writer = VTX::CreateFlatBuffersNetworkWriterFacade(cfg);
        ASSERT_TRUE(writer);

        for (int i = 0; i < kFrames; ++i) {
            auto frame = BuildFrame(i);
            VTX::GameTime::GameTimeRegister t;
            t.game_time = float(i) / 60.0f;
            writer->RecordFrame(frame, t);
        }
        writer->Stop();
    }
    srv.WaitForCompletion();

    const std::string tmp = WriteTempVtx(srv.received, "net_chunks.vtx");

    auto ctx = VTX::OpenReplayFile(tmp);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), kFrames);
    EXPECT_EQ(ctx.reader->GetSeekTable().size(), static_cast<size_t>(kFrames));
}
