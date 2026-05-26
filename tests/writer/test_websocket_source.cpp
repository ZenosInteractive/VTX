// Integration tests for VTX::WebSocketFrameDataSource.
//
// In-process loopback: spin up an ix::WebSocketServer on an OS-assigned port,
// push a controlled number of messages at the first client that connects,
// then close.  VTX runs as the WebSocket client and records the stream
// into a .vtx that the reader must be able to parse back.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/sources/websocket_frame_source.h"

#include "util/test_fixtures.h"

namespace {

    // ------------------------------------------------------------------
    // Each received WebSocket message becomes a one-entity frame, with
    // the message's byte count stored as the entity's int32 property so
    // a downstream assertion could in principle reconstruct what was
    // pushed.  Returning true unconditionally means the stream length is
    // dictated by the server (close / disconnect ends it).
    // ------------------------------------------------------------------
    struct TestWsAdapter {
        bool ParseFrame(std::span<const std::byte> message, VTX::Frame& out,
                        VTX::GameTime::GameTimeRegister& time) const {
            auto& bucket = out.CreateBucket("entity");
            VTX::PropertyContainer pc;
            pc.entity_type_id = 0;
            pc.int32_properties.push_back(static_cast<int32_t>(message.size()));
            bucket.unique_ids.push_back("ws_e");
            bucket.entities.push_back(std::move(pc));
            time.game_time = 0.0f;
            return true;
        }
    };

    // ix::WebSocketServer's getPort() returns the *constructor* port, not the
    // OS-assigned port after bind, so passing port=0 leaves us unable to learn
    // the bound port.  Work around it: bind a raw socket to port 0, read back
    // the OS-assigned port via getsockname, close, and hand that explicit port
    // to ix::WebSocketServer.  Small TOCTOU window but adequate for tests.
    int PickFreeLoopbackPort() {
#ifdef _WIN32
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
        int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        sockaddr_in bound {};
        socklen_t len = sizeof(bound);
        ::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
        const int port = ntohs(bound.sin_port);
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        return port;
    }

    VTX::WriterFacadeConfig MakeWriterConfig(const std::string& out_path, const std::string& uuid) {
        VTX::WriterFacadeConfig cfg;
        cfg.output_filepath = out_path;
        cfg.schema_json_path = VtxTest::FixturePath("test_schema.json");
        cfg.replay_name = "WebSocketSourceTest";
        cfg.replay_uuid = uuid;
        cfg.default_fps = 60.0f;
        cfg.chunk_max_frames = 100;
        cfg.use_compression = true;
        return cfg;
    }

    // Owns the writer for the duration of the drain and destroys it before
    // returning -- the file sink only flushes its ofstream in the destructor,
    // so the .vtx is not safe to re-open until the writer goes out of scope.
    size_t DrainSourceIntoWriter(VTX::WebSocketFrameDataSource<TestWsAdapter>& source,
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

} // anonymous namespace

// =========================================================================
//  Fixture -- centralises ix::initNetSystem() / uninitNetSystem().  IXWebSocket
//  expects this on Windows before any socket calls (Winsock startup); a no-op
//  on POSIX.  Refcounted on Windows, so calling it both here and inside
//  WebSocketClient is safe.
// =========================================================================

class WebSocketSourceTest : public ::testing::Test {
protected:
    void SetUp() override { ix::initNetSystem(); }
    void TearDown() override { ix::uninitNetSystem(); }
};

// =========================================================================
//  Tests
// =========================================================================

// Happy path: server pushes N text messages at the connecting client and
// closes; VTX records each as a frame and finalises a parseable .vtx.
TEST_F(WebSocketSourceTest, ReceivesAndFinalisesVtx) {
    constexpr int kFrames = 25;

    const int port = PickFreeLoopbackPort();
    ix::WebSocketServer server(port, "127.0.0.1");
    server.disablePerMessageDeflate();
    // Use the per-client message callback (the v12 path) instead of the
    // weak_ptr-based connection callback -- the latter sometimes races the
    // client's handshake so badly that VTX's WebSocketClient::Connect sees
    // finished=true before open=true and reports a failed Initialize().
    server.setOnClientMessageCallback(
        [&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                for (int i = 0; i < kFrames; ++i)
                    ws.sendText(std::string("{\"i\":") + std::to_string(i) + "}");
                // Give send buffers a moment to drain before closing.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ws.close();
            }
        });

    auto listen_res = server.listen();
    ASSERT_TRUE(listen_res.first) << listen_res.second;
    server.start();

    const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/";
    const std::string out_path = VtxTest::OutputPath("websocket_source_ok.vtx");

    VTX::WebSocketFrameDataSource<TestWsAdapter>::Config src_cfg;
    src_cfg.url = url;
    VTX::WebSocketFrameDataSource<TestWsAdapter> source(src_cfg);
    ASSERT_TRUE(source.Initialize());

    auto writer = VTX::CreateFlatBuffersWriterFacade(MakeWriterConfig(out_path, "ws-source-ok"));
    ASSERT_TRUE(writer);
    const size_t processed = DrainSourceIntoWriter(source, std::move(writer));

    server.stop();

    EXPECT_EQ(processed, static_cast<size_t>(kFrames));

    auto ctx = VTX::OpenReplayFile(out_path);
    ASSERT_TRUE(ctx) << ctx.error;
    EXPECT_EQ(ctx.reader->GetTotalFrames(), kFrames);
}

// Initialize() must return false when no server is listening on the URL.
TEST_F(WebSocketSourceTest, InitializeReturnsFalseOnRefusedConnection) {
    // Pick a port the OS just released -- nothing is listening on it.
    const int free_port = PickFreeLoopbackPort();

    VTX::WebSocketFrameDataSource<TestWsAdapter>::Config src_cfg;
    src_cfg.url = "ws://127.0.0.1:" + std::to_string(free_port) + "/";
    VTX::WebSocketFrameDataSource<TestWsAdapter> source(src_cfg);

    EXPECT_FALSE(source.Initialize());
}

// Streaming -- the total frame count is unknown until the server closes
// the connection.  The driver uses 0 to switch from "X%" to "X processed".
TEST_F(WebSocketSourceTest, ReportsZeroExpectedTotalFrames) {
    VTX::WebSocketFrameDataSource<TestWsAdapter>::Config cfg;
    // No Initialize() -- just read the streaming-total contract.
    VTX::WebSocketFrameDataSource<TestWsAdapter> source(cfg);
    EXPECT_EQ(source.GetExpectedTotalFrames(), 0u);
}
