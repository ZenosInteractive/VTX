/**
 * @file vtx_replay_framing.h
 * @brief The .vtx on-disk framing, defined once for every producer and consumer.
 *
 * @details A .vtx file is laid out as
 *
 *   [magic][u32 header_size][header]  [u32 chunk_size][chunk]*  [footer][u32 footer_size][magic]
 *
 * Every blob (header, chunk, footer) is zstd-compressed when that is beneficial and
 * left raw otherwise; readers sniff the zstd magic instead of trusting a flag. A
 * chunk's seek-table checksum is xxHash64 over its on-disk payload (the bytes after
 * the size prefix).
 *
 * This header is the single definition of those rules. ChunkedFileSink and
 * ChunkedNetworkSink (recording) and the vtx_transform tools that modify an existing
 * file (RepairReplayFile, CutReplayFile, FilterReplayFile) all frame bytes through it,
 * so a format change is made in one place and their outputs stay byte-identical to
 * each other by construction. The reader keeps its own header/footer parser (it owns
 * the corrupt-file diagnostics) but shares the constants and the zstd sniff.
 *
 * Byte order: sizes are stored in host byte order, which is little-endian on every
 * supported target -- exactly the bytes the format has always shipped.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <xxh3.h>

namespace VTX {
    namespace Framing {

        inline constexpr size_t kMagicSize = 4;                     ///< "VTXF" / "VTXP"
        inline constexpr size_t kSizePrefixSize = sizeof(uint32_t); ///< u32 before header / chunk payloads
        inline constexpr size_t kFooterTrailerSize = kSizePrefixSize + kMagicSize; ///< [u32 footer_size][magic]
        inline constexpr size_t kCompressMinBytes = 512;                           ///< Smaller payloads stay raw.
        inline constexpr int8_t kDefaultCompressionLevel = 10;                   ///< zstd level of the writer defaults.
        inline constexpr unsigned char kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD}; ///< zstd frame magic, as stored.

        // ------------------------------------------------------------------
        // Compression
        // ------------------------------------------------------------------

        /// True if @p data starts with the zstd frame magic.
        inline bool LooksLikeZstd(const void* data, size_t size) {
            return data != nullptr && size >= sizeof(kZstdMagic) &&
                   std::memcmp(data, kZstdMagic, sizeof(kZstdMagic)) == 0;
        }

        inline bool LooksLikeZstd(std::string_view bytes) {
            return LooksLikeZstd(bytes.data(), bytes.size());
        }

        /// The writer's rule for attempting compression at all: enabled and not tiny.
        inline bool ShouldCompress(size_t payload_size, bool enabled) {
            return enabled && payload_size >= kCompressMinBytes;
        }

        /// zstd at @p level; returns @p payload unchanged when zstd fails or does not
        /// shrink it (readers accept either form). Defined in vtx_common
        /// (vtx_replay_framing.cpp) so this header carries no zstd include.
        std::string Compress(std::string payload, int8_t level);

        /// ShouldCompress + Compress: the complete zstd-if-beneficial rule.
        inline std::string CompressIfBeneficial(std::string payload, bool enabled = true,
                                                int8_t level = kDefaultCompressionLevel) {
            if (!ShouldCompress(payload.size(), enabled)) {
                return payload;
            }
            return Compress(std::move(payload), level);
        }

        // ------------------------------------------------------------------
        // Checksums
        // ------------------------------------------------------------------

        /// Seek-table checksum of a chunk: xxHash64 over the on-disk payload (after the size prefix).
        inline uint64_t PayloadChecksum(const void* data, size_t size) {
            return XXH3_64bits(data, size);
        }

        inline uint64_t PayloadChecksum(std::string_view payload) {
            return PayloadChecksum(payload.data(), payload.size());
        }

        // ------------------------------------------------------------------
        // Size prefixes and framed blocks
        // ------------------------------------------------------------------

        using SizePrefix = std::array<char, kSizePrefixSize>;

        /// The u32 prefix exactly as it is written (host byte order).
        inline SizePrefix EncodeSizePrefix(uint32_t size) {
            SizePrefix out {};
            std::memcpy(out.data(), &size, sizeof(size));
            return out;
        }

        inline uint32_t DecodeSizePrefix(const char* bytes) {
            uint32_t size = 0;
            std::memcpy(&size, bytes, sizeof(size));
            return size;
        }

        /// Bytes a chunk occupies on disk: prefix + payload (what ChunkIndexEntry::chunk_size_bytes stores).
        inline uint32_t ChunkSizeOnDisk(size_t payload_size) {
            return static_cast<uint32_t>(payload_size + kSizePrefixSize);
        }

        /// [u32 size][payload]
        inline std::string ChunkBlock(std::string_view payload) {
            std::string block;
            block.reserve(kSizePrefixSize + payload.size());
            const SizePrefix prefix = EncodeSizePrefix(static_cast<uint32_t>(payload.size()));
            block.append(prefix.data(), prefix.size());
            block.append(payload.data(), payload.size());
            return block;
        }

        /// [magic][u32 size][payload]
        inline std::string HeaderBlock(std::string_view magic, std::string_view payload) {
            std::string block;
            block.reserve(magic.size() + kSizePrefixSize + payload.size());
            block.append(magic.data(), magic.size());
            const SizePrefix prefix = EncodeSizePrefix(static_cast<uint32_t>(payload.size()));
            block.append(prefix.data(), prefix.size());
            block.append(payload.data(), payload.size());
            return block;
        }

        /// [u32 footer_size][magic] -- the bytes that follow the footer payload.
        inline std::string FooterTrailer(uint32_t payload_size, std::string_view magic) {
            std::string trailer;
            trailer.reserve(kSizePrefixSize + magic.size());
            const SizePrefix prefix = EncodeSizePrefix(payload_size);
            trailer.append(prefix.data(), prefix.size());
            trailer.append(magic.data(), magic.size());
            return trailer;
        }

        /// [payload][u32 size][magic]
        inline std::string FooterBlock(std::string_view payload, std::string_view magic) {
            std::string block;
            block.reserve(payload.size() + kSizePrefixSize + magic.size());
            block.append(payload.data(), payload.size());
            block += FooterTrailer(static_cast<uint32_t>(payload.size()), magic);
            return block;
        }

        // ------------------------------------------------------------------
        // Locating the blocks of a finished file (rewriters copy bytes by range)
        // ------------------------------------------------------------------

        struct FileLayout {
            uint64_t file_size = 0;
            uint64_t header_end = 0;    ///< First byte after [magic][u32][header]; chunks start here.
            uint64_t footer_offset = 0; ///< First byte of the footer payload.
            uint32_t footer_size = 0;   ///< Footer payload bytes (before the trailer).
            bool footer_compressed = false;
        };

        /// Size of the stream in bytes; restores the read position.
        inline uint64_t StreamSize(std::istream& in) {
            in.clear();
            const std::streampos prev = in.tellg();
            in.seekg(0, std::ios::end);
            const std::streamoff end = in.tellg();
            in.clear();
            in.seekg(prev);
            return end < 0 ? 0 : static_cast<uint64_t>(end);
        }

        inline bool ReadAt(std::istream& in, uint64_t offset, void* dst, size_t size) {
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset));
            return static_cast<bool>(in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size)));
        }

        /// True if the @p available bytes at @p offset start with the zstd magic.
        inline bool IsZstdAt(std::istream& in, uint64_t offset, uint64_t available) {
            unsigned char magic[sizeof(kZstdMagic)];
            if (available < sizeof(magic) || !ReadAt(in, offset, magic, sizeof(magic))) {
                return false;
            }
            return LooksLikeZstd(magic, sizeof(magic));
        }

        /// Validates [magic][u32 header_size] at offset 0 and yields where the chunks begin.
        inline bool ProbeHeader(std::istream& in, std::string_view expected_magic, uint64_t file_size,
                                uint64_t& header_end, std::string& error) {
            if (file_size < expected_magic.size() + kSizePrefixSize) {
                error = "The replay is too small to hold a header.";
                return false;
            }
            std::string magic(expected_magic.size(), '\0');
            if (!ReadAt(in, 0, magic.data(), magic.size()) || magic != expected_magic) {
                error = "The replay does not start with the expected magic bytes.";
                return false;
            }
            char prefix[kSizePrefixSize];
            if (!ReadAt(in, expected_magic.size(), prefix, sizeof(prefix))) {
                error = "Could not read the header size of the replay.";
                return false;
            }
            const uint32_t header_size = DecodeSizePrefix(prefix);
            const uint64_t end = expected_magic.size() + kSizePrefixSize + header_size;
            if (header_size == 0 || end > file_size) {
                error = "The replay header size is implausible; the file is corrupt or truncated.";
                return false;
            }
            header_end = end;
            return true;
        }

        /// Validates the trailing [u32 footer_size][magic] and yields where the footer payload starts.
        /// A footerless (crash-truncated) file fails here.
        inline bool ProbeFooterTrailer(std::istream& in, std::string_view expected_magic, uint64_t file_size,
                                       uint64_t& footer_offset, uint32_t& footer_size, std::string& error) {
            const size_t trailer_size = kSizePrefixSize + expected_magic.size();
            if (file_size < trailer_size) {
                error = "The replay is too small to hold a footer.";
                return false;
            }
            std::string trailer(trailer_size, '\0');
            if (!ReadAt(in, file_size - trailer_size, trailer.data(), trailer.size())) {
                error = "Could not read the footer trailer of the replay.";
                return false;
            }
            if (std::string_view(trailer).substr(kSizePrefixSize) != expected_magic) {
                error = "The replay has no footer (incomplete recording?). Repair it first.";
                return false;
            }
            const uint32_t size = DecodeSizePrefix(trailer.data());
            if (size == 0 || static_cast<uint64_t>(size) + trailer_size > file_size) {
                error = "The replay footer size is implausible; the file is corrupt or truncated.";
                return false;
            }
            footer_size = size;
            footer_offset = file_size - trailer_size - size;
            return true;
        }

        /// Header + footer probe of a complete file, plus the footer's compression state.
        inline bool ProbeLayout(std::istream& in, std::string_view expected_magic, FileLayout& out,
                                std::string& error) {
            FileLayout layout;
            layout.file_size = StreamSize(in);
            if (!ProbeHeader(in, expected_magic, layout.file_size, layout.header_end, error)) {
                return false;
            }
            if (!ProbeFooterTrailer(in, expected_magic, layout.file_size, layout.footer_offset, layout.footer_size,
                                    error)) {
                return false;
            }
            if (layout.footer_offset < layout.header_end) {
                error = "The replay header and footer overlap; the file is corrupt.";
                return false;
            }
            layout.footer_compressed = IsZstdAt(in, layout.footer_offset, layout.footer_size);
            out = layout;
            return true;
        }

        /// Streams [offset, offset + bytes) from @p in to @p out through a bounded buffer.
        inline bool CopyRange(std::istream& in, std::ostream& out, uint64_t offset, uint64_t bytes,
                              std::string& error) {
            constexpr size_t kBufferSize = 4 * 1024 * 1024;
            std::vector<char> buffer(
                static_cast<size_t>(std::min<uint64_t>(std::max<uint64_t>(bytes, 1), kBufferSize)));
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset));
            uint64_t remaining = bytes;
            while (remaining > 0) {
                const size_t step = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                if (!in.read(buffer.data(), static_cast<std::streamsize>(step))) {
                    error = "Read failed at offset " + std::to_string(offset + (bytes - remaining)) + ".";
                    return false;
                }
                if (!out.write(buffer.data(), static_cast<std::streamsize>(step))) {
                    error = "Write failed after " + std::to_string(bytes - remaining) + " bytes.";
                    return false;
                }
                remaining -= step;
            }
            return true;
        }

    } // namespace Framing
} // namespace VTX
