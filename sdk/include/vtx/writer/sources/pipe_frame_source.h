#pragma once

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <fcntl.h>
#    include <io.h>
#else
#    include <cerrno>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/core/vtx_data_source.h"

namespace VTX {

    template <typename A>
    concept IPipeFrameAdapter = requires(A& adapter, std::span<const std::byte> payload,
                                          VTX::Frame& frame, VTX::GameTime::GameTimeRegister& time) {
        { adapter.ParseFrame(payload, frame, time) } -> std::convertible_to<bool>;
    };

    template <typename Adapter>
    class PipeFrameDataSource : public IFrameDataSource {
    public:
        struct Config {
            /// Empty => read from stdin.  Otherwise a named pipe / FIFO path
            /// (e.g. "\\\\.\\pipe\\vtx" on Windows, "/tmp/vtx.fifo" on POSIX).
            std::string pipe_path;

            bool as_server = false;

            Adapter adapter;
        };

        explicit PipeFrameDataSource(Config config) : config_(std::move(config)) {}

        ~PipeFrameDataSource() override {
            if (owns_file_ && file_)
                std::fclose(file_);
#ifndef _WIN32
            if (created_fifo_)
                ::unlink(config_.pipe_path.c_str());
#endif
        }

        PipeFrameDataSource(const PipeFrameDataSource&)            = delete;
        PipeFrameDataSource& operator=(const PipeFrameDataSource&) = delete;

        bool Initialize() override {
            if (config_.pipe_path.empty()) {
                file_      = stdin;
                owns_file_ = false;
#ifdef _WIN32
                ::_setmode(::_fileno(stdin), _O_BINARY);
#endif
                return true;
            }

            if (config_.as_server)
                return OpenAsServer();

            file_      = std::fopen(config_.pipe_path.c_str(), "rb");
            owns_file_ = true;
            return file_ != nullptr;
        }

        bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
            if (!file_)
                return false;

            // Read the 4-byte size prefix.  EOF here means a clean end.
            uint32_t size = 0;
            if (!ReadExact(&size, sizeof(size),true))
                return false;
            if (size == 0)
                return false;

            buffer_.resize(size);
            if (!ReadExact(buffer_.data(), size,false))
                return false;

            const std::span<const std::byte> payload(reinterpret_cast<const std::byte*>(buffer_.data()),
                                                      buffer_.size());
            return config_.adapter.ParseFrame(payload, out_frame, out_time);
        }


        size_t GetExpectedTotalFrames() const override { return 0; }

    private:

        bool OpenAsServer() {
#ifdef _WIN32
            // Create the named pipe, then block until a producer connects.
            HANDLE h = ::CreateNamedPipeA(config_.pipe_path.c_str(), PIPE_ACCESS_INBOUND,
                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                          1,
                                          64 * 1024, 64 * 1024, // out / in buffer sizes
                                          0,
                                          nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return false;

            const BOOL connected =
                ::ConnectNamedPipe(h, nullptr) ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) {
                ::CloseHandle(h);
                return false;
            }

            const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
            if (fd == -1) {
                ::CloseHandle(h);
                return false;
            }
            file_ = ::_fdopen(fd, "rb");
            if (!file_) {
                ::_close(fd);
                return false;
            }
            owns_file_ = true;
            return true;
#else
            // Create the FIFO (tolerate a leftover one from a previous run).
            if (::mkfifo(config_.pipe_path.c_str(), 0666) != 0 && errno != EEXIST)
                return false;
            created_fifo_ = true;

            // fopen on a FIFO blocks until a producer opens the write end.
            file_      = std::fopen(config_.pipe_path.c_str(), "rb");
            owns_file_ = true;
            return file_ != nullptr;
#endif
        }

        bool ReadExact(void* dst, size_t n, bool allow_eof = false) {
            auto*  bytes = static_cast<char*>(dst);
            size_t got   = 0;
            while (got < n) {
                size_t r = std::fread(bytes + got, 1, n - got, file_);
                if (r == 0) {
                    return allow_eof && got == 0 ? false : false;
                }
                got += r;
            }
            return true;
        }

        Config config_;
        FILE* file_ = nullptr;
        bool owns_file_ = false;
        bool created_fifo_ = false;
        std::vector<std::byte> buffer_;
    };

} // namespace VTX
