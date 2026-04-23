#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <functional>
#include <algorithm>
#include <iostream>
#include <ranges>
#include <stop_token>
#include <stdexcept>
#include <span>

#include "vtx_deserializer_service.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_concepts.h"
#include "vtx/reader/core/vtx_schema_adapter.h"
#include "vtx_frame_accessor.h"
#include "vtx/reader/serialization/flatbuffers_to_vtx.h"

namespace VTX {

    struct ReaderMetrics {
        int32_t active_chunk = -1;
        int32_t cache_window_backward = 0;
        int32_t cache_window_forward = 0;

        std::vector<int32_t> cached_chunks;
        std::vector<int32_t> pending_chunks;

        size_t memory_used_bytes = 0;
    };

    struct ReplayReaderEvents {
        std::function<void(int32_t)> OnChunkLoadStarted;
        std::function<void(int32_t)> OnChunkLoadFinished;
        std::function<void(int32_t)> OnChunkEvicted;
    };

    template <IVtxReaderPolicy SerializerPolicy>
    class ReplayReader {
    public:
        using HeaderType = typename SerializerPolicy::HeaderType;
        using FooterType = typename SerializerPolicy::FooterType;
        using SchemaType = typename SerializerPolicy::SchemaType;

        explicit ReplayReader(const std::string& filepath) : filepath_(filepath) {
            std::ifstream init_stream(filepath_, std::ios::binary | std::ios::in);
            if (!init_stream.is_open()) {
                throw std::runtime_error("VTX Reader: Can not open the file " + filepath);
            }

            try {
                ReadHeader(init_stream);
                ReadFooter(init_stream);
            } catch (const std::exception& e) {
                init_stream.close();
                throw std::runtime_error(std::string("VTX Reader Init Failed: ") + e.what());
            }
        }

        ~ReplayReader() {
            // Cancel every in-flight prefetch, then wait for each task to
            // observe the stop_token and exit.  Per-chunk `stop_source`s
            // replace the previous single global `stop_source_`; the wait
            // step is unchanged because `shared_future` destruction does
            // not block and we still need to synchronise with the worker
            // threads before releasing `*this`.
            std::vector<std::shared_future<void>> tasks;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (auto& kv : pending_loads_) {
                    kv.second.stop.request_stop();
                    if (kv.second.future.valid()) tasks.push_back(kv.second.future);
                }
            }

            for (auto& task : tasks) { task.wait(); }
        }

        // Fix A4: protect events_ with a mutex.  std::function is not
        // thread-safe -- reading it while another thread writes is UB.
        // We snapshot events_ at every callback site so the actual callback
        // invocation happens outside the lock (avoiding deadlock if the
        // user's callback re-enters the reader).
        void SetEvents(const ReplayReaderEvents& events) {
            std::lock_guard<std::mutex> lock(events_mutex_);
            events_ = events;
        }

    private:
        ReplayReaderEvents GetEventsSnapshot() const {
            std::lock_guard<std::mutex> lock(events_mutex_);
            return events_;
        }
    public:
        int32_t GetTotalFrames() const { return SerializerPolicy::GetTotalFrames(footer_); }
        const std::vector<ChunkIndexEntry>& GetSeekTable() const { return chunk_index_table_; }
        const SchemaType& GetPropertySchema() const { return SerializerPolicy::GetSchema(header_); }

        void SetCacheWindow(uint32_t backward, uint32_t forward) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_backward_ = backward;
            cache_forward_ = forward;
            current_range_start_ = -1;
        }

        // §3.A -- explicit prefetch hint.
        //
        // Tell the reader the caller is about to access `frame_index`.
        // Returns immediately: if the enclosing chunk is already cached
        // or in flight, this is a no-op; otherwise it kicks off an
        // asynchronous load.  The subsequent GetFrame(frame_index) will
        // either hit the cache (if the prefetch finished) or wait on
        // the in-flight future -- either way, no synchronous decompress
        // on the hot path.
        //
        // Intended use: commit a seek gesture with WarmAt(target_frame),
        // let the UI finish its animation, then call GetFrame.  Moves
        // the ZSTD decompress off the first-frame-after-seek critical
        // path and overlaps it with any teardown the caller is doing.
        //
        // Implementation note: reuses UpdateCacheWindow verbatim, which
        // means WarmAt also updates the §1.B access-pattern EWMA.  This
        // is correct -- from the reader's point of view, WarmAt is
        // indistinguishable from a "virtual" access, and the distance
        // it contributes to the EWMA reflects the real jump the caller
        // just committed to.
        void WarmAt(int32_t frame_index) {
            auto it = std::lower_bound(
                chunk_index_table_.begin(), chunk_index_table_.end(), frame_index,
                [](const ChunkIndexEntry& e, int32_t val) { return e.end_frame < val; });
            if (it == chunk_index_table_.end() || frame_index < it->start_frame) {
                return;
            }
            UpdateCacheWindow(it->chunk_index);
        }

        std::span<const std::byte> GetRawFrameBytes(int32_t frame_index) {
            auto it = std::lower_bound(chunk_index_table_.begin(), chunk_index_table_.end(), frame_index,
                [](const ChunkIndexEntry& e, int32_t val) { return e.end_frame < val; });

            if (it == chunk_index_table_.end() || frame_index < it->start_frame) {
                return {};
            }

            int32_t target_chunk = it->chunk_index;
            int32_t relative_idx = frame_index - it->start_frame;

            UpdateCacheWindow(target_chunk);

            std::shared_future<void> load_task;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (chunk_cache_.contains(target_chunk)) {
                    const auto& chunk = chunk_cache_[target_chunk];
                    if (relative_idx < chunk.raw_frames_spans.size()) {
                        return chunk.raw_frames_spans[relative_idx];
                    }
                    return {};
                }
                if (pending_loads_.contains(target_chunk)) {
                    load_task = pending_loads_[target_chunk].future;
                }
            }

            if (load_task.valid()) {
                load_task.wait();
            } else {
                LoadChunkToCacheSync(target_chunk);
            }

            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (chunk_cache_.contains(target_chunk)) {
                const auto& chunk = chunk_cache_[target_chunk];
                if (relative_idx < chunk.raw_frames_spans.size()) {
                    return chunk.raw_frames_spans[relative_idx];
                }
            }
            return {};
        }

        bool GetFrame(int32_t frame_index, VTX::Frame& out_frame) {
            auto it = std::lower_bound(chunk_index_table_.begin(), chunk_index_table_.end(), frame_index,
                [](const ChunkIndexEntry& e, int32_t val) { return e.end_frame < val; });

            if (it == chunk_index_table_.end() || frame_index < it->start_frame) return false;

            int32_t target_chunk = it->chunk_index;
            int32_t relative_idx = frame_index - it->start_frame;

            UpdateCacheWindow(target_chunk);

            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (chunk_cache_.contains(target_chunk)) {
                const auto& chunk = chunk_cache_[target_chunk];
                if (relative_idx >= 0 && relative_idx < chunk.native_frames.size()) {
                    out_frame = chunk.native_frames[relative_idx];
                    return true;
                }
            }

            return false;
        }

        void GetFrameRange(int32_t start_frame, int32_t range, std::vector<VTX::Frame>& out_frames) {
            out_frames.clear();
            out_frames.reserve(range + 1);
            for (int32_t i = 0; i <= range; ++i) {
                VTX::Frame frame;
                if (GetFrame(start_frame + i, frame)) {
                    out_frames.push_back(std::move(frame));
                }
            }
        }

        std::vector<VTX::Frame> GetFrameContext(int32_t center_frame, int32_t back_range, int32_t forward_range) {
            std::vector<VTX::Frame> context;
            const int32_t start = std::max(0, center_frame - back_range);
            const int32_t end = center_frame + forward_range;
            context.reserve(end - start + 1);

            for (int32_t i = start; i <= end; ++i) {
                VTX::Frame frame;
                if (GetFrame(i, frame)) {
                    context.push_back(std::move(frame));
                }
            }
            return context;
        }

        const VTX::Frame* GetFramePtr(int32_t frame_index) {
            auto it = std::lower_bound(chunk_index_table_.begin(), chunk_index_table_.end(), frame_index,
                [](const ChunkIndexEntry& e, int32_t val) { return e.end_frame < val; });

            if (it == chunk_index_table_.end() || frame_index < it->start_frame) return nullptr;

            int32_t target_chunk = it->chunk_index;
            int32_t relative_idx = frame_index - it->start_frame;

            UpdateCacheWindow(target_chunk);

            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (chunk_cache_.contains(target_chunk)) {
                const auto& chunk = chunk_cache_[target_chunk];
                if (relative_idx >= 0 && relative_idx < chunk.native_frames.size()) {
                    return &chunk.native_frames[relative_idx];
                }
            }

            return nullptr;
        }

        const VTX::Frame* GetFramePtrSync(int32_t frame_index) {
            auto it = std::lower_bound(chunk_index_table_.begin(), chunk_index_table_.end(), frame_index,
                [](const ChunkIndexEntry& e, int32_t val) { return e.end_frame < val; });

            if (it == chunk_index_table_.end() || frame_index < it->start_frame) {
                return nullptr;
            }

            int32_t target_chunk = it->chunk_index;
            int32_t relative_idx = frame_index - it->start_frame;

            UpdateCacheWindow(target_chunk);

            std::shared_future<void> load_task;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (chunk_cache_.contains(target_chunk)) {
                    const auto& chunk = chunk_cache_[target_chunk];
                    if (relative_idx >= 0 && relative_idx < chunk.native_frames.size()) {
                        return &chunk.native_frames[relative_idx];
                    }
                    return nullptr;
                }
                if (pending_loads_.contains(target_chunk)) {
                    load_task = pending_loads_[target_chunk].future;
                }
            }

            if (load_task.valid()) {
                load_task.wait();
            } else {
                LoadChunkToCacheSync(target_chunk);
            }

            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (chunk_cache_.contains(target_chunk)) {
                const auto& chunk = chunk_cache_[target_chunk];
                if (relative_idx >= 0 && relative_idx < chunk.native_frames.size()) {
                    return &chunk.native_frames[relative_idx];
                }
            }

            return nullptr;
        }

        int32_t GetChunkFrameCountSafe(int32_t chunk_index) {
             std::lock_guard<std::mutex> lock(cache_mutex_);
             if (chunk_cache_.contains(chunk_index)) return chunk_cache_[chunk_index].native_frames.size();
             return 0;
        }

        FrameAccessor CreateAccessor() const {
            FrameAccessor accessor;
            accessor.InitializeFromCache(property_address_cache_);
            return accessor;
        }

        void InspectChunkHeader(int32_t index) const {
            if (index < 0 || index >= chunk_index_table_.size()) return;
            const auto& entry = chunk_index_table_[index];
            VTX_INFO("--- Chunk Inspection --- Index: {} Offset: {} Size: {}", entry.chunk_index, entry.file_offset, entry.chunk_size_bytes);
        }

        VTX::FileHeader GetFileHeader()
        {
            return SerializerPolicy::GetVTXHeader(header_);
        }

        VTX::FileFooter GetFileFooter()
        {
            return SerializerPolicy::GetVTXFooter(footer_);
        }

        VTX::ContextualSchema GetContextualSchema()
        {
            return SerializerPolicy::GetVTXContextualSchema(header_);
        }



        VTX::PropertyAddressCache GetPropertyAddressCache()
        {
            return property_address_cache_;
        }

    private:
        struct CachedChunk {
            int32_t index;
            std::vector<VTX::Frame> native_frames; // Accessor data

            // ZERO-COPY DATA OWNER
            std::vector<uint8_t> decompressed_blob;
            std::vector<std::span<const std::byte>> raw_frames_spans; //pointing to decompressed_blob
        };

        // Returns the total size of the file behind `stream` (bytes).  Uses
        // seekg(end) + tellg; restores the stream position afterwards.  Used
        // for bounds validation in the corrupt-file paths below.
        static int64_t GetStreamSize(std::ifstream& stream) {
            const auto prev = stream.tellg();
            stream.seekg(0, std::ios::end);
            const auto end = stream.tellg();
            stream.seekg(prev);
            return (end < 0) ? int64_t{0} : static_cast<int64_t>(end);
        }

        void ReadHeader(std::ifstream& stream) {
            const int64_t file_size = GetStreamSize(stream);
            if (file_size < 8) {
                throw std::runtime_error("VTX file truncated: smaller than header preamble");
            }

            char magic[5] = {0};
            stream.read(magic, 4);
            if (stream.gcount() != 4) {
                throw std::runtime_error("VTX file truncated while reading magic bytes");
            }
            std::string actual_magic(magic);
            std::string expected_magic = SerializerPolicy::GetMagicBytes();

            if (actual_magic != expected_magic) {
                throw std::runtime_error("VTX INVALID MAGIC NUMBER.");
            }

            uint32_t size = 0;
            stream.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (stream.gcount() != sizeof(size)) {
                throw std::runtime_error("VTX file truncated while reading header size");
            }
            // Bound the declared header size by the remaining file size.
            const int64_t remaining = file_size - stream.tellg();
            if (size == 0 || static_cast<int64_t>(size) > remaining) {
                throw std::runtime_error("VTX file has implausible header size");
            }

            std::string buffer;
            buffer.resize(size);
            stream.read(buffer.data(), size);
            if (stream.gcount() != static_cast<std::streamsize>(size)) {
                throw std::runtime_error("VTX file truncated while reading header payload");
            }

            std::string final_buffer = ReplayUnpacker::Decompress(buffer);

            header_ = SerializerPolicy::ParseHeader(final_buffer);
            VTX::SchemaAdapter<SchemaType>::BuildCache(GetPropertySchema(), property_address_cache_);
        }

        void ReadFooter(std::ifstream& stream) {
            // Fix A1: the original code declared `uint32_t footer_size;` and
            // used it after `stream.read()` without checking `gcount()`.  If
            // the file is too short to contain the trailing 8-byte footer
            // size, we would seek to a garbage offset and crash.
            const int64_t file_size = GetStreamSize(stream);
            if (file_size < 8) {
                throw std::runtime_error("VTX file too small to contain footer");
            }

            stream.seekg(-8, std::ios::end);
            uint32_t footer_size = 0;
            stream.read(reinterpret_cast<char*>(&footer_size), sizeof(footer_size));
            if (stream.gcount() != sizeof(footer_size) || stream.fail()) {
                throw std::runtime_error("VTX file truncated while reading footer size");
            }

            // Fix A3: guard against an implausible footer_size (either
            // malicious UINT32_MAX or garbage from a corrupt trailer) that
            // would seek before the beginning of the file.
            if (footer_size == 0 ||
                static_cast<int64_t>(footer_size) + 8 > file_size) {
                throw std::runtime_error("VTX file has implausible footer size");
            }

            stream.seekg(-static_cast<int64_t>(footer_size + 8), std::ios::end);
            std::string buffer(footer_size, ' ');
            stream.read(buffer.data(), footer_size);
            if (stream.gcount() != static_cast<std::streamsize>(footer_size)) {
                throw std::runtime_error("VTX file truncated while reading footer payload");
            }

            std::string final_buffer = ReplayUnpacker::Decompress(buffer);

            footer_ = SerializerPolicy::ParseFooter(final_buffer);

            SerializerPolicy::PopulateIndexTable(footer_, chunk_index_table_);
            SerializerPolicy::PopulateGameTimes(footer_, game_times_);
        }

        void UpdateCacheWindow(int32_t current_idx) {
            // Orphaned PendingLoads moved out of pending_loads_ by trigger()
            // when it respawns a stale-cancelled entry.  Declared BEFORE the
            // lock_guard so the vector -- and therefore each orphan's
            // std::shared_future destructor -- runs AFTER cache_mutex_ has
            // been released on function exit.
            //
            // libstdc++'s std::shared_future dtor blocks until the async
            // task completes when it holds the last reference to a state
            // created by std::async(std::launch::async, ...).  The task
            // itself acquires cache_mutex_ in AsyncLoadTask to check
            // stop_requested() and (optionally) write chunk_cache_, so
            // destroying a to-be-cancelled shared_future WHILE holding
            // cache_mutex_ would deadlock: we would wait on a task that
            // cannot make progress without the mutex we hold.
            //
            // By moving the stale entry into orphans[] before overwriting
            // it in pending_loads_, we defer the shared_future dtor until
            // the lock_guard below has released the mutex.  The task can
            // then observe its already-requested stop, bail at the entry
            // check in PerformHeavyLoading, skip the cache write, and
            // resolve its future -- unblocking the orphans[] destructor.
            std::vector<PendingLoad> orphans;

            std::lock_guard<std::mutex> lock(cache_mutex_);

            // §1.B -- access-pattern detection.
            //
            // Maintain an EWMA of |current - last_requested| chunk
            // distance.  Small values mean the caller is reading
            // sequentially (playback, or a short scrub); large values
            // mean random access (editor seeks, scripted frame picking).
            //
            // We only benefit from lateral prefetches when the next
            // access is likely to land inside the current window -- i.e.
            // when the expected distance is <= window radius.  Under
            // random access the [start..end] prefetch loop below would
            // spawn N loads that get cancelled on the very next seek,
            // pure waste that also steals CPU and cache_mutex_ from the
            // synchronous load the caller is actually waiting on.
            //
            // α = 0.3 gives ~3-sample reaction time to a regime change
            // (spike of 50 chunk distance becomes EWMA=15 after one
            // sample, 25.5 after two, 30.9 after three; well above a
            // typical cache_window of 2-10).  The decision threshold is
            // just the window size: if the typical jump exceeds what the
            // window can cover, the window can't help at all.
            bool do_lateral_prefetch = true;
            const int32_t window_size = static_cast<int32_t>(cache_backward_ + cache_forward_);
            if (last_requested_chunk_ >= 0) {
                const int32_t dist = std::abs(current_idx - last_requested_chunk_);
                constexpr float alpha = 0.3f;
                ewma_chunk_distance_ =
                    alpha * static_cast<float>(dist) +
                    (1.0f - alpha) * ewma_chunk_distance_;
                if (window_size > 0 &&
                    ewma_chunk_distance_ > static_cast<float>(window_size)) {
                    do_lateral_prefetch = false;
                }
            }
            last_requested_chunk_ = current_idx;

            // Reap prefetches that finished naturally since the last call.
            for (auto it = pending_loads_.begin(); it != pending_loads_.end(); ) {
                if (it->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    it = pending_loads_.erase(it);
                } else {
                    ++it;
                }
            }

            int32_t start = std::max(0, current_idx - static_cast<int32_t>(cache_backward_));
            int32_t end = std::min(static_cast<int32_t>(chunk_index_table_.size()) - 1, current_idx + (int32_t)cache_forward_);

            if (start == current_range_start_ && end == current_range_end_) return;
            current_range_start_ = start; current_range_end_ = end;

            // Cancel in-flight prefetches whose target chunk fell outside
            // the new window.  AsyncLoadTask checks `stop_requested()` at
            // several points (entry, pre-decompression, and inside the
            // per-frame deserialisation loop in ProcessChunkData); any
            // unavoidable work already done is sunk cost, but the remaining
            // work -- frame deserialisation and the final
            // cache_mutex_-guarded write to chunk_cache_ -- is skipped.
            //
            // Kept in pending_loads_ until the worker thread observes the
            // stop and the future transitions to ready; the next
            // UpdateCacheWindow() call reaps it at the top of this method.
            // This is the fix for the cache-sweep pathology where small
            // symmetric windows were ~4-8x slower than either (0,0) or
            // (>=chunk_count) because every random jump triggered a storm
            // of prefetches that then thrashed eviction.
            for (auto& kv : pending_loads_) {
                if (kv.first < start || kv.first > end) {
                    kv.second.stop.request_stop();
                }
            }

            // Fix A4: snapshot events_ under its mutex, then use the local
            // copy for the rest of this function.  Prevents std::function UB
            // from a concurrent SetEvents() and avoids holding events_mutex_
            // while firing user callbacks.
            const auto evts = GetEventsSnapshot();

            for (auto it = chunk_cache_.begin(); it != chunk_cache_.end(); ) {
                if (it->first < start || it->first > end) {
                    if (evts.OnChunkEvicted) evts.OnChunkEvicted(it->first);
                    it = chunk_cache_.erase(it);
                } else {
                    ++it;
                }
            }

            const size_t max_concurrent_loads = 3;

            auto trigger = [&](int32_t i) {
                if (chunk_cache_.contains(i)) return;

                // A prefetch that was cancelled by a previous window shift
                // (line ~509) leaves its entry in pending_loads_ with the
                // stop_token already requested.  If the chunk re-enters the
                // window before the worker thread has even started running
                // (easy under TSan's scheduling overhead, or any burst of
                // random seeks), the worker will observe stop_requested()
                // on entry and bail without populating chunk_cache_.  The
                // future resolves cleanly, but GetFramePtrSync() then reads
                // an empty cache and returns nullptr -- a latent bug from
                // the §1.A cancellation work (PR #4).
                //
                // Detect that case here and replace the stale entry with a
                // fresh PendingLoad.  The orphaned task will exit on its
                // own; its cache write is gated by a stop_requested() check
                // inside cache_mutex_ (see AsyncLoadTask) so it cannot
                // pollute the cache with empty data either.
                const bool stale = pending_loads_.contains(i) &&
                                   pending_loads_[i].stop.get_token().stop_requested();

                if (pending_loads_.contains(i) && !stale) return;

                // The concurrency cap guards *new* slots.  Respawning a
                // stale entry replaces an existing slot rather than adding
                // one, so don't gate the resurrection path on the cap --
                // otherwise we could refuse to revive a chunk the caller is
                // about to synchronously wait on.
                if (!stale && pending_loads_.size() >= max_concurrent_loads) return;

                if (evts.OnChunkLoadStarted) evts.OnChunkLoadStarted(i);

                // Relocate the stale entry to orphans[] before overwriting.
                // See the long comment at the top of UpdateCacheWindow for
                // why destroying its shared_future inside cache_mutex_
                // would deadlock under TSan.
                if (stale) {
                    orphans.push_back(std::move(pending_loads_[i]));
                }

                // Give each prefetch its own stop_source so we can
                // cancel it independently from the "window changed"
                // path above.  The token is captured by value into the
                // lambda -- it's ref-counted, cheap to copy, and stays
                // valid even if the map entry is later erased while the
                // worker thread is still running (it won't be, because
                // we wait on the future first, but the invariant is
                // useful for reasoning about shutdown paths).
                PendingLoad pl;
                auto token = pl.stop.get_token();
                pl.future = std::async(std::launch::async, [this, i, token]() {
                    this->AsyncLoadTask(i, token);
                }).share();
                pending_loads_[i] = std::move(pl);  // overwrites moved-from entry if stale
            };

            trigger(current_idx);
            if (do_lateral_prefetch) {
                for (int32_t i = start; i <= end; ++i) {
                    if (i != current_idx) trigger(i);
                }
            }
        }

        void LoadChunkToCacheSync(int32_t idx) {
            std::stop_token dummy;
            auto data = PerformHeavyLoading(idx, dummy);
            if (!data.native_frames.empty()) {
                // Snapshot events_ before taking cache_mutex_ so we don't
                // hold two locks at once, and so a concurrent SetEvents()
                // can't race with our callback read (A4).
                const auto evts = GetEventsSnapshot();
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    chunk_cache_[idx] = std::move(data);
                    pending_loads_.erase(idx);
                }
                if (evts.OnChunkLoadFinished) evts.OnChunkLoadFinished(idx);
            }
        }

        void AsyncLoadTask(int32_t idx, std::stop_token stop_token) {
            CachedChunk data;
            bool thread_survived = false;

            try {
                data = PerformHeavyLoading(idx, stop_token);
                thread_survived = true;
            } catch (...) {
                VTX_ERROR("[READER] Chunk {} thread crashed", idx);
            }

            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (!stop_token.stop_requested()) {
                    chunk_cache_[idx] = std::move(data);
                }
            }

            if (thread_survived) {
                // A4: snapshot before invoking.
                const auto evts = GetEventsSnapshot();
                if (evts.OnChunkLoadFinished) evts.OnChunkLoadFinished(idx);
            }
        }

        CachedChunk PerformHeavyLoading(int32_t idx, const std::stop_token& stop_token) {
            if (idx < 0 || idx >= chunk_index_table_.size()) return {};
            const auto& entry = chunk_index_table_[idx];

            if (stop_token.stop_requested()) return {};

            std::string compressed_blob;
            {
                std::ifstream local_stream(filepath_, std::ios::binary | std::ios::in);
                if (!local_stream.is_open()) return {};

                // Fix A3: validate that the chunk is fully contained in the
                // file before seeking.  A corrupt seek table can list offsets
                // past EOF; without this guard the subsequent read would
                // produce garbage that then crashes the deserialiser.
                const int64_t file_size = GetStreamSize(local_stream);
                const int64_t chunk_end =
                    static_cast<int64_t>(entry.file_offset) +
                    static_cast<int64_t>(entry.chunk_size_bytes);
                if (entry.chunk_size_bytes == 0 ||
                    static_cast<int64_t>(entry.file_offset) < 0 ||
                    chunk_end > file_size) {
                    VTX_ERROR("[READER] Chunk {} offset/size out of bounds (offset={}, size={}, file_size={})",
                              idx,
                              static_cast<int64_t>(entry.file_offset),
                              static_cast<int64_t>(entry.chunk_size_bytes),
                              file_size);
                    return {};
                }

                local_stream.seekg(entry.file_offset);
                std::string raw_buffer;
                raw_buffer.resize(entry.chunk_size_bytes);
                local_stream.read(raw_buffer.data(), entry.chunk_size_bytes);

                // Fix A2: detect partial reads via gcount().  A truncated
                // file between validation and read, or a filesystem error,
                // would otherwise leave `raw_buffer` half-filled with zeros
                // and feed that to the deserialiser.
                if (local_stream.gcount() != static_cast<std::streamsize>(entry.chunk_size_bytes)) {
                    VTX_ERROR("[READER] Chunk {} short read: expected {} bytes, got {}",
                              idx, entry.chunk_size_bytes,
                              static_cast<int64_t>(local_stream.gcount()));
                    return {};
                }

                if (raw_buffer.size() <= 4) return {};
                compressed_blob = raw_buffer.substr(4);
            }

            if (stop_token.stop_requested()) return {};

            try {
                CachedChunk cc;
                cc.index = idx;
                SerializerPolicy::ProcessChunkData(idx, compressed_blob, stop_token, cc.native_frames, cc.decompressed_blob, cc.raw_frames_spans);
                return cc;
            } catch (const std::exception& e) {
                VTX_ERROR("[READER] Chunk {} deserialization failed: {}", idx, e.what());
                return {};
            } catch (...) {
                VTX_ERROR("[READER] Chunk {} deserialization failed: unknown exception", idx);
                return {};
            }
        }

    private:
        std::string filepath_;

        HeaderType header_;
        FooterType footer_;
        std::vector<ChunkIndexEntry> chunk_index_table_;
        VTX::GameTime::VTXGameTimes game_times_;

        // A pending chunk load.  Each load gets its own `stop_source` so
        // `UpdateCacheWindow` can cancel prefetches whose target chunk has
        // fallen outside the current cache window without affecting
        // unrelated in-flight loads.  Pre-fix this was a single global
        // `stop_source_` that was only fired at reader destruction, which
        // meant every stale prefetch ran to completion and competed for
        // `cache_mutex_` + CPU with the synchronous load the user was
        // actually waiting on.
        struct PendingLoad {
            std::stop_source stop;
            std::shared_future<void> future;
        };

        std::map<int32_t, CachedChunk> chunk_cache_;
        std::map<int32_t, PendingLoad> pending_loads_;

        mutable std::mutex cache_mutex_;
        ReplayReaderEvents events_;
        mutable std::mutex events_mutex_;  // protects events_ (A4)

        uint32_t cache_backward_ = 2;
        uint32_t cache_forward_ = 2;

        int32_t current_range_start_ = -1;
        int32_t current_range_end_ = -1;

        // §1.B access-pattern detection state.  EWMA of chunk-index
        // distance between consecutive GetFrame/GetRawFrameBytes calls.
        // `last_requested_chunk_ == -1` bootstraps on the second call.
        // Not reset by SetCacheWindow(): the EWMA reflects the access
        // pattern (property of the caller) while the threshold reflects
        // the window (property of the config), so the decision adapts
        // naturally when the user reconfigures prefetch aggressiveness.
        int32_t last_requested_chunk_ = -1;
        float ewma_chunk_distance_ = 0.0f;

        PropertyAddressCache property_address_cache_={};
    };
}

