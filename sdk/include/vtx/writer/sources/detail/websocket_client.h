#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace VTX::ws_detail {

    class WebSocketClient {
    public:
        WebSocketClient();
        ~WebSocketClient();

        WebSocketClient(const WebSocketClient&) = delete;
        WebSocketClient& operator=(const WebSocketClient&) = delete;

        /// Connects to the given WebSocket URL and blocks until and eturns false on connect / handshake / TLS failure.
        bool Connect(const std::string& url);

        /// Blocks until one complete message arrives and copies its bytes
        bool ReadMessage(std::vector<std::byte>& out);

        /// Closes the connection.  Idempotent; also called by the destructor.
        void Close();
        bool IsConnected() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace VTX::ws_detail
