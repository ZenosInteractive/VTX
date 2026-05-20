#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using VtxSocketHandle = SOCKET;
inline constexpr VtxSocketHandle kVtxInvalidSocket = INVALID_SOCKET;
inline void vtx_close_socket(VtxSocketHandle s) {
    ::closesocket(s);
}
using vtx_socklen_t = int;
struct WsaScope {
    WsaScope() {
        WSADATA d {};
        if (::WSAStartup(MAKEWORD(2, 2), &d) != 0)
            throw std::runtime_error("VTX: WSAStartup failed");
    }
    ~WsaScope() { ::WSACleanup(); }
    WsaScope(const WsaScope&) = delete;
    WsaScope& operator=(const WsaScope&) = delete;
};
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using VtxSocketHandle = int;
inline constexpr VtxSocketHandle kVtxInvalidSocket = -1;
inline void vtx_close_socket(VtxSocketHandle s) {
    ::close(s);
}
using vtx_socklen_t = socklen_t;
#endif

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <zstd.h>

#include "vtx/common/vtx_concepts.h"
#include "vtx/common/vtx_types.h"

namespace VTX {

    template <IVtxWriterPolicy Policy>
    class ChunkedNetworkSink {
    public:
        using SerializerPolicy = Policy;
        using FrameType = typename SerializerPolicy::FrameType;
        using SchemaType = typename SerializerPolicy::SchemaType;
        using HeaderType = typename SerializerPolicy::HeaderType;

        struct Config {
            std::string host;
            uint16_t port = 0;
            HeaderType header_config;
            bool b_use_compression = true;
            int8_t compression_level = 10;
        };

        explicit ChunkedNetworkSink(Config config)
            : config_(std::move(config)) {
            const std::string port_str = std::to_string(config_.port);

            struct addrinfo hints {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;

            struct addrinfo* result = nullptr;
            if (::getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &result) != 0)
                throw std::runtime_error("VTX: getaddrinfo failed for " + config_.host + ":" + port_str);

            for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
                socket_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (socket_ == kVtxInvalidSocket)
                    continue;
                if (::connect(socket_, ai->ai_addr, static_cast<vtx_socklen_t>(ai->ai_addrlen)) == 0)
                    break;
                vtx_close_socket(socket_);
                socket_ = kVtxInvalidSocket;
            }
            ::freeaddrinfo(result);

            if (socket_ == kVtxInvalidSocket)
                throw std::runtime_error("VTX: Could not connect to " + config_.host + ":" + port_str);
        }

        ~ChunkedNetworkSink() {
            if (socket_ != kVtxInvalidSocket)
                vtx_close_socket(socket_);
        }

        ChunkedNetworkSink(const ChunkedNetworkSink&) = delete;
        ChunkedNetworkSink& operator=(const ChunkedNetworkSink&) = delete;

        void OnSessionStart(const SchemaType& schema) {
            SendAll(SerializerPolicy::GetMagicBytes());

            std::string header_payload = SerializerPolicy::SerializeHeader(config_.header_config, schema);
            header_payload = CompressIfBeneficial(std::move(header_payload));
            uint32_t final_size = static_cast<uint32_t>(header_payload.size());
            SendAll(reinterpret_cast<const char*>(&final_size), sizeof(final_size));
            SendAll(header_payload);
        }

        void SaveChunk(std::vector<std::unique_ptr<FrameType>>& frames, const std::vector<int64_t>& /*created_utc*/,
                       int32_t start_frame, int32_t total_frames) {
            if (frames.empty())
                return;

            std::string payload = SerializerPolicy::SerializeChunk(frames, chunkIndex_, config_.b_use_compression);
            payload = CompressIfBeneficial(std::move(payload));

            uint64_t current_offset = bytes_sent_;
            uint32_t final_size = static_cast<uint32_t>(payload.size());
            SendAll(reinterpret_cast<const char*>(&final_size), sizeof(final_size));
            SendAll(payload);

            ChunkIndexData entry;
            entry.chunk_index = chunkIndex_++;
            entry.file_offset = current_offset;
            entry.start_frame = start_frame;
            entry.end_frame = total_frames - 1;
            entry.chunk_size_bytes = final_size + static_cast<uint32_t>(sizeof(uint32_t));
            seek_table_.push_back(entry);
        }

        void Close(const SessionFooter& footerData) {
            if (socket_ == kVtxInvalidSocket)
                return;

            std::string footer_payload = SerializerPolicy::SerializeFooter(seek_table_, footerData);
            footer_payload = CompressIfBeneficial(std::move(footer_payload));
            SendAll(footer_payload);
            uint32_t final_size = static_cast<uint32_t>(footer_payload.size());
            SendAll(reinterpret_cast<const char*>(&final_size), sizeof(final_size));
            SendAll(SerializerPolicy::GetMagicBytes());

            vtx_close_socket(socket_);
            socket_ = kVtxInvalidSocket;
        }

    private:
        void SendAll(const char* data, size_t len) {
            size_t sent = 0;
            while (sent < len) {
                int n = ::send(socket_, data + sent, static_cast<int>(len - sent), 0);
                if (n <= 0)
                    throw std::runtime_error("VTX: Network send failed");
                sent += static_cast<size_t>(n);
            }
            bytes_sent_ += len;
        }

        void SendAll(const std::string& data) { SendAll(data.data(), data.size()); }

        std::string CompressIfBeneficial(std::string payload) {
            if (!config_.b_use_compression || payload.size() < 512)
                return payload;

            size_t max_size = ZSTD_compressBound(payload.size());
            std::string compressed(max_size, '\0');
            size_t compressed_size =
                ZSTD_compress(compressed.data(), max_size, payload.data(), payload.size(), config_.compression_level);

            if (ZSTD_isError(compressed_size) || compressed_size >= payload.size())
                return payload;

            compressed.resize(compressed_size);
            return compressed;
        }

#ifdef _WIN32
        WsaScope wsa_;
#endif
        VtxSocketHandle socket_ = kVtxInvalidSocket;
        Config config_;
        int32_t chunkIndex_ = 0;
        std::vector<ChunkIndexData> seek_table_;
        uint64_t bytes_sent_ = 0;
    };

} // namespace VTX
