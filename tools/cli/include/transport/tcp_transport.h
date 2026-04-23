#pragma once

#include "core/cli_concepts.h"

#include <cstdint>
#include <optional>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
inline int closesocket(int fd) {
    return ::close(fd);
}
#endif

namespace VtxCli {

    class TcpTransport {
    public:
        explicit TcpTransport(uint16_t port, bool verbose = false, bool json_only = false);
        ~TcpTransport();

        TcpTransport(const TcpTransport&) = delete;
        TcpTransport& operator=(const TcpTransport&) = delete;

        std::optional<std::string> ReadLine();
        void WriteLine(const std::string& s);
        void WriteLog(const std::string& s);
        bool IsOpen() const;
        bool IsVerbose() const;
        bool IsJsonOnly() const;

    private:
        SocketHandle server_socket_ = kInvalidSocket;
        SocketHandle client_socket_ = kInvalidSocket;
        std::string read_buffer_;
        bool verbose_ = false;
        bool json_only_ = false;
        bool connected_ = false;

#ifdef _WIN32
        bool wsa_initialized_ = false;
#endif
    };

    static_assert(Transport<TcpTransport>, "TcpTransport must satisfy Transport");

} // namespace VtxCli
