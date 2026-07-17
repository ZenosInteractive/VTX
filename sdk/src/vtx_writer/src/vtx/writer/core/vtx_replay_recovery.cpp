#include "vtx/writer/core/vtx_replay_recovery.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <xxh3.h>
#include <zstd.h>

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

        // True if the file already ends with a well-formed footer trailer
        // [u32 footer_size][4-byte magic]. A cleanly-closed file ends this way; a
        // crash-truncated (footerless) file almost never does. Used to detect a
        // complete file with only a leftover journal, so we DON'T rewrite its footer.
        bool HasValidTrailingFooter(const std::string& path, const std::string& magic, int64_t file_size) {
            if (file_size < 8)
                return false;
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return false;
            in.seekg(file_size - 8);
            char buf[8];
            in.read(buf, 8);
            if (in.gcount() != 8)
                return false;
            uint32_t footer_size = 0;
            std::memcpy(&footer_size, buf, sizeof(footer_size)); // host-native, matches the sink
            if (std::string(buf + 4, 4) != magic)
                return false;
            return footer_size > 0 && static_cast<int64_t>(footer_size) + 8 <= file_size;
        }

        // Mirror of ChunkedFileSink::CompressIfBeneficial, so a footer synthesized by
        // repair is byte-identical to one written by a clean Close() under the same
        // (journaled) compression settings.
        std::string CompressIfBeneficial(std::string payload, bool use_compression, int8_t level) {
            if (!use_compression || payload.size() < 512)
                return payload;
            const size_t max_size = ZSTD_compressBound(payload.size());
            std::string compressed(max_size, '\0');
            const size_t compressed_size =
                ZSTD_compress(compressed.data(), max_size, payload.data(), payload.size(), level);
            if (ZSTD_isError(compressed_size) || compressed_size >= payload.size())
                return payload;
            compressed.resize(compressed_size);
            return compressed;
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

            // Keep only committed chunks whose on-disk bytes are present and checksum-clean, in order.
            std::vector<ChunkIndexData> good;
            int64_t truncate_to = header_end;
            int32_t last_committed_frame = -1;
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
                last_committed_frame = c.end_frame;
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

            DurableFile out;
            if (!out.OpenExisting(path)) {
                r.error = "cannot reopen main file to append";
                return r;
            }
            out.SeekEnd();

            // Recover the in-flight (un-flushed) frames: append each pending frame (index
            // beyond the last committed one) as its own chunk, in contiguous order.
            std::vector<const RecoveryJournal::PendingFrame*> pending;
            for (const auto& f : journal.frames)
                if (f.index > last_committed_frame)
                    pending.push_back(&f);
            std::sort(pending.begin(), pending.end(),
                      [](const RecoveryJournal::PendingFrame* a, const RecoveryJournal::PendingFrame* b) {
                          return a->index < b->index;
                      });

            int32_t last_frame = last_committed_frame;
            int32_t expected = last_committed_frame + 1;
            for (const auto* f : pending) {
                if (f->index != expected || f->payload.empty())
                    break; // gap / duplicate / empty -> stop at the first hole
                const uint64_t offset = out.Tell();
                const uint32_t size = static_cast<uint32_t>(f->payload.size());
                out.Write(&size, sizeof(size));
                out.Write(f->payload.data(), f->payload.size());

                ChunkIndexData e;
                e.chunk_index = good.empty() ? 0 : good.back().chunk_index + 1;
                e.file_offset = static_cast<int64_t>(offset);
                e.start_frame = f->index;
                e.end_frame = f->index;
                e.chunk_size_bytes = size + static_cast<uint32_t>(sizeof(uint32_t));
                e.checksum = XXH3_64bits(f->payload.data(), f->payload.size());
                good.push_back(e);

                last_frame = f->index;
                ++expected;
            }

            const int32_t total_frames = last_frame + 1;

            // Reconstruct exact per-frame times from T records (committed) and F records (pending).
            std::vector<int64_t> game_times(total_frames > 0 ? static_cast<size_t>(total_frames) : 0, 0);
            std::vector<int64_t> created_utc(total_frames > 0 ? static_cast<size_t>(total_frames) : 0, 0);
            auto place = [&](int32_t idx, int64_t gt, int64_t cu) {
                if (idx >= 0 && idx < total_frames) {
                    game_times[static_cast<size_t>(idx)] = gt;
                    created_utc[static_cast<size_t>(idx)] = cu;
                }
            };
            for (const auto& t : journal.times)
                place(t.index, t.game_time, t.created_utc);
            for (const auto& f : journal.frames)
                place(f.index, f.game_time, f.created_utc);

            // Reconstruct the footer's derived time data exactly as VTXGameTimes would have
            // produced it (mirrors GetDuration / DetectGap / DetectGameSegment): duration
            // from the created_utc span, timeline gaps from UTC deltas vs the FPS-derived
            // threshold, and game segments from game-time direction reversals. Frame
            // numbers are 1-based, as in VTXGameTimes::GetFrameNumber(). Manual segment
            // marks are not journaled and so are not recovered.
            SessionFooter footer_data;
            footer_data.total_frames = total_frames;
            std::vector<int32_t> gaps;
            std::vector<int32_t> segments;
            if (total_frames > 0) {
                // GetDuration() returns float; keep the same float rounding before the
                // footer's double field so a recovered footer matches a clean Stop() exactly.
                const double duration = static_cast<float>(created_utc.back() - created_utc.front()) /
                                        static_cast<double>(GameTime::TICKS_PER_SECOND);
                footer_data.duration_seconds = static_cast<float>(duration);
            }
            if (journal.has_timing && total_frames > 1) {
                if (journal.fps > 0.0f) {
                    // Same expression as VTXGameTimes::SetFPS -> fps_inverse_.
                    const int64_t fps_inverse = static_cast<int64_t>((1.0f / journal.fps) * GameTime::TICKS_PER_SECOND);
                    const int64_t threshold = 3 * fps_inverse;
                    for (int32_t i = 1; i < total_frames; ++i)
                        if (created_utc[static_cast<size_t>(i)] - created_utc[static_cast<size_t>(i) - 1] > threshold)
                            gaps.push_back(i + 1);
                }
                for (int32_t i = 1; i < total_frames; ++i) {
                    const int64_t delta = game_times[static_cast<size_t>(i)] - game_times[static_cast<size_t>(i) - 1];
                    if ((journal.is_increasing && delta < 0) || (!journal.is_increasing && delta > 0))
                        segments.push_back(i + 1);
                }
            }
            // Always pass the vectors -- even empty -- exactly as Stop() does, so a
            // recovered footer serializes byte-identically to a clean shutdown's.
            footer_data.game_times = &game_times;
            footer_data.created_utc = &created_utc;
            footer_data.gaps = &gaps;
            footer_data.segments = &segments;
            // Compress exactly as the sink's Close() would have (settings journaled in
            // the 'S' record), so large recovered footers also match byte-for-byte.
            const std::string footer_payload = CompressIfBeneficial(Policy::SerializeFooter(good, footer_data),
                                                                    journal.use_compression, journal.compression_level);

            bool footer_ok = out.Write(footer_payload.data(), footer_payload.size());
            const uint32_t footer_size = static_cast<uint32_t>(footer_payload.size());
            footer_ok = out.Write(&footer_size, sizeof(footer_size)) && footer_ok;
            const std::string magic = Policy::GetMagicBytes();
            footer_ok = out.Write(magic.data(), magic.size()) && footer_ok;
            footer_ok = out.Sync() && footer_ok;
            // Good() also catches any failure in the pending-frame append writes above.
            const bool all_ok = footer_ok && out.Good();
            out.Close();

            if (!all_ok) {
                // Leave the journal in place: a re-run (more disk free, or a CLI tool) is
                // idempotent -- it re-truncates to the last committed chunk and retries.
                r.error = "failed to write the recovered footer durably (disk full?); journal left intact";
                return r;
            }

            const std::string journal_path = RecoveryJournal::PathFor(path);
            std::remove(journal_path.c_str());
            std::remove(RecoveryJournal::CompactTempFor(journal_path).c_str());

            r.repaired = true;
            r.recovered_chunks = static_cast<int32_t>(good.size());
            r.recovered_frames = total_frames;
            return r;
        }

    } // namespace

    std::string RecoveryJournalPath(const std::string& path) {
        return RecoveryJournal::PathFor(path);
    }

    bool ReplayNeedsRecovery(const std::string& path) {
        std::error_code ec;
        return std::filesystem::exists(RecoveryJournal::PathFor(path), ec);
    }

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

        const int64_t file_size = FileSizeOf(path);

        // A cleanly-finished file already ends with a valid footer; the journal is
        // just leftover (crash between the footer fsync and the journal delete).
        // Preserve the complete footer (incl. per-frame times) -- only drop the journal.
        if (HasValidTrailingFooter(path, m, file_size)) {
            // Only delete the sidecar if it actually IS a journal ("VTXR" magic) -- an
            // unrelated user file that merely shares the ".recovery" suffix must never
            // be destroyed on the strength of a name collision.
            if (journal.looks_like_journal) {
                std::remove(journal_path.c_str());
                std::remove(RecoveryJournal::CompactTempFor(journal_path).c_str());
            }
            r.was_clean = true;
            return r;
        }

        // A present-but-unparseable journal header (corrupt first bytes, or a version
        // from an incompatible SDK) carries no chunk index. Repairing from it would
        // truncate the main file down to just its header and destroy the body -- refuse
        // instead, leaving the file untouched for manual recovery.
        if (!journal.header_valid) {
            r.error = "recovery journal header is unreadable or from an incompatible version; refusing to repair";
            return r;
        }

        // Refuse a journal whose recorded format does not match this file (e.g. a
        // stale sidecar left over a file that was replaced with the other format).
        if (!journal.format_magic.empty() && journal.format_magic != m) {
            r.error = "recovery journal format (" + journal.format_magic + ") does not match file (" + m + ")";
            return r;
        }

        if (m == "VTXP")
            return RepairImpl<ProtobufVtxPolicy>(path, journal);
        if (m == "VTXF")
            return RepairImpl<FlatBuffersVtxPolicy>(path, journal);

        r.error = "unrecognized magic in main file; cannot repair";
        return r;
    }

} // namespace VTX
