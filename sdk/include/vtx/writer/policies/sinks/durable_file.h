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
#include <io.h>    // _commit, _fileno
#include <share.h> // _SH_DENYWR
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
        /// On Windows the handle denies other WRITERS (readers stay allowed), so a stray
        /// repair pass cannot truncate a recording that is still being written.
        bool Open(const std::string& path) {
            Close();
#ifdef _WIN32
            fp_ = _fsopen(path.c_str(), "wb+", _SH_DENYWR);
#else
            fp_ = std::fopen(path.c_str(), "wb+");
#endif
            good_ = (fp_ != nullptr);
            return fp_ != nullptr;
        }

        /// Open an EXISTING file for binary read/write WITHOUT truncating (used by repair).
        bool OpenExisting(const std::string& path) {
            Close();
            fp_ = std::fopen(path.c_str(), "rb+");
            good_ = (fp_ != nullptr);
            return fp_ != nullptr;
        }

        bool IsOpen() const { return fp_ != nullptr; }

        /// True while no write/seek/tell/sync has failed since the last Open. A caller
        /// about to make an irreversible decision on the strength of prior writes (e.g.
        /// deleting the recovery journal after writing a footer) must check this first.
        bool Good() const { return good_; }

        /// Returns false (and latches Good() false) on a short write -- e.g. a full disk.
        bool Write(const void* data, size_t size) {
            if (!fp_ || size == 0)
                return fp_ != nullptr;
            const size_t written = std::fwrite(data, 1, size, fp_);
            if (written != size)
                good_ = false;
            return written == size;
        }

        bool Write(const std::string& data) { return Write(data.data(), data.size()); }

        /// Current write position (byte offset from the start of the file), or 0 on error
        /// (which also latches Good() false, since a poisoned offset would corrupt the seek table).
        uint64_t Tell() {
            if (!fp_) {
                good_ = false;
                return 0;
            }
#ifdef _WIN32
            const long long pos = _ftelli64(fp_);
#else
            const off_t pos = ftello(fp_);
#endif
            if (pos < 0) {
                good_ = false;
                return 0;
            }
            return static_cast<uint64_t>(pos);
        }

        /// Returns false (and latches Good() false) on a failed seek -- a write issued
        /// after an unnoticed seek failure would land at the wrong offset.
        bool Seek(uint64_t offset) {
            if (!fp_) {
                good_ = false;
                return false;
            }
#ifdef _WIN32
            if (_fseeki64(fp_, static_cast<long long>(offset), SEEK_SET) != 0) {
#else
            if (fseeko(fp_, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
                good_ = false;
                return false;
            }
            return true;
        }

        bool SeekEnd() {
            if (!fp_) {
                good_ = false;
                return false;
            }
#ifdef _WIN32
            if (_fseeki64(fp_, 0, SEEK_END) != 0) {
#else
            if (fseeko(fp_, 0, SEEK_END) != 0) {
#endif
                good_ = false;
                return false;
            }
            return true;
        }

        /// Push the user-space buffer to the OS (survives a process crash, not power loss).
        bool Flush() {
            if (!fp_) {
                good_ = false;
                return false;
            }
            if (std::fflush(fp_) != 0) {
                good_ = false;
                return false;
            }
            return true;
        }

        /// Flush all the way to physical media (survives power loss).
        bool Sync() {
            if (!fp_) {
                good_ = false;
                return false;
            }
            if (std::fflush(fp_) != 0) {
                good_ = false;
                return false;
            }
#ifdef _WIN32
            if (_commit(_fileno(fp_)) != 0) {
                good_ = false;
                return false;
            }
#else
            if (fsync(fileno(fp_)) != 0) {
                good_ = false;
                return false;
            }
#endif
            return true;
        }

        void Close() {
            if (fp_) {
                std::fclose(fp_);
                fp_ = nullptr;
            }
        }

    private:
        std::FILE* fp_ = nullptr;
        bool good_ = true; ///< Latches false on the first failed write/seek/tell/sync.
    };

} // namespace VTX
