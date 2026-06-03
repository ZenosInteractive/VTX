#pragma once

#include "vtx/writer/sources/detail/shared_memory_transport.h"
#include "vtx/writer/sources/detail/shm_ring.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace VTX {


    struct SpscRingTransport {
        struct Config {
            /// Number of slots in the ring.  Must be a power of two.
            uint32_t capacity = 64;
            /// Bytes per slot, including the 16-byte slot header.
            /// Maximum payload size is `slot_size - 16`.
            uint32_t slot_size = 16 * 1024;
        };

        Config config_;

        SpscRingTransport() = default;
        explicit SpscRingTransport(Config c)
            : config_(c) {}

        bool IsValid() const noexcept {
            if (config_.capacity == 0)
                return false;
            if ((config_.capacity & (config_.capacity - 1)) != 0)
                return false;
            if (config_.slot_size < shm_detail::kSlotHeaderBytes)
                return false;
            return true;
        }

        std::size_t SegmentBytes() const noexcept {
            return shm_detail::SegmentBytes(config_.capacity, config_.slot_size);
        }

        uint32_t SlotPayloadCapacity() const noexcept { return config_.slot_size - shm_detail::kSlotHeaderBytes; }

        void InitSegment(void* base) noexcept { shm_detail::InitSegment(base, config_.capacity, config_.slot_size); }

        bool ValidateSegment(const void* base) const noexcept {
            return shm_detail::ValidateSegment(base, config_.capacity, config_.slot_size);
        }

        shm_detail::PopResult Pop(void* base, std::byte* out, uint32_t out_capacity, uint32_t& out_size,
                                  uint64_t& dropped) noexcept {
            return shm_detail::Pop(base, out, out_capacity, out_size, dropped);
        }
    };

    static_assert(ISharedMemoryTransport<SpscRingTransport>,
                  "SpscRingTransport must satisfy ISharedMemoryTransport (concept drift?)");

} // namespace VTX
