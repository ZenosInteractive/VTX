/**
 * @file recovery_journal.h
 * @brief Single write-ahead sidecar (".recovery") that makes a crashed recording
 *        recoverable up to the last frame.
 *
 * @details One file holds a typed record stream so a crash before the footer is
 * written can be recovered without losing committed chunks, per-frame times, or the
 * in-flight (un-flushed) frame batch. Every record is self-validating (per-record
 * xxHash64), so a torn last record from a crash mid-write is detected and dropped.
 *
 * Record types (each: [u8 type][u32 payload_len][payload][u64 checksum]):
 *   'S' session: written once after the header: {f32 fps, u8 is_increasing,
 *               u8 use_compression, i8 compression_level}. Lets the repair pass
 *               reconstruct the footer's derived time data (duration, timeline gaps,
 *               game segments) exactly as VTXGameTimes would have, and compress the
 *               synthesized footer exactly as the sink would have -- so a recovered
 *               file can be byte-identical to a cleanly closed one.
 *   'C' chunk : a committed chunk's index entry (offset/size/frames/checksum).
 *   'T' time  : one committed frame's {index, game_time, created_utc}.
 *   'F' frame : an un-flushed frame's {index, game_time, created_utc, chunk-payload
 *               bytes}.
 *
 * The log is strictly APPEND-ONLY in the crash-critical path: a frame appends an F
 * record; a flush appends the chunk's C record plus its frames' T records. Nothing
 * already written is ever moved or truncated in place, so at every instant the file
 * on disk is a valid prefix of records -- there is no window in which a durable chunk
 * is described by neither its F records nor its C record. Ordering is
 * data-before-journal: a chunk is fsync'd into the .vtx first, then its C/T records
 * are appended and fsync'd here.
 *
 * Once a chunk's C/T records are durable, that chunk's F records are redundant (the
 * frames now live in the .vtx and are recoverable via the C record); repair dedups
 * them by frame index. To keep the journal from growing to the size of the whole
 * recording, those redundant F records are reclaimed by periodic COMPACTION: the log
 * is rewritten (all C/T + only the still-pending F) into a temp file that atomically
 * replaces the sidecar. A crash during compaction leaves either the old or the new
 * file (rename is atomic) -- both are valid, complete journals.
 *
 * On a clean Close() the footer is written to the .vtx and the ".recovery" file is
 * deleted, so its presence at open time signals an unclean shutdown.
 *
 * Byte order: little-endian host (documented; matches the rest of the format).
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include <xxh3.h>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/policies/sinks/durable_file.h"

namespace VTX {

    class RecoveryJournal {
    public:
        static constexpr uint32_t kVersion = 4;
        static constexpr size_t kHeaderSize = 12; // "VTXR" + u32 version + 4-byte format magic

        struct FrameTime {
            int32_t index = 0;
            int64_t game_time = 0;
            int64_t created_utc = 0;
        };
        struct PendingFrame {
            int32_t index = 0;
            int64_t game_time = 0;
            int64_t created_utc = 0;
            std::string payload; // serialized single-frame chunk payload (as stored in the .vtx)
        };

        static std::string PathFor(const std::string& main_file) { return main_file + ".recovery"; }
        static std::string CompactTempFor(const std::string& journal_file) { return journal_file + ".compact"; }

        // --- Write side (append-only) ---

        /// @param fps               Writer's default FPS; lets repair reconstruct the
        ///                          footer's timeline-gap data exactly (0 disables gap
        ///                          detection, as in VTXGameTimes).
        /// @param is_increasing     Writer's game-time direction; lets repair reconstruct
        ///                          the footer's game-segment data exactly.
        /// @param use_compression   Sink's compression setting; lets repair compress the
        ///                          synthesized footer exactly as the sink would have.
        /// @param compression_level Sink's zstd level, paired with @p use_compression.
        bool Open(const std::string& path, const std::string& format_magic, bool durable, float fps = 0.0f,
                  bool is_increasing = true, bool use_compression = true, int8_t compression_level = 10) {
            durable_ = durable;
            path_ = path;
            fps_ = fps;
            is_increasing_ = is_increasing;
            use_compression_ = use_compression;
            compression_level_ = compression_level;
            format_magic_.assign(4, ' ');
            for (size_t i = 0; i < 4 && i < format_magic.size(); ++i)
                format_magic_[i] = format_magic[i];

            // Drop any temp left behind by a compaction interrupted in a previous run.
            std::error_code ec;
            std::filesystem::remove(CompactTempFor(path_), ec);

            if (!file_.Open(path_))
                return false;
            WriteHeaderTo(file_, format_magic_);
            WriteRecordTo(file_, 'S', BuildTimingPayload(fps_, is_increasing_, use_compression_, compression_level_));
            SyncOrFlush();
            pending_f_bytes_ = 0;
            orphan_f_bytes_ = 0;
            return file_.Good();
        }

        bool IsOpen() const { return file_.IsOpen(); }

        // Compaction fires when reclaimable (superseded) F bytes exceed this. Exposed so
        // tests can force compaction without writing hundreds of MB.
        void SetCompactThresholdBytes(uint64_t bytes) { compact_threshold_ = bytes; }

        // Append an un-flushed frame (F record) at the tail. Call as each frame is recorded.
        //
        // @param sync  When true (default, synchronous sink) the record is made durable
        //              immediately. When false the record is only appended to the buffer;
        //              the caller must issue a later SyncNow() to make this and any other
        //              un-synced F records durable together (group commit, used by the
        //              async sink). Batching only defers WHEN the append becomes durable,
        //              never the append ORDER: records still hit disk in write order, so a
        //              crash still lands on a clean contiguous prefix.
        void AppendFrame(int32_t index, int64_t game_time, int64_t created_utc, const std::string& chunk_payload,
                         bool sync = true) {
            // A frame whose record would exceed the read-side sanity bound could not be
            // parsed back on repair; skip journaling it rather than write an unreadable
            // record (the frame is still captured once its chunk is flushed to the .vtx).
            if (chunk_payload.size() + kFramePrefixBytes > kMaxRecordPayload)
                return;
            const std::vector<uint8_t> payload = BuildFramePayload(index, game_time, created_utc, chunk_payload);
            pending_f_bytes_ += WriteRecordTo(file_, 'F', payload);
            if (sync)
                SyncOrFlush();
        }

        // Make every buffered-but-un-synced record durable. Pairs with AppendFrame(sync=false)
        // to implement group commit: N appends, then ONE fsync for the whole batch.
        void SyncNow() { SyncOrFlush(); }

        // Commit a flushed chunk: durably append the chunk (C) and its frames' times (T).
        // Call AFTER the chunk is durable in the .vtx. Append-only -- the now-redundant F
        // records for this batch are left in place (repair dedups them) and reclaimed later
        // by compaction, so there is never a window in which the batch is unrecoverable.
        void CommitChunk(const ChunkIndexData& chunk, const std::vector<FrameTime>& batch_times) {
            WriteRecordTo(file_, 'C', BuildChunkPayload(chunk));
            for (const auto& t : batch_times)
                WriteRecordTo(file_, 'T', BuildTimePayload(t));
            SyncOrFlush();

            // This batch's F records are now superseded by the durable C record.
            orphan_f_bytes_ += pending_f_bytes_;
            pending_f_bytes_ = 0;
            if (orphan_f_bytes_ > compact_threshold_)
                Compact();
        }

        void Close() { file_.Close(); }

        // --- Read side (repair) ---

        struct Parsed {
            bool has_journal = false;
            bool looks_like_journal = false; ///< File starts with "VTXR" (any version).
            bool header_valid = false;
            std::string format_magic;
            bool has_timing = false;       ///< An 'S' record was read.
            float fps = 0.0f;              ///< Writer FPS (0 = gap detection disabled).
            bool is_increasing = true;     ///< Writer game-time direction.
            bool use_compression = true;   ///< Sink compression setting at record time.
            int8_t compression_level = 10; ///< Sink zstd level at record time.
            std::vector<ChunkIndexData> chunks;
            std::vector<FrameTime> times;
            std::vector<PendingFrame> frames;
        };

        static Parsed ReadValid(const std::string& path) {
            Parsed out;
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return out;
            out.has_journal = true;

            // Actual size on disk, used to bound per-record allocations: a corrupt
            // length field must not trigger a huge transient allocation (up to
            // kMaxRecordPayload) for a journal that is only a few KB long.
            uint64_t file_size = 0;
            {
                std::error_code ec;
                const auto s = std::filesystem::file_size(path, ec);
                if (!ec)
                    file_size = static_cast<uint64_t>(s);
            }

            char header[kHeaderSize];
            in.read(header, sizeof(header));
            if (in.gcount() >= 4 && std::memcmp(header, "VTXR", 4) == 0)
                out.looks_like_journal = true;
            if (in.gcount() != static_cast<std::streamsize>(sizeof(header)) || !out.looks_like_journal)
                return out;
            uint32_t version = 0;
            std::memcpy(&version, header + 4, sizeof(version));
            if (version != kVersion)
                return out;
            out.format_magic.assign(header + 8, 4);
            out.header_valid = true;

            uint64_t pos = kHeaderSize; // bytes consumed so far
            for (;;) {
                char head[5];
                in.read(head, 5); // u8 type + u32 len
                if (in.gcount() != 5)
                    break;
                pos += 5;
                const uint8_t type = static_cast<uint8_t>(head[0]);
                uint32_t len = 0;
                std::memcpy(&len, head + 1, sizeof(len));
                if (len > kMaxRecordPayload)
                    break; // implausible -> torn
                // The payload + trailing checksum cannot exceed what the file holds:
                // reject before allocating, so a corrupt length cannot balloon memory.
                // (Additive form -- no unsigned underflow if the size query failed.)
                if (pos + static_cast<uint64_t>(len) + 8 > file_size)
                    break;
                pos += static_cast<uint64_t>(len) + 8;
                std::string payload(len, '\0');
                if (len > 0) {
                    in.read(payload.data(), len);
                    if (in.gcount() != static_cast<std::streamsize>(len))
                        break;
                }
                char cbuf[8];
                in.read(cbuf, 8);
                if (in.gcount() != 8)
                    break;
                uint64_t stored = 0;
                std::memcpy(&stored, cbuf, sizeof(stored));

                // Checksum covers type + len + payload.
                XXH3_state_t state;
                XXH3_64bits_reset(&state);
                XXH3_64bits_update(&state, head, 5);
                if (len > 0)
                    XXH3_64bits_update(&state, payload.data(), len);
                if (XXH3_64bits_digest(&state) != stored)
                    break; // torn / corrupt record -> stop

                if (type == 'S') {
                    if (payload.size() != 7)
                        break;
                    size_t o = 0;
                    const uint32_t fps_bits = ReadU32(payload, o);
                    std::memcpy(&out.fps, &fps_bits, sizeof(out.fps));
                    out.is_increasing = payload[4] != 0;
                    out.use_compression = payload[5] != 0;
                    out.compression_level = static_cast<int8_t>(payload[6]);
                    out.has_timing = true;
                } else if (type == 'C') {
                    if (payload.size() != 32)
                        break;
                    size_t o = 0;
                    ChunkIndexData e;
                    e.chunk_index = ReadI32(payload, o);
                    e.start_frame = ReadI32(payload, o);
                    e.end_frame = ReadI32(payload, o);
                    e.file_offset = static_cast<int64_t>(ReadU64(payload, o));
                    e.chunk_size_bytes = ReadU32(payload, o);
                    e.checksum = ReadU64(payload, o);
                    out.chunks.push_back(e);
                } else if (type == 'T') {
                    if (payload.size() != 20)
                        break;
                    size_t o = 0;
                    FrameTime t;
                    t.index = ReadI32(payload, o);
                    t.game_time = ReadI64(payload, o);
                    t.created_utc = ReadI64(payload, o);
                    out.times.push_back(t);
                } else if (type == 'F') {
                    if (payload.size() < 20)
                        break;
                    size_t o = 0;
                    PendingFrame f;
                    f.index = ReadI32(payload, o);
                    f.game_time = ReadI64(payload, o); // carried for pending-frame time recovery
                    f.created_utc = ReadI64(payload, o);
                    f.payload = payload.substr(o);
                    out.frames.push_back(f);
                } else {
                    break; // unknown type -> stop
                }
            }
            return out;
        }

    private:
        static constexpr uint32_t kMaxRecordPayload = 512u * 1024u * 1024u; // 512 MB sanity bound
        static constexpr size_t kFramePrefixBytes = 20;                     // i32 index + i64 gt + i64 cu
        // Reclaim superseded F records once this many bytes have accrued. Also bounds the
        // transient memory a compaction reads back (superseded payloads are loaded then dropped).
        static constexpr uint64_t kDefaultCompactThreshold = 64ull * 1024ull * 1024ull;

        void SyncOrFlush() {
            if (durable_)
                file_.Sync();
            else
                file_.Flush();
        }

        // Rewrite the log keeping every C/T record and only the still-pending F records,
        // into a temp file that atomically replaces the sidecar. Crash-safe: until the
        // rename lands the original journal is intact; after it, the compacted one is.
        void Compact() {
            SyncOrFlush();
            file_.Close();

            const Parsed p = ReadValid(path_);
            if (!p.header_valid) {
                // Unexpected -- reopen the original and keep appending rather than risk data.
                file_.OpenExisting(path_);
                file_.SeekEnd();
                return;
            }
            int32_t last_committed = -1;
            for (const auto& c : p.chunks)
                if (c.end_frame > last_committed)
                    last_committed = c.end_frame;

            const std::string tmp = CompactTempFor(path_);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);

            DurableFile out;
            uint64_t kept_pending = 0;
            if (out.Open(tmp)) {
                WriteHeaderTo(out, p.format_magic.empty() ? format_magic_ : p.format_magic);
                WriteRecordTo(out, 'S', BuildTimingPayload(fps_, is_increasing_, use_compression_, compression_level_));
                for (const auto& c : p.chunks)
                    WriteRecordTo(out, 'C', BuildChunkPayload(c));
                for (const auto& t : p.times)
                    WriteRecordTo(out, 'T', BuildTimePayload(t));
                for (const auto& f : p.frames) {
                    if (f.index <= last_committed)
                        continue; // superseded -> drop
                    kept_pending +=
                        WriteRecordTo(out, 'F', BuildFramePayload(f.index, f.game_time, f.created_utc, f.payload));
                }
                out.Sync();
                const bool ok = out.Good();
                out.Close();
                if (ok) {
                    std::filesystem::rename(tmp, path_, ec);
                    if (ec)
                        std::filesystem::remove(tmp, ec); // rename failed -> keep the original
                    else {
                        orphan_f_bytes_ = 0;
                        pending_f_bytes_ = kept_pending;
                    }
                } else {
                    std::filesystem::remove(tmp, ec); // temp write failed -> keep the original
                }
            }

            file_.OpenExisting(path_);
            file_.SeekEnd();
        }

        static void WriteHeaderTo(DurableFile& f, const std::string& format_magic) {
            char magic[4] = {'V', 'T', 'X', 'R'};
            f.Write(magic, 4);
            uint32_t version = kVersion;
            f.Write(&version, sizeof(version));
            char fmt[4] = {' ', ' ', ' ', ' '};
            for (size_t i = 0; i < 4 && i < format_magic.size(); ++i)
                fmt[i] = format_magic[i];
            f.Write(fmt, 4);
        }

        // Writes one framed record and returns the number of bytes it occupies on disk.
        static uint64_t WriteRecordTo(DurableFile& f, char type, const std::vector<uint8_t>& payload) {
            uint8_t head[5];
            head[0] = static_cast<uint8_t>(type);
            const uint32_t len = static_cast<uint32_t>(payload.size());
            std::memcpy(head + 1, &len, sizeof(len));
            XXH3_state_t state;
            XXH3_64bits_reset(&state);
            XXH3_64bits_update(&state, head, 5);
            if (!payload.empty())
                XXH3_64bits_update(&state, payload.data(), payload.size());
            const uint64_t checksum = XXH3_64bits_digest(&state);
            f.Write(head, 5);
            if (!payload.empty())
                f.Write(payload.data(), payload.size());
            f.Write(&checksum, sizeof(checksum));
            return 5ull + payload.size() + 8ull;
        }

        static std::vector<uint8_t> BuildTimingPayload(float fps, bool is_increasing, bool use_compression,
                                                       int8_t compression_level) {
            std::vector<uint8_t> s;
            uint32_t fps_bits = 0;
            std::memcpy(&fps_bits, &fps, sizeof(fps_bits));
            AppendU32(s, fps_bits);
            s.push_back(is_increasing ? 1 : 0);
            s.push_back(use_compression ? 1 : 0);
            s.push_back(static_cast<uint8_t>(compression_level));
            return s;
        }

        static std::vector<uint8_t> BuildChunkPayload(const ChunkIndexData& chunk) {
            std::vector<uint8_t> c;
            AppendI32(c, chunk.chunk_index);
            AppendI32(c, chunk.start_frame);
            AppendI32(c, chunk.end_frame);
            AppendU64(c, static_cast<uint64_t>(chunk.file_offset));
            AppendU32(c, chunk.chunk_size_bytes);
            AppendU64(c, chunk.checksum);
            return c;
        }

        static std::vector<uint8_t> BuildTimePayload(const FrameTime& t) {
            std::vector<uint8_t> tp;
            AppendI32(tp, t.index);
            AppendI64(tp, t.game_time);
            AppendI64(tp, t.created_utc);
            return tp;
        }

        static std::vector<uint8_t> BuildFramePayload(int32_t index, int64_t game_time, int64_t created_utc,
                                                      const std::string& chunk_payload) {
            std::vector<uint8_t> payload;
            AppendI32(payload, index);
            AppendI64(payload, game_time);
            AppendI64(payload, created_utc);
            payload.insert(payload.end(), chunk_payload.begin(), chunk_payload.end());
            return payload;
        }

        static void AppendU32(std::vector<uint8_t>& b, uint32_t v) {
            for (int i = 0; i < 4; ++i)
                b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        static void AppendI32(std::vector<uint8_t>& b, int32_t v) { AppendU32(b, static_cast<uint32_t>(v)); }
        static void AppendU64(std::vector<uint8_t>& b, uint64_t v) {
            for (int i = 0; i < 8; ++i)
                b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        static void AppendI64(std::vector<uint8_t>& b, int64_t v) { AppendU64(b, static_cast<uint64_t>(v)); }

        static uint32_t ReadU32(const std::string& b, size_t& o) {
            uint32_t v = static_cast<uint8_t>(b[o]) | (static_cast<uint32_t>(static_cast<uint8_t>(b[o + 1])) << 8) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(b[o + 2])) << 16) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(b[o + 3])) << 24);
            o += 4;
            return v;
        }
        static int32_t ReadI32(const std::string& b, size_t& o) { return static_cast<int32_t>(ReadU32(b, o)); }
        static uint64_t ReadU64(const std::string& b, size_t& o) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<uint64_t>(static_cast<uint8_t>(b[o + i])) << (8 * i);
            o += 8;
            return v;
        }
        static int64_t ReadI64(const std::string& b, size_t& o) { return static_cast<int64_t>(ReadU64(b, o)); }

        DurableFile file_;
        bool durable_ = true;
        std::string path_;
        std::string format_magic_;
        float fps_ = 0.0f;
        bool is_increasing_ = true;
        bool use_compression_ = true;
        int8_t compression_level_ = 10;
        uint64_t pending_f_bytes_ = 0;                          // F bytes for the current un-flushed batch
        uint64_t orphan_f_bytes_ = 0;                           // superseded F bytes awaiting compaction
        uint64_t compact_threshold_ = kDefaultCompactThreshold; // reclaim when orphan_f_bytes_ exceeds this
    };

} // namespace VTX
