/**
 * @file durable_file.h
 * @brief Minimal binary file wrapper that can flush all the way to physical disk.
 *
 * @details std::ofstream cannot portably fsync (there is no standard way to reach
 * the underlying descriptor/handle), which is required for crash/power-loss
 * durability. DurableFile wraps a C stdio FILE* and exposes Sync() = fflush +
 * fsync/_commit, plus the seek/tell primitives the sink needs. Sequential-write
 * oriented; opened read/write+truncate so a repair pass can reuse the handle.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <io.h> // _commit, _fileno
#else
#include <unistd.h> // fsync, fileno
#endif

namespace VTX {

    class DurableFile {
    public:
        DurableFile() = default;
        ~DurableFile() { Close(); }

        DurableFile(const DurableFile&) = delete;
        DurableFile& operator=(const DurableFile&) = delete;

        /// Open for binary read/write, truncating any existing file. Returns false on failure.
        bool Open(const std::string& path) {
            Close();
            fp_ = std::fopen(path.c_str(), "wb+");
            return fp_ != nullptr;
        }

        /// Open an EXISTING file for binary read/write WITHOUT truncating (used by repair).
        bool OpenExisting(const std::string& path) {
            Close();
            fp_ = std::fopen(path.c_str(), "rb+");
            return fp_ != nullptr;
        }

        bool IsOpen() const { return fp_ != nullptr; }

        void Write(const void* data, size_t size) {
            if (fp_ && size > 0) {
                std::fwrite(data, 1, size, fp_);
            }
        }

        void Write(const std::string& data) { Write(data.data(), data.size()); }

        /// Current write position (byte offset from the start of the file).
        uint64_t Tell() {
            if (!fp_)
                return 0;
#ifdef _WIN32
            return static_cast<uint64_t>(_ftelli64(fp_));
#else
            return static_cast<uint64_t>(ftello(fp_));
#endif
        }

        void Seek(uint64_t offset) {
            if (!fp_)
                return;
#ifdef _WIN32
            _fseeki64(fp_, static_cast<long long>(offset), SEEK_SET);
#else
            fseeko(fp_, static_cast<off_t>(offset), SEEK_SET);
#endif
        }

        void SeekEnd() {
            if (!fp_)
                return;
#ifdef _WIN32
            _fseeki64(fp_, 0, SEEK_END);
#else
            fseeko(fp_, 0, SEEK_END);
#endif
        }

        /// Push the user-space buffer to the OS (survives a process crash, not power loss).
        void Flush() {
            if (fp_)
                std::fflush(fp_);
        }

        /// Flush all the way to physical media (survives power loss).
        void Sync() {
            if (!fp_)
                return;
            std::fflush(fp_);
#ifdef _WIN32
            _commit(_fileno(fp_));
#else
            fsync(fileno(fp_));
#endif
        }

        void Close() {
            if (fp_) {
                std::fclose(fp_);
                fp_ = nullptr;
            }
        }

    private:
        std::FILE* fp_ = nullptr;
    };

} // namespace VTX
