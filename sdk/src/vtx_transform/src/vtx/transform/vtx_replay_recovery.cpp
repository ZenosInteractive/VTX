#include "vtx/transform/vtx_replay_recovery.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "vtx/common/vtx_replay_framing.h"
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
            if (file_size < static_cast<int64_t>(Framing::kFooterTrailerSize))
                return false;
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return false;
            uint64_t footer_offset = 0;
            uint32_t footer_size = 0;
            std::string ignored;
            return Framing::ProbeFooterTrailer(in, magic, static_cast<uint64_t>(file_size), footer_offset, footer_size,
                                               ignored);
        }

        // Byte offset where chunks begin (magic + u32 header_size + header), or -1 if the
        // header framing is not intact.
        int64_t ReadHeaderEnd(std::ifstream& in, const std::string& expect_magic, int64_t file_size) {
            uint64_t header_end = 0;
            std::string ignored;
            if (!Framing::ProbeHeader(in, expect_magic, static_cast<uint64_t>(file_size), header_end, ignored))
                return -1;
            return static_cast<int64_t>(header_end);
        }

        template <typename Policy>
        RepairResult RepairImpl(const std::string& path, const RecoveryJournal::Parsed& journal) {
            RepairResult r;
            const int64_t file_size = FileSizeOf(path);
            if (file_size < static_cast<int64_t>(Framing::kMagicSize + Framing::kSizePrefixSize)) {
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
                if (off < header_end || c.chunk_size_bytes <= Framing::kSizePrefixSize || end > file_size)
                    break; // torn tail / beyond EOF

                std::string bytes;
                bytes.resize(c.chunk_size_bytes);
                in.clear();
                in.seekg(off);
                in.read(bytes.data(), c.chunk_size_bytes);
                if (in.gcount() != static_cast<std::streamsize>(c.chunk_size_bytes))
                    break;

                // The stored checksum covers the payload after the size prefix.
                const uint64_t hash = Framing::PayloadChecksum(bytes.data() + Framing::kSizePrefixSize,
                                                               c.chunk_size_bytes - Framing::kSizePrefixSize);
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
                const Framing::SizePrefix prefix = Framing::EncodeSizePrefix(static_cast<uint32_t>(f->payload.size()));
                out.Write(prefix.data(), prefix.size());
                out.Write(f->payload.data(), f->payload.size());

                ChunkIndexData e;
                e.chunk_index = good.empty() ? 0 : good.back().chunk_index + 1;
                e.file_offset = static_cast<int64_t>(offset);
                e.start_frame = f->index;
                e.end_frame = f->index;
                e.chunk_size_bytes = Framing::ChunkSizeOnDisk(f->payload.size());
                e.checksum = Framing::PayloadChecksum(f->payload);
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
            const std::string footer_payload = Framing::CompressIfBeneficial(
                Policy::SerializeFooter(good, footer_data), journal.use_compression, journal.compression_level);

            bool footer_ok = out.Write(footer_payload.data(), footer_payload.size());
            const std::string trailer =
                Framing::FooterTrailer(static_cast<uint32_t>(footer_payload.size()), Policy::GetMagicBytes());
            footer_ok = out.Write(trailer.data(), trailer.size()) && footer_ok;
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
        char magic[Framing::kMagicSize] = {};
        in.read(magic, Framing::kMagicSize);
        in.close();
        const std::string m(magic, Framing::kMagicSize);

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
