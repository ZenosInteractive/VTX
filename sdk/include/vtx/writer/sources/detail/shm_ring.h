#pragma once

// SPSC ring buffer layout that lives inside a shared-memory segment.
//
// Two cursors (head, tail) coordinate a single producer and a single
// consumer across processes.  Indices are monotonic uint64_t and
// slot = index & (capacity - 1) where capacity is a power of two.
//
// Each slot carries its own publish sequence (`seq = index + 1` when
// written) -- the LMAX Disruptor pattern.  This gives empty detection
// without touching head, gap detection under drop-oldest, and torn-
// read protection (the consumer re-reads seq after copying the payload;
// if it changed, the slot got overwritten and we retry).
//
// Drop-oldest: the producer never blocks and never touches `tail` -- if
// the ring is full, the next slot is simply overwritten and the
// consumer notices via the seq jump.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace VTX::shm_detail {

    enum class PopResult {
        Ok,       //Frame copied, advance tail
        Empty,    //Empty ring
        Closed,   //Productor closed and ring cleaned
        TornRead, //Productor received in the middle of a mempcpy,
    };


    inline constexpr uint32_t kRingMagic = 0x53585456; // "VTXS" little-endian
    inline constexpr uint32_t kRingVersion = 1;
    inline constexpr uint32_t kSlotHeaderBytes = 16;


    struct alignas(64) RingHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t capacity;
        uint32_t slot_size;
        std::atomic<uint32_t> closed;
        uint32_t reserved32;
        uint64_t reserved64[5];
    };
    static_assert(sizeof(RingHeader) == 64, "RingHeader must occupy exactly one cache line of 64 bytes");

    struct alignas(64) ProducerCursor {
        std::atomic<uint64_t> head;
        char pad[64 - sizeof(std::atomic<uint64_t>)];
    };
    static_assert(sizeof(ProducerCursor) == 64, "ProducerCursor must occupy exactly one cache line of 64 bytes");

    struct alignas(64) ConsumerCursor {
        std::atomic<uint64_t> tail;
        char _pad[64 - sizeof(std::atomic<uint64_t>)];
    };
    static_assert(sizeof(ConsumerCursor) == 64, "ConsumerCursor must occupy exactly one cache line of 64 bytes");

    struct SlotHeader {
        std::atomic<uint64_t> seq;
        uint32_t size;
        uint32_t reserved;
    };
    static_assert(sizeof(SlotHeader) == kSlotHeaderBytes, "SlotHeader must occupy exactly kSlotHeaderBytes");

    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "VTX SharedMemoryFrameDataSource requires lock-free 64-bit atomics; "
                  "exotic platforms without HW support would break cross-process safety");

    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "VTX SharedMemoryFrameDataSource requires lock-free 32-bit atomics; "
                  "exotic platforms without HW support would break cross-process safety");


    //Ptr arithmetic helpers
    inline constexpr size_t SegmentBytes(uint32_t capacity, uint32_t slot_size) noexcept {
        size_t total_bytes = sizeof(RingHeader) + sizeof(ProducerCursor) + sizeof(ConsumerCursor) +
                             static_cast<size_t>(capacity) * slot_size;
        return total_bytes;
    }

    inline ProducerCursor* ProducerOf(void* base) noexcept {
        return reinterpret_cast<ProducerCursor*>(static_cast<std::byte*>(base) + sizeof(RingHeader));
    }

    inline ConsumerCursor* ConsumerOf(void* base) noexcept {
        return reinterpret_cast<ConsumerCursor*>(static_cast<std::byte*>(base) + sizeof(RingHeader) +
                                                 sizeof(ProducerCursor));
    }

    inline std::byte* RingBaseOf(void* base) noexcept {
        return static_cast<std::byte*>(base) + sizeof(RingHeader) + sizeof(ProducerCursor) + sizeof(ConsumerCursor);
    }

    inline SlotHeader* SlotAt(std::byte* ring_base, uint32_t slot_size, uint32_t mask, uint64_t index) noexcept {
        return reinterpret_cast<SlotHeader*>(ring_base + (index & mask) * slot_size);
    }

    inline std::byte* SlotPayload(SlotHeader* slot) noexcept {
        return reinterpret_cast<std::byte*>(slot) + sizeof(SlotHeader);
    }

    inline void InitSegment(void* base, uint32_t capacity, uint32_t slot_size) {
        if (!base) {
            return;
        }

        RingHeader* header = static_cast<RingHeader*>(base);
        if (header) {
            header->magic = kRingMagic;
            header->version = kRingVersion;
            header->slot_size = slot_size;
            header->capacity = capacity;
            header->reserved32 = 0;
            std::memset(header->reserved64, 0, sizeof(header->reserved64));
            header->closed.store(0, std::memory_order_relaxed);
            ProducerOf(base)->head.store(0, std::memory_order_relaxed);
            ConsumerOf(base)->tail.store(0, std::memory_order_relaxed);
        }

        std::byte* ring = RingBaseOf(base);
        const size_t ring_size = static_cast<size_t>(capacity) * slot_size;
        std::memset(ring, 0, ring_size);
    }

    inline bool ValidateSegment(const void* base, uint32_t expected_capacity, uint32_t expected_slot_size) noexcept {
        if (!base) {
            return false;
        }
        const RingHeader* header = static_cast<const RingHeader*>(base);
        if (header) {
            if (header->magic != kRingMagic)
                return false;
            if (header->version != kRingVersion)
                return false;
            if (header->capacity != expected_capacity)
                return false;
            if (header->slot_size != expected_slot_size)
                return false;
            if ((header->capacity & (header->capacity - 1)) != 0)
                return false;
            if (header->slot_size < kSlotHeaderBytes)
                return false;
            return true;
        }

        return false;
    }

    inline bool Push(void* base, const std::byte* payload, uint32_t payload_size) noexcept {
        auto* header = static_cast<RingHeader*>(base);
        if (!header) {
            return false;
        }

        if (payload_size + kSlotHeaderBytes > header->slot_size)
            return false;

        const uint32_t mask = header->capacity - 1;
        auto* prod = ProducerOf(base);
        std::byte* ring = RingBaseOf(base);

        const uint64_t h = prod->head.load(std::memory_order_relaxed);
        SlotHeader* slot = SlotAt(ring, header->slot_size, mask, h);
        slot->size = payload_size;
        if (payload_size > 0) {
            std::memcpy(SlotPayload(slot), payload, payload_size);
        }
        slot->seq.store(h + 1, std::memory_order_release);
        prod->head.store(h + 1, std::memory_order_relaxed);
        return true;
    }

    inline PopResult Pop(void* base, std::byte* out, uint32_t out_capacity, uint32_t& out_size,
                         uint64_t& dropped) noexcept {
        auto* header = static_cast<RingHeader*>(base);
        auto* consumer = ConsumerOf(base);
        std::byte* ring = RingBaseOf(base);
        const uint32_t mask = header->capacity - 1;
        const uint32_t slot_size = header->slot_size;

        uint64_t tail = consumer->tail.load(std::memory_order_relaxed);
        SlotHeader* slot = SlotAt(ring, slot_size, mask, tail);
        const uint64_t observed = slot->seq.load(std::memory_order_acquire);
        const uint64_t expected = tail + 1;

        if (observed < expected) {
            if (header->closed.load(std::memory_order_acquire) != 0) {
                const uint64_t recheck = slot->seq.load(std::memory_order_acquire);
                if (recheck < expected) {
                    return PopResult::Closed;
                }
            }
            return PopResult::Empty;
        }

        if (observed > expected) {
            dropped += (observed - 1) - tail;
            tail = observed - 1;
        }

        const uint32_t size = slot->size;
        if (size > out_capacity || size + kSlotHeaderBytes > slot_size) {
            return PopResult::TornRead;
        }

        if (size > 0) {
            std::memcpy(out, SlotPayload(slot), size);
        }

        out_size = size;

        const uint64_t verify = slot->seq.load(std::memory_order_acquire);
        if (verify != observed) {
            return PopResult::TornRead;
        }

        consumer->tail.store(tail + 1, std::memory_order_relaxed);
        return PopResult::Ok;
    }

    inline void Close(void* base) noexcept {
        static_cast<RingHeader*>(base)->closed.store(1, std::memory_order_release);
    }
} // namespace VTX::shm_detail
