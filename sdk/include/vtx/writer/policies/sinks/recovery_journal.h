/**
 * @file recovery_journal.h
 * @brief Write-ahead journal (sidecar ".recovery" file) of committed chunk index
 *        entries, so a crash before the footer is written can be recovered.
 *
 * @details Each record is fixed-size and self-validating (per-record xxHash64), so
 * a torn last record from a crash mid-write is detected and dropped. Records are
 * appended AFTER the corresponding chunk is durably on disk (data-before-journal
 * ordering) so the journal never references a chunk that isn't there. On a clean
 * Close() the main-file footer is written and the ".recovery" file is deleted; its
 * presence at open time therefore signals an unclean shutdown.
 *
 * Byte layout (little-endian host):
 *   Header:  "VTXR" | u32 version | 4-byte main-file magic ("VTXP"/"VTXF")
 *   Record:  i32 chunk_index | i32 start_frame | i32 end_frame | u64 file_offset |
 *            u32 chunk_size_bytes | u64 chunk_checksum | u64 record_checksum
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <xxh3.h>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/policies/sinks/durable_file.h"

namespace VTX {

    class RecoveryJournal {
    public:
        static constexpr uint32_t kVersion = 1;
        static constexpr size_t kPayloadSize = 4 + 4 + 4 + 8 + 4 + 8; // 32
        static constexpr size_t kRecordSize = kPayloadSize + 8;       // 40 (+ record checksum)

        /// The sidecar path for a given main output file.
        static std::string PathFor(const std::string& main_file) { return main_file + ".recovery"; }

        /// Create/truncate the journal and write its header. @p format_magic is the
        /// main file's 4-byte magic. Returns false on failure.
        bool Open(const std::string& path, const std::string& format_magic, bool durable) {
            durable_ = durable;
            if (!file_.Open(path))
                return false;
            char magic[4] = {'V', 'T', 'X', 'R'};
            file_.Write(magic, 4);
            uint32_t version = kVersion;
            file_.Write(&version, sizeof(version));
            char fmt[4] = {' ', ' ', ' ', ' '};
            for (size_t i = 0; i < 4 && i < format_magic.size(); ++i)
                fmt[i] = format_magic[i];
            file_.Write(fmt, 4);
            if (durable_)
                file_.Sync();
            return true;
        }

        bool IsOpen() const { return file_.IsOpen(); }

        /// Append one committed-chunk record. Call AFTER the chunk is durable on disk.
        void AppendChunk(const ChunkIndexData& e) {
            uint8_t rec[kRecordSize];
            size_t o = 0;
            PackI32(rec, o, e.chunk_index);
            PackI32(rec, o, e.start_frame);
            PackI32(rec, o, e.end_frame);
            PackU64(rec, o, static_cast<uint64_t>(e.file_offset));
            PackU32(rec, o, e.chunk_size_bytes);
            PackU64(rec, o, e.checksum);
            uint64_t record_checksum = XXH3_64bits(rec, kPayloadSize);
            PackU64(rec, o, record_checksum);
            file_.Write(rec, kRecordSize);
            if (durable_)
                file_.Sync();
        }

        void Close() { file_.Close(); }

        // --- Read side (repair) ---

        struct Parsed {
            bool has_journal = false;   ///< The sidecar file existed.
            bool header_valid = false;  ///< Header magic/version parsed OK.
            std::string format_magic;   ///< Main-file magic recorded in the header.
            std::vector<ChunkIndexData> chunks; ///< Valid committed-chunk records, in order.
        };

        /// Parse a journal file, returning all VALID records up to the first torn/invalid one.
        static Parsed ReadValid(const std::string& path) {
            Parsed out;
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return out; // has_journal stays false
            out.has_journal = true;

            char header[12];
            in.read(header, sizeof(header));
            if (in.gcount() != static_cast<std::streamsize>(sizeof(header)))
                return out; // truncated header -> no usable records
            if (std::memcmp(header, "VTXR", 4) != 0)
                return out;
            uint32_t version = 0;
            std::memcpy(&version, header + 4, sizeof(version));
            if (version != kVersion)
                return out;
            out.format_magic.assign(header + 8, 4);
            out.header_valid = true;

            uint8_t rec[kRecordSize];
            while (in.read(reinterpret_cast<char*>(rec), kRecordSize),
                   in.gcount() == static_cast<std::streamsize>(kRecordSize)) {
                uint64_t stored = 0;
                std::memcpy(&stored, rec + kPayloadSize, sizeof(stored));
                if (XXH3_64bits(rec, kPayloadSize) != stored)
                    break; // torn / corrupt record -> stop here

                ChunkIndexData e;
                size_t o = 0;
                e.chunk_index = UnpackI32(rec, o);
                e.start_frame = UnpackI32(rec, o);
                e.end_frame = UnpackI32(rec, o);
                e.file_offset = static_cast<int64_t>(UnpackU64(rec, o));
                e.chunk_size_bytes = UnpackU32(rec, o);
                e.checksum = UnpackU64(rec, o);
                out.chunks.push_back(e);
            }
            return out;
        }

    private:
        static void PackI32(uint8_t* b, size_t& o, int32_t v) {
            uint32_t u = static_cast<uint32_t>(v);
            PackU32(b, o, u);
        }
        static void PackU32(uint8_t* b, size_t& o, uint32_t v) {
            b[o + 0] = static_cast<uint8_t>(v);
            b[o + 1] = static_cast<uint8_t>(v >> 8);
            b[o + 2] = static_cast<uint8_t>(v >> 16);
            b[o + 3] = static_cast<uint8_t>(v >> 24);
            o += 4;
        }
        static void PackU64(uint8_t* b, size_t& o, uint64_t v) {
            for (int i = 0; i < 8; ++i)
                b[o + i] = static_cast<uint8_t>(v >> (8 * i));
            o += 8;
        }
        static int32_t UnpackI32(const uint8_t* b, size_t& o) { return static_cast<int32_t>(UnpackU32(b, o)); }
        static uint32_t UnpackU32(const uint8_t* b, size_t& o) {
            uint32_t v = static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                         (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
            o += 4;
            return v;
        }
        static uint64_t UnpackU64(const uint8_t* b, size_t& o) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<uint64_t>(b[o + i]) << (8 * i);
            o += 8;
            return v;
        }

        DurableFile file_;
        bool durable_ = true;
    };

} // namespace VTX
