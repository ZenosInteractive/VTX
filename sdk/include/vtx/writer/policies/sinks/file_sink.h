#pragma once
#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <atomic>
#include <chrono>
#include <zstd.h>
#include <xxh3.h>
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_concepts.h"
#include "vtx/writer/policies/sinks/durable_file.h"
#include "vtx/writer/policies/sinks/recovery_journal.h"
namespace VTX {

    struct FileSinkPerformanceStats {
        uint64_t serialization_us = 0;
        uint64_t compression_us = 0;
        uint64_t disk_write_us = 0;
    };

    // Called synchronously from the writer thread; implementations must be cheap.
    // Attach via Config::perf_observer; leave null for zero-cost no-op.
    class IFileSinkPerfObserver {
    public:
        virtual ~IFileSinkPerfObserver() = default;
        virtual void OnSerialize(std::chrono::microseconds us) = 0;
        virtual void OnCompress(std::chrono::microseconds us) = 0;
        virtual void OnDiskWrite(std::chrono::microseconds us) = 0;
    };

    // Default thread-safe collector. Share one instance across sinks to aggregate;
    // one per sink for per-writer measurements.
    class FileSinkAtomicPerfObserver : public IFileSinkPerfObserver {
    public:
        void OnSerialize(std::chrono::microseconds us) override {
            serialization_us_.fetch_add(static_cast<uint64_t>(us.count()), std::memory_order_relaxed);
        }
        void OnCompress(std::chrono::microseconds us) override {
            compression_us_.fetch_add(static_cast<uint64_t>(us.count()), std::memory_order_relaxed);
        }
        void OnDiskWrite(std::chrono::microseconds us) override {
            disk_write_us_.fetch_add(static_cast<uint64_t>(us.count()), std::memory_order_relaxed);
        }
        FileSinkPerformanceStats Snapshot() const {
            return {serialization_us_.load(std::memory_order_relaxed), compression_us_.load(std::memory_order_relaxed),
                    disk_write_us_.load(std::memory_order_relaxed)};
        }
        void Reset() {
            serialization_us_.store(0, std::memory_order_relaxed);
            compression_us_.store(0, std::memory_order_relaxed);
            disk_write_us_.store(0, std::memory_order_relaxed);
        }

    private:
        std::atomic<uint64_t> serialization_us_ {0};
        std::atomic<uint64_t> compression_us_ {0};
        std::atomic<uint64_t> disk_write_us_ {0};
    };

    template <IVtxWriterPolicy Policy>
    class ChunkedFileSink {
    public:
        using SerializerPolicy = Policy;
        using FrameType = typename SerializerPolicy::FrameType;
        using SchemaType = typename SerializerPolicy::SchemaType;
        using HeaderType = typename SerializerPolicy::HeaderType;

        struct Config {
            std::string filename;
            HeaderType header_config;
            bool b_use_compression = true;
            int8_t compression_level = 10;
            bool durable_writes = true;          ///< fsync each chunk to physical disk (crash/power-loss safe).
            bool enable_recovery_journal = true; ///< maintain a ".recovery" sidecar for crash recovery.
            uint64_t journal_compact_threshold_bytes = 0;   ///< journal compaction trigger; 0 = journal default.
            IFileSinkPerfObserver* perf_observer = nullptr; ///< optional perf timings; null = no-op.
        };

        explicit ChunkedFileSink(Config config)
            : config_(std::move(config)) {
            if (!file_.Open(config_.filename))
                throw std::runtime_error("VTX: Could not open " + config_.filename);
        }

        void OnSessionStart(const SchemaType& schema) {
            //this writes the vrx format, ie  "VTXP"(protobuff) VTXF(flatbuffer)
            std::string magic_bytes = SerializerPolicy::GetMagicBytes();
            WriteBlob(magic_bytes);

            const auto serialize_start = std::chrono::steady_clock::now();
            std::string header_payload = SerializerPolicy::SerializeHeader(config_.header_config, schema);
            NotifySerialize(serialize_start);
            header_payload = CompressIfBeneficial(std::move(header_payload));
            uint32_t final_size = static_cast<uint32_t>(header_payload.size());
            TimedWrite(&final_size, sizeof(final_size));
            TimedWrite(header_payload.data(), final_size);
            if (config_.durable_writes)
                file_.Sync();
            else
                file_.Flush(); // process-crash safe (reaches the OS) even without fsync

            // Start the crash-recovery journal only once the header is durable. If it
            // cannot be opened cleanly (or its own header write failed), disable it and
            // remove the torn sidecar -- a half-written journal would later block repair,
            // which is worse than recording without one.
            if (config_.enable_recovery_journal) {
                const std::string journal_path = RecoveryJournal::PathFor(config_.filename);
                if (!journal_.Open(journal_path, SerializerPolicy::GetMagicBytes(), config_.durable_writes,
                                   journal_fps_, journal_is_increasing_, config_.b_use_compression,
                                   config_.compression_level)) {
                    journal_.Close();
                    std::remove(journal_path.c_str());
                } else if (config_.journal_compact_threshold_bytes > 0) {
                    journal_.SetCompactThresholdBytes(config_.journal_compact_threshold_bytes);
                }
            } else {
                // Journaling opted out: remove any stale sidecar a previous (crashed)
                // session left for this filename, so it cannot masquerade as recovery
                // state for THIS recording.
                std::remove(RecoveryJournal::PathFor(config_.filename).c_str());
                std::remove(RecoveryJournal::CompactTempFor(RecoveryJournal::PathFor(config_.filename)).c_str());
            }
        }

        // Timing parameters the writer resolved from its config; journaled once ('S'
        // record) so a repair can reconstruct the footer's derived time data (duration,
        // gaps, segments) exactly. Call before OnSessionStart.
        void JournalTiming(float fps, bool is_increasing) {
            journal_fps_ = fps;
            journal_is_increasing_ = is_increasing;
        }

        void SaveChunk(std::vector<std::unique_ptr<FrameType>>& frames, const std::vector<int64_t>& created_utc,
                       int32_t start_frame, int32_t total_frames) {
            if (frames.empty())
                return;

            const auto serialize_start = std::chrono::steady_clock::now();
            std::string payload = SerializerPolicy::SerializeChunk(frames, chunkIndex_, config_.b_use_compression);
            NotifySerialize(serialize_start);
            payload = CompressIfBeneficial(std::move(payload));

            uint64_t current_offset = file_.Tell();
            uint32_t final_size = static_cast<uint32_t>(payload.size());

            TimedWrite(&final_size, sizeof(final_size));
            TimedWrite(payload.data(), final_size);
            if (config_.durable_writes)
                file_.Sync();
            else
                file_.Flush(); // process-crash safe (reaches the OS) even without fsync

            ChunkIndexData indexEntry;
            indexEntry.chunk_index = chunkIndex_++;
            indexEntry.file_offset = current_offset;
            indexEntry.start_frame = start_frame;
            indexEntry.end_frame = total_frames - 1;
            indexEntry.chunk_size_bytes = final_size + sizeof(uint32_t);
            indexEntry.checksum = XXH3_64bits(payload.data(), payload.size());
            seek_table_.push_back(indexEntry);

            // Commit the chunk to the journal AFTER its bytes are durable on disk
            // (data-before-journal): drop the now-redundant pending-frame tail and
            // record the chunk (C) plus its frames' times (T).
            if (journal_.IsOpen()) {
                journal_.CommitChunk(indexEntry, batch_times_);
                batch_times_.clear();
            }
        }

        // Journal a single recorded frame BEFORE it is flushed as part of a chunk, so
        // a crash mid-batch can still recover the in-flight frames (and their times).
        void JournalFrame(const FrameType& frame, int32_t frame_index, int64_t game_time, int64_t created_utc) {
            JournalFrameImpl(frame, frame_index, game_time, created_utc, /*sync=*/true);
        }

        // Same as JournalFrame but does NOT force the F record durable; the caller (the async
        // sink's I/O worker) batches a run of these and issues one SyncJournal() at the end --
        // group commit. Only WHEN the record becomes durable changes; the append order (and
        // therefore crash-recovery contiguity) is identical to the synchronous path.
        void JournalFrameBatched(const FrameType& frame, int32_t frame_index, int64_t game_time, int64_t created_utc) {
            JournalFrameImpl(frame, frame_index, game_time, created_utc, /*sync=*/false);
        }

        // Flush the journal's group-commit batch to durability. No-op if journaling is off.
        void SyncJournal() {
            if (journal_.IsOpen())
                journal_.SyncNow();
        }

        // True once OnSessionStart has an open recovery journal. Lets the async adapter skip
        // enqueuing (and copying) journal frames when journaling is disabled or failed to open.
        bool IsJournalActive() const { return journal_.IsOpen(); }

        // True while no write/seek/sync on the main .vtx file has failed. The async worker
        // latches its failure protocol on this going false (see AbortClose()).
        bool Good() const { return file_.Good(); }

        // Failure/abort path: release the file + journal handles WITHOUT writing a footer and
        // WITHOUT deleting the recovery journal. On Windows this drops the deny-write share so
        // the partially written .vtx and its journal become repair-ready immediately, even while
        // the process keeps running. (Contrast Close(), which finalizes a clean recording.)
        void AbortClose() {
            file_.Close();
            if (journal_.IsOpen())
                journal_.Close();
        }

        void Close(const SessionFooter& footerData) {
            if (!file_.IsOpen())
                return;
            const auto serialize_start = std::chrono::steady_clock::now();
            std::string footer_payload = SerializerPolicy::SerializeFooter(seek_table_, footerData);
            NotifySerialize(serialize_start);
            footer_payload = CompressIfBeneficial(std::move(footer_payload));

            TimedWrite(footer_payload.data(), footer_payload.size());
            uint32_t final_size = static_cast<uint32_t>(footer_payload.size());
            TimedWrite(&final_size, sizeof(final_size));
            WriteBlob(SerializerPolicy::GetMagicBytes());
            if (config_.durable_writes)
                file_.Sync();
            else
                file_.Flush(); // process-crash safe (reaches the OS) even without fsync

            // Clean shutdown: the footer is durable, so the recovery journal is no
            // longer needed. Its absence signals a clean file to the repair path.
            if (journal_.IsOpen()) {
                journal_.Close();
                std::remove(RecoveryJournal::PathFor(config_.filename).c_str());
            }
        }

    private:
        void JournalFrameImpl(const FrameType& frame, int32_t frame_index, int64_t game_time, int64_t created_utc,
                              bool sync) {
            if (!journal_.IsOpen())
                return;
            std::vector<std::unique_ptr<FrameType>> one;
            one.push_back(std::make_unique<FrameType>(frame));
            std::string payload = SerializerPolicy::SerializeChunk(one, /*chunk_idx*/ 0, config_.b_use_compression);
            payload = CompressIfBeneficial(std::move(payload));
            journal_.AppendFrame(frame_index, game_time, created_utc, payload, sync);
            batch_times_.push_back({frame_index, game_time, created_utc});
        }

        void NotifySerialize(std::chrono::steady_clock::time_point start) const {
            if (config_.perf_observer)
                config_.perf_observer->OnSerialize(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start));
        }
        void NotifyCompress(std::chrono::steady_clock::time_point start) const {
            if (config_.perf_observer)
                config_.perf_observer->OnCompress(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start));
        }
        void NotifyDiskWrite(std::chrono::steady_clock::time_point start) const {
            if (config_.perf_observer)
                config_.perf_observer->OnDiskWrite(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start));
        }
        void TimedWrite(const void* data, size_t size) {
            const auto start = std::chrono::steady_clock::now();
            file_.Write(data, size);
            NotifyDiskWrite(start);
        }
        void WriteBlob(const std::string& data) { TimedWrite(data.data(), data.size()); }

        std::string CompressIfBeneficial(std::string payload) {
            if (!config_.b_use_compression || payload.size() < 512) {
                return payload;
            }

            const auto compression_start = std::chrono::steady_clock::now();
            size_t const max_size = ZSTD_compressBound(payload.size());
            std::string compressed_blob(max_size, '\0');

            size_t const compressed_size = ZSTD_compress(compressed_blob.data(), max_size, payload.data(),
                                                         payload.size(), config_.compression_level);
            NotifyCompress(compression_start);

            if (ZSTD_isError(compressed_size)) {
                return payload;
            }

            if (compressed_size >= payload.size()) {
                return payload;
            }

            compressed_blob.resize(compressed_size);
            return compressed_blob;
        }

        Config config_;
        DurableFile file_;
        RecoveryJournal journal_;
        std::vector<RecoveryJournal::FrameTime> batch_times_; // times of the current un-flushed batch
        float journal_fps_ = 0.0f;                            // writer timing, journaled in the 'S' record
        bool journal_is_increasing_ = true;
        int32_t chunkIndex_ = 0;
        std::vector<ChunkIndexData> seek_table_; //Generic tables, format agnostic
    };
}; // namespace VTX
