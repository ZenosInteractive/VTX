/**
 * @file binary_cursor.h
 * @brief Lightweight cursor over a contiguous byte buffer for typed sequential reads.
 *
 * @details Owns nothing -- just a std::span<const std::byte> view, current position
 *          and the source endianness. The cursor advances automatically on Read*()
 *          and Skip(). Bounds-checked: overrun throws std::out_of_range.
 *
 *          Endianness defaults to native; pass std::endian::big or std::endian::little
 *          to byte-swap on multi-byte arithmetic reads (cross-platform binary formats).
 *
 *          Intended use: a client writes a BinaryBinding<Tag>::Transfer that walks the
 *          cursor with Read<T>() / ReadString / ReadCStr / ReadLenString / SubCursor in
 *          the exact order the data appears in the buffer. The loader (binary_loader.h)
 *          then plugs each value into the right PropertyContainer slot via LoadField.
 *
 * @author Zenos Interactive
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib> // _byteswap_* on MSVC
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace VTX {

    /**
     * @class BinaryCursor
     * @brief Typed forward-only reader over a byte buffer.
     *
     * @details Holds a non-owning std::span<const std::byte> view plus a current
     *          position and a source endianness. All Read*() and Skip*() operations
     *          advance the position; bounds violations throw std::out_of_range.
     */
    class BinaryCursor {
    public:
        /**
         * @brief Primary constructor over a std::span<const std::byte>.
         * @details Construct via std::as_bytes(std::span{buffer}) from
         *          std::vector<T>, std::array<T,N>, or any contiguous range of
         *          byte-like values.
         * @param data Read-only view over the source buffer.
         * @param endian Source endianness (default: native; pass big/little to
         *               trigger automatic byte-swapping inside Read<T>()).
         */
        explicit BinaryCursor(std::span<const std::byte> data, std::endian endian = std::endian::native)
            : data_(data)
            , pos_(0)
            , endian_(endian) {}

        /**
         * @brief Convenience constructor for raw (pointer, size) pairs.
         * @details Provided for callers that already work with
         *          std::vector<uint8_t>::data() / size() or with C-style buffers.
         * @param base Pointer to the first byte of the buffer.
         * @param size Number of bytes available from @p base.
         * @param endian Source endianness (default: native).
         */
        BinaryCursor(const uint8_t* base, size_t size, std::endian endian = std::endian::native)
            : data_(reinterpret_cast<const std::byte*>(base), size)
            , pos_(0)
            , endian_(endian) {}


        /** @brief Current read position in bytes from the start of the buffer. */
        size_t Tell() const { return pos_; }

        /** @brief Total size of the underlying buffer in bytes. */
        size_t Size() const { return data_.size(); }

        /** @brief Bytes still readable from the current position. */
        size_t Remaining() const { return data_.size() - pos_; }

        /** @brief True once the cursor has consumed (or passed) the whole buffer. */
        bool Eof() const { return pos_ >= data_.size(); }

        /** @brief Underlying byte view (read-only). */
        std::span<const std::byte> Data() const { return data_; }

        /** @brief Source endianness the cursor was constructed with. */
        std::endian Endian() const { return endian_; }

        /** @brief True if Read<T>() will byte-swap to match the native endianness. */
        bool NeedsSwap() const { return endian_ != std::endian::native; }


        /**
         * @brief Move the cursor to an absolute position in the buffer.
         * @param pos New position in bytes from the start of the buffer.
         * @throws std::out_of_range if @p pos > Size().
         */
        void Seek(size_t pos) {
            if (pos > data_.size()) {
                throw std::out_of_range("BinaryCursor::Seek beyond end of buffer");
            }
            pos_ = pos;
        }

        /**
         * @brief Skip @p bytes from the current position without reading.
         * @param bytes Number of bytes to skip forward.
         * @throws std::out_of_range if the skip would overrun the buffer.
         */
        void Skip(size_t bytes) {
            EnsureBounds(bytes);
            pos_ += bytes;
        }

        /**
         * @brief Advance the cursor to the next multiple of @p alignment.
         * @details No-op if @p alignment is 0 or the cursor is already aligned.
         * @param alignment Alignment in bytes (typically 2, 4, 8, 16).
         * @throws std::out_of_range if the required padding would overrun.
         */
        void AlignTo(size_t alignment) {
            if (alignment == 0) {
                return;
            }
            const size_t misalign = pos_ % alignment;
            if (misalign != 0) {
                Skip(alignment - misalign);
            }
        }


        /**
         * @brief Read a trivially-copyable value of type @p T and advance the cursor.
         * @details Reads sizeof(T) bytes via memcpy (safe for unaligned access),
         *          advances the position, and byte-swaps the result if the cursor
         *          endianness differs from native AND @p T is a multi-byte
         *          arithmetic type.
         * @tparam T Trivially-copyable type to deserialize.
         * @return The deserialized value, in native byte order.
         * @throws std::out_of_range if fewer than sizeof(T) bytes remain.
         */
        template <typename T>
        T Read() {
            static_assert(std::is_trivially_copyable_v<T>, "BinaryCursor::Read<T>: T must be trivially copyable.");
            EnsureBounds(sizeof(T));
            T value;
            std::memcpy(&value, data_.data() + pos_, sizeof(T));
            pos_ += sizeof(T);
            if constexpr (sizeof(T) > 1 && std::is_arithmetic_v<T>) {
                if (NeedsSwap()) {
                    value = ByteSwap(value);
                }
            }
            return value;
        }


        /**
         * @brief Read a fixed-length string (NOT null-terminated).
         * @param length Number of bytes to consume into the string.
         * @return The bytes interpreted as a std::string.
         * @throws std::out_of_range if @p length > Remaining().
         */
        std::string ReadString(size_t length) {
            EnsureBounds(length);
            std::string s(reinterpret_cast<const char*>(data_.data() + pos_), length);
            pos_ += length;
            return s;
        }

        /**
         * @brief Read a null-terminated C-string, up to @p max_len bytes.
         * @details Advances past the string and its null terminator (if found).
         *          If the terminator is not found within @p max_len (or before
         *          end-of-buffer), the scanned range is returned and the cursor
         *          stops at the last scanned byte.
         * @param max_len Maximum number of bytes to scan (default: unlimited).
         * @return The deserialized string (without the terminator).
         */
        std::string ReadCStr(size_t max_len = SIZE_MAX) {
            const size_t cap = std::min(max_len, Remaining());
            size_t len = 0;
            while (len < cap && static_cast<uint8_t>(data_[pos_ + len]) != 0) {
                ++len;
            }
            std::string s(reinterpret_cast<const char*>(data_.data() + pos_), len);
            pos_ += len;
            if (pos_ < data_.size() && static_cast<uint8_t>(data_[pos_]) == 0) {
                ++pos_;
            }
            return s;
        }

        /**
         * @brief Read a length-prefixed string.
         * @details Reads a length value of type @p LenT (endian-swapped if needed),
         *          then consumes that many bytes as the string body. No null terminator.
         * @tparam LenT Length-prefix type (typically uint8_t / uint16_t / uint32_t).
         * @return The deserialized string.
         * @throws std::out_of_range if the buffer ends before the prefix or body.
         */
        template <typename LenT>
        std::string ReadLenString() {
            const auto len = Read<LenT>();
            return ReadString(static_cast<size_t>(len));
        }


        /**
         * @brief Carve a sub-cursor of @p len bytes starting at the current position.
         * @details The parent advances past the slice. The sub-cursor inherits the
         *          parent's endianness. Useful for nested structs with a known
         *          size, or for frame chunks in a stream of length-prefixed frames.
         * @param len Size of the sub-region in bytes.
         * @return A new BinaryCursor scoped to the carved slice.
         * @throws std::out_of_range if @p len > Remaining().
         */
        BinaryCursor SubCursor(size_t len) {
            EnsureBounds(len);
            BinaryCursor sub(data_.subspan(pos_, len), endian_);
            pos_ += len;
            return sub;
        }

    private:
        /**
         * @brief Internal bounds-check helper.
         * @param n Number of additional bytes about to be consumed.
         * @throws std::out_of_range if pos_ + n > data_.size().
         */
        void EnsureBounds(size_t n) const {
            if (pos_ + n > data_.size()) {
                throw std::out_of_range("BinaryCursor: read past end of buffer");
            }
        }

        /**
         * @details Delegates to the compiler intrinsic, typically a single `bswap`
         *          instruction on x86. Falls through to a no-op for sizes other
         *          than 2/4/8 bytes (e.g. char, long double).
         * @tparam T Trivially-copyable arithmetic type.
         * @param value Value to byte-swap.
         * @return The byte-swapped value (or unchanged if size is not 2/4/8).
         */
        template <typename T>
        static T ByteSwap(T value) {
            if constexpr (sizeof(T) == 2) {
                uint16_t raw;
                std::memcpy(&raw, &value, sizeof(T));
                #if defined(_MSC_VER)
                                raw = _byteswap_ushort(raw);
                #else
                                raw = __builtin_bswap16(raw);
                #endif
                                std::memcpy(&value, &raw, sizeof(T));
                            } else if constexpr (sizeof(T) == 4) {
                                uint32_t raw;
                                std::memcpy(&raw, &value, sizeof(T));
                #if defined(_MSC_VER)
                                raw = _byteswap_ulong(raw);
                #else
                                raw = __builtin_bswap32(raw);
                #endif
                                std::memcpy(&value, &raw, sizeof(T));
                            } else if constexpr (sizeof(T) == 8) {
                                uint64_t raw;
                                std::memcpy(&raw, &value, sizeof(T));
                #if defined(_MSC_VER)
                                raw = _byteswap_uint64(raw);
                #else
                                raw = __builtin_bswap64(raw);
                #endif
                std::memcpy(&value, &raw, sizeof(T));
            }
            return value;
        }

        std::span<const std::byte> data_;
        size_t pos_;
        std::endian endian_;
    };

} // namespace VTX
