#include "vtx/writer/core/vtx_replay_recovery.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <xxh3.h>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"
#include "vtx/writer/policies/sinks/durable_file.h"
#include "vtx/writer/policies/sinks/recovery_journal.h"

namespace VTX {
    namespace {

        int64_t FileSizeOf(const std::string& path) {
            std::error_code ec;
            auto s = std::filesystem::file_size(path, ec);
            return ec ? -1 : static_cast<int64_t>(s);
        }

        // Byte offset where chunks begin (magic(4) + u32 header_size + header), or -1 if
        // the header framing is not intact.
        int64_t ReadHeaderEnd(std::ifstream& in, const std::string& expect_magic, int64_t file_size) {
            in.clear();
            in.seekg(0);
            char magic[4];
            in.read(magic, 4);
            if (in.gcount() != 4 || std::string(magic, 4) != expect_magic)
                return -1;
            uint32_t header_size = 0;
            in.read(reinterpret_cast<char*>(&header_size), sizeof(header_size));
            if (in.gcount() != static_cast<std::streamsize>(sizeof(header_size)))
                return -1;
            const int64_t header_end = 8 + static_cast<int64_t>(header_size);
            if (header_size == 0 || header_end > file_size)
                return -1;
            return header_end;
        }

        template <typename Policy>
        RepairResult RepairImpl(const std::string& path, const RecoveryJournal::Parsed& journal) {
            RepairResult r;
            const int64_t file_size = FileSizeOf(path);
            if (file_size < 8) {
                r.error = "main file too small or missing";
                return r;
            }

            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                r.error = "cannot open main file";
                return r;
            }
            const int64_t header_end = ReadHeaderEnd(in, Policy::GetMagicBytes(), file_size);
            if (header_end < 0) {
                r.error = "header framing not intact; cannot repair";
                return r;
            }

            // Keep only chunks whose on-disk bytes are present and checksum-clean, in order.
            std::vector<ChunkIndexData> good;
            int64_t truncate_to = header_end;
            for (const auto& c : journal.chunks) {
                const int64_t off = c.file_offset;
                const int64_t end = off + static_cast<int64_t>(c.chunk_size_bytes);
                if (off < header_end || c.chunk_size_bytes <= sizeof(uint32_t) || end > file_size)
                    break; // torn tail / beyond EOF

                std::string bytes;
                bytes.resize(c.chunk_size_bytes);
                in.clear();
                in.seekg(off);
                in.read(bytes.data(), c.chunk_size_bytes);
                if (in.gcount() != static_cast<std::streamsize>(c.chunk_size_bytes))
                    break;

                // The stored checksum covers the payload after the 4-byte length prefix.
                const uint64_t hash =
                    XXH3_64bits(bytes.data() + sizeof(uint32_t), c.chunk_size_bytes - sizeof(uint32_t));
                if (c.checksum != 0 && hash != c.checksum)
                    break; // corrupt chunk -> stop, drop it and everything after

                good.push_back(c);
                truncate_to = end;
            }
            in.close();

            // Drop the torn tail (a partial chunk written before its journal record, or a
            // stale footer from a crash between footer-write and journal-delete).
            std::error_code ec;
            std::filesystem::resize_file(path, static_cast<std::uintmax_t>(truncate_to), ec);
            if (ec) {
                r.error = "failed to truncate main file: " + ec.message();
                return r;
            }

            // Synthesize and append a footer over the recovered chunk index.
            SessionFooter footer_data;
            footer_data.total_frames = good.empty() ? 0 : (good.back().end_frame + 1);
            const std::string footer_payload = Policy::SerializeFooter(good, footer_data);

            DurableFile out;
            if (!out.OpenExisting(path)) {
                r.error = "cannot reopen main file to append footer";
                return r;
            }
            out.SeekEnd();
            out.Write(footer_payload.data(), footer_payload.size());
            const uint32_t footer_size = static_cast<uint32_t>(footer_payload.size());
            out.Write(&footer_size, sizeof(footer_size));
            const std::string magic = Policy::GetMagicBytes();
            out.Write(magic.data(), magic.size());
            out.Sync();
            out.Close();

            std::remove(RecoveryJournal::PathFor(path).c_str());

            r.repaired = true;
            r.recovered_chunks = static_cast<int32_t>(good.size());
            r.recovered_frames = footer_data.total_frames;
            return r;
        }

    } // namespace

    RepairResult RepairReplayFile(const std::string& path) {
        RepairResult r;

        const std::string journal_path = RecoveryJournal::PathFor(path);
        const RecoveryJournal::Parsed journal = RecoveryJournal::ReadValid(journal_path);
        if (!journal.has_journal) {
            r.was_clean = true; // no sidecar -> file was closed cleanly (or never journaled)
            return r;
        }

        // Format is decided by the main file's leading magic.
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            r.error = "cannot open main file: " + path;
            return r;
        }
        char magic[4] = {0, 0, 0, 0};
        in.read(magic, 4);
        in.close();
        const std::string m(magic, 4);

        if (m == "VTXP")
            return RepairImpl<ProtobufVtxPolicy>(path, journal);
        if (m == "VTXF")
            return RepairImpl<FlatBuffersVtxPolicy>(path, journal);

        r.error = "unrecognized magic in main file; cannot repair";
        return r;
    }

} // namespace VTX
