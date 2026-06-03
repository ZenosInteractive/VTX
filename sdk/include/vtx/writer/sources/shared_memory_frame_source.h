#pragma once

// SharedMemoryFrameDataSource -- streaming IFrameDataSource over a
// shared-memory segment.  Lowest-latency option for a game injector
// talking to a VTX recorder: zero syscalls per frame after setup, zero
// copies across the IPC boundary (the producer writes directly into
// the slot the consumer will read).
//
// Architecture:
//   * The class owns the platform code (Win32 `CreateFileMapping` /
//     `MapViewOfFile` or POSIX `shm_open` + `mmap`) and the polling
//     loop that drives `GetNextFrame`.
//   * The wire layout of the segment + the publish/subscribe protocol
//     is delegated to a `Transport` that satisfies
//     `ISharedMemoryTransport`.
//
// Default transport (`SpscRingTransport`) is the VTX-native SPSC ring
// with per-slot release/acquire seq (LMAX Disruptor pattern), drop-
// oldest under overflow, and a `closed` flag for clean shutdown.
// See `detail/shm_ring.h` for the layout + correctness arguments and
// `detail/spsc_ring_transport.h` for the wrapper struct.
//
// To plug in a different wire protocol (e.g. UGI's
// `StreamControlBlockV1` + seqlock, an Aeron-compatible logbuffer, or
// your own in-house layout), write a struct that satisfies
// `ISharedMemoryTransport` and instantiate
// `SharedMemoryFrameDataSource<MyAdapter, MyTransport>`.  The public
// API does not change.
//
// Modes:
//   * Server (`as_server = true`): VTX creates the segment.  Mirrors
//     the named-pipe server mode -- right shape for a recorder that
//     launches before the game.
//   * Client (`as_server = false`): VTX attaches to an existing segment
//     a producer must already have created.  For attaching to a game
//     that runs first.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/core/vtx_data_source.h"
#include "vtx/writer/sources/detail/shared_memory_transport.h"
#include "vtx/writer/sources/detail/shm_ring.h"
#include "vtx/writer/sources/detail/spsc_ring_transport.h"
#include "vtx/writer/sources/frame_payload_adapter.h"

namespace VTX {

    template <IFramePayloadAdapter Adapter, ISharedMemoryTransport Transport = SpscRingTransport>
    class SharedMemoryFrameDataSource : public IFrameDataSource {
    public:
        struct Config {
            /// Segment name.  Cross-platform:
            ///   * Windows: a session-local name like "vtx_replay"; we
            ///     prefix "Local\\" automatically.  Pass "Global\\foo"
            ///     to opt into the global namespace.
            ///   * POSIX: a leading-slash name like "/vtx_replay" (we
            ///     add the slash if you omit it).  Lives in /dev/shm.
            std::string name;

            /// If true, VTX creates the segment (and unlinks it on POSIX
            /// shutdown).  If false, VTX attaches to an existing segment
            /// a producer must already have created.
            bool as_server = false;

            /// In client mode, how long to retry attaching while the
            /// producer hasn't yet created the segment.
            std::chrono::milliseconds attach_retry_timeout {5000};

            /// Sleep interval between empty-segment polls.  Lower latency,
            /// more idle CPU at higher polling rate.  200us is a good
            /// default for 60-144 fps recording workloads.
            std::chrono::microseconds poll_interval {200};

            Adapter adapter;

            /// Transport instance.  Default-constructed gives the v1
            /// SPSC ring with 64 slots of 16 KiB.  Override with e.g.
            /// `SpscRingTransport({.capacity = 16, .slot_size = 64 * 1024})`
            /// for a different ring geometry, or with a non-default
            /// `Transport` type for an entirely different wire protocol.
            Transport transport {};
        };

        explicit SharedMemoryFrameDataSource(Config config)
            : config_(std::move(config)) {}

        ~SharedMemoryFrameDataSource() override { Close(); }

        SharedMemoryFrameDataSource(const SharedMemoryFrameDataSource&) = delete;
        SharedMemoryFrameDataSource& operator=(const SharedMemoryFrameDataSource&) = delete;

        bool Initialize() override {
            if (!config_.transport.IsValid())
                return false;

            const size_t total_bytes = config_.transport.SegmentBytes();

            if (config_.as_server) {
                if (!CreateSegment(total_bytes))
                    return false;
                config_.transport.InitSegment(base_);
            } else {
                if (!AttachWithRetry(total_bytes))
                    return false;
                if (!config_.transport.ValidateSegment(base_)) {
                    Close();
                    return false;
                }
            }

            scratch_.resize(config_.transport.SlotPayloadCapacity());
            return true;
        }

        bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
            if (!base_)
                return false;

            for (;;) {
                uint32_t size = 0;
                shm_detail::PopResult r = config_.transport.Pop(base_, scratch_.data(),
                                                                static_cast<uint32_t>(scratch_.size()), size, dropped_);

                switch (r) {
                case shm_detail::PopResult::Ok: {
                    std::span<const std::byte> payload(scratch_.data(), size);
                    if (!config_.adapter.ParseFrame(payload, out_frame, out_time))
                        return false;
                    return true;
                }
                case shm_detail::PopResult::TornRead:
                    continue;
                case shm_detail::PopResult::Empty:
                    std::this_thread::sleep_for(config_.poll_interval);
                    continue;
                case shm_detail::PopResult::Closed:
                    return false;
                }
            }
        }

        size_t GetExpectedTotalFrames() const override { return 0; }

        uint64_t dropped_frames() const noexcept { return dropped_; }

    private:
#ifdef _WIN32
        std::string FullName() const {
            if (config_.name.rfind("Global\\", 0) == 0 || config_.name.rfind("Local\\", 0) == 0)
                return config_.name;
            return "Local\\" + config_.name;
        }

        bool CreateSegment(size_t bytes) {
            const std::string full = FullName();
            const DWORD hi = static_cast<DWORD>(bytes >> 32);
            const DWORD lo = static_cast<DWORD>(bytes & 0xFFFFFFFFu);
            HANDLE h = ::CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, full.c_str());
            if (h == nullptr)
                return false;
            void* p = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
            if (p == nullptr) {
                ::CloseHandle(h);
                return false;
            }
            handle_ = h;
            base_ = p;
            size_ = bytes;
            owns_ = true;
            return true;
        }

        bool AttachWithRetry(size_t bytes) {
            const std::string full = FullName();
            const auto deadline = std::chrono::steady_clock::now() + config_.attach_retry_timeout;
            HANDLE h = nullptr;
            while (h == nullptr && std::chrono::steady_clock::now() < deadline) {
                h = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, full.c_str());
                if (h == nullptr)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (h == nullptr)
                return false;
            void* p = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
            if (p == nullptr) {
                ::CloseHandle(h);
                return false;
            }
            handle_ = h;
            base_ = p;
            size_ = bytes;
            owns_ = false;
            return true;
        }

        void Close() noexcept {
            if (base_ != nullptr) {
                ::UnmapViewOfFile(base_);
                base_ = nullptr;
            }
            if (handle_ != nullptr) {
                ::CloseHandle(handle_);
                handle_ = nullptr;
            }
            size_ = 0;
        }
#else
        std::string FullName() const {
            return config_.name.empty() || config_.name.front() == '/' ? config_.name : "/" + config_.name;
        }

        bool CreateSegment(size_t bytes) {
            const std::string full = FullName();
            ::shm_unlink(full.c_str());
            int fd = ::shm_open(full.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd == -1)
                return false;
            if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
                ::close(fd);
                ::shm_unlink(full.c_str());
                return false;
            }
            void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                ::shm_unlink(full.c_str());
                return false;
            }
            fd_ = fd;
            base_ = p;
            size_ = bytes;
            owns_ = true;
            unlink_name_ = full;
            return true;
        }

        bool AttachWithRetry(size_t bytes) {
            const std::string full = FullName();
            const auto deadline = std::chrono::steady_clock::now() + config_.attach_retry_timeout;
            int fd = -1;
            while (fd == -1 && std::chrono::steady_clock::now() < deadline) {
                fd = ::shm_open(full.c_str(), O_RDWR, 0600);
                if (fd == -1)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (fd == -1)
                return false;
            void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                return false;
            }
            fd_ = fd;
            base_ = p;
            size_ = bytes;
            owns_ = false;
            return true;
        }

        void Close() noexcept {
            if (base_ != nullptr) {
                ::munmap(base_, size_);
                base_ = nullptr;
            }
            if (fd_ != -1) {
                ::close(fd_);
                fd_ = -1;
            }
            if (owns_ && !unlink_name_.empty()) {
                ::shm_unlink(unlink_name_.c_str());
                unlink_name_.clear();
            }
            size_ = 0;
            owns_ = false;
        }
#endif

        // Common state ----------------------------------------------------
        Config config_;
        void* base_ = nullptr;
        size_t size_ = 0;
        bool owns_ = false;
        std::vector<std::byte> scratch_;
        uint64_t dropped_ = 0;

#ifdef _WIN32
        HANDLE handle_ = nullptr;
#else
        int fd_ = -1;
        std::string unlink_name_;
#endif
    };

} // namespace VTX
