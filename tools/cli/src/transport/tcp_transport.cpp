#include "transport/tcp_transport.h"

#include <iostream>

namespace VtxCli {

    TcpTransport::TcpTransport(uint16_t port, bool verbose, bool json_only)
        : verbose_(verbose)
        , json_only_(json_only) {
#ifdef _WIN32
        WSADATA wsa_data {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            std::cerr << "[tcp] WSAStartup failed.\n";
            return;
        }
        wsa_initialized_ = true;
#endif

        server_socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_socket_ == kInvalidSocket) {
            std::cerr << "[tcp] Failed to create socket.\n";
            return;
        }

        // Allow port reuse (avoids "address already in use" on quick restart)
        int opt = 1;
        setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(server_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "[tcp] Failed to bind on port " << port << ".\n";
            closesocket(server_socket_);
            server_socket_ = kInvalidSocket;
            return;
        }

        if (::listen(server_socket_, 1) != 0) {
            std::cerr << "[tcp] Failed to listen.\n";
            closesocket(server_socket_);
            server_socket_ = kInvalidSocket;
            return;
        }

        if (verbose_) {
            std::cerr << "[tcp] Listening on port " << port << " ...\n";
        }

        // Blocks until a client connects
        client_socket_ = ::accept(server_socket_, nullptr, nullptr);
        if (client_socket_ == kInvalidSocket) {
            std::cerr << "[tcp] Accept failed.\n";
            closesocket(server_socket_);
            server_socket_ = kInvalidSocket;
            return;
        }

        connected_ = true;

        if (verbose_) {
            std::cerr << "[tcp] Client connected.\n";
        }
    }

    TcpTransport::~TcpTransport() {
        if (client_socket_ != kInvalidSocket) {
            closesocket(client_socket_);
        }
        if (server_socket_ != kInvalidSocket) {
            closesocket(server_socket_);
        }

#ifdef _WIN32
        if (wsa_initialized_) {
            WSACleanup();
        }
#endif

        if (verbose_) {
            std::cerr << "[tcp] Sockets closed.\n";
        }
    }

    std::optional<std::string> TcpTransport::ReadLine() {
        while (true) {
            // Check buffer for a complete line
            auto pos = read_buffer_.find('\n');
            if (pos != std::string::npos) {
                std::string line = read_buffer_.substr(0, pos);
                read_buffer_.erase(0, pos + 1);
                // Strip \r if present (telnet / Windows clients send \r\n)
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line;
            }

            // Need more data from socket
            char chunk[4096];
            int received = ::recv(client_socket_, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                connected_ = false;
                return std::nullopt;
            }
            read_buffer_.append(chunk, static_cast<size_t>(received));
        }
    }

    void TcpTransport::WriteLine(const std::string& s) {
        if (!connected_)
            return;

        std::string data = s + "\n";
        const char* ptr = data.c_str();
        int remaining = static_cast<int>(data.size());

        // Loop to handle partial sends
        while (remaining > 0) {
            int sent = ::send(client_socket_, ptr, remaining, 0);
            if (sent <= 0) {
                connected_ = false;
                return;
            }
            ptr += sent;
            remaining -= sent;
        }
    }

    bool TcpTransport::IsOpen() const {
        return connected_;
    }

    bool TcpTransport::IsVerbose() const {
        return verbose_;
    }

    bool TcpTransport::IsJsonOnly() const {
        return json_only_;
    }

    void TcpTransport::WriteLog(const std::string& s) {
        // TCP: logs go to stderr (never to the client socket)
        std::cerr << s << "\n";
    }

} // namespace VtxCli
