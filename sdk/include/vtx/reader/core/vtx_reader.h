#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <functional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

        // Fired exactly once, the first time chunk 0 is in RAM and
        // deserialised.  Pair with OnReadyFailed -- the reader fires one
        // or the other, never both, and both are single-shot for the
        // lifetime of the reader.
        //
        // Callback runs on the worker thread that finished the chunk
        // load (or on the caller's thread if the ready signal came from
        // the empty-replay vacuous path).  GUI consumers should marshal
        // to their UI thread.
        std::function<void()> OnReady;
        std::function<void(const std::string&)> OnReadyFailed;
    };

    template <IVtxReaderPolicy SerializerPolicy>
    class ReplayReader {
    public:
        using HeaderType = typename SerializerPolicy::HeaderType;
        using FooterType = typename SerializerPolicy::FooterType;
        using SchemaType = typename SerializerPolicy::SchemaType;

        explicit ReplayReader(const std::string& filepath)
            : filepath_(filepath) {
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
            // Unblock anyone waiting on WaitUntilReady().  If chunk 0
            // hasn't signalled ready/failed yet, flip to "failed" so
            // waiters observe a definite answer instead of hanging on
            // a reader that's being torn down.  No callback is fired
            // here: the caller is destroying the reader, so invoking
            // OnReadyFailed at this point would be invoked into a
            // potentially already-torn-down context.
            {
                std::lock_guard<std::mutex> lk(ready_mutex_);
                if (!ready_.load() && !ready_failed_.load()) {
                    ready_failed_.store(true);
                    ready_error_ = "Reader destroyed before first chunk was ready";
                }
            }
            ready_cv_.notify_all();

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
                    if (kv.second.future.valid())
                        tasks.push_back(kv.second.future);
                }
            }

            for (auto& task : tasks) {
                task.wait();
            }
        }

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
        // "Ready" == chunk 0 is decompressed and deserialised in RAM.
        // Separate from the more general `Loaded()` concept on
        // ReaderContext (which only asserts "reader object exists").
        // The reader fires exactly one of OnReady / OnReadyFailed the
        // first time chunk 0 resolves, and IsReady()/IsReadyFailed()
        // mirror that terminal state.
        bool IsReady() const { return ready_.load(std::memory_order_acquire); }
        bool IsReadyFailed() const { return ready_failed_.load(std::memory_order_acquire); }

        std::string GetReadyError() const {
            std::lock_guard<std::mutex> lk(ready_mutex_);
            return ready_error_;
        }

        // Blocks until the reader reports ready or failed.  Returns
        // IsReady() at unblock time, so a `false` return means either
        // "failed" or "destroyed before chunk 0 landed".  Check
        // IsReadyFailed()/GetReadyError() to disambiguate.
        bool WaitUntilReady() {
            std::unique_lock<std::mutex> lk(ready_mutex_);
            ready_cv_.wait(lk, [this] {
                return ready_.load(std::memory_order_acquire) ||
                       ready_failed_.load(std::memory_order_acquire);
            });
            return ready_.load(std::memory_order_acquire);
        }

        bool WaitUntilReady(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lk(ready_mutex_);
            ready_cv_.wait_for(lk, timeout, [this] {
                return ready_.load(std::memory_order_acquire) ||
                       ready_failed_.load(std::memory_order_acquire);
            });
            return ready_.load(std::memory_order_acquire);
        }

        // Facade escape hatch for the empty-replay case: a file with
        // zero chunks never triggers a chunk load, so the ready signal
        // would never fire organically.  OpenReplayFile() calls this
        // directly on such replays so waiters / pollers / callbacks get
        // a definite "ready" answer without synthetic work.
        void MarkReadyVacuous() { SignalFirstChunkReady(true, {}); }

        int32_t GetTotalFrames() const { return SerializerPolicy::GetTotalFrames(footer_); }
        const std::vector<ChunkIndexEntry>& GetSeekTable() const { return chunk_index_table_; }
        const SchemaType& GetPropertySchema() const { return SerializerPolicy::GetSchema(header_); }

        void SetCacheWindow(uint32_t backward, uint32_t forward) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_backward_ = backward;
            cache_forward_ = forward;
            current_range_start_ = -1;
        }

        void WarmAt(int32_t frame_index) {
            auto it = std::lower_bound(chunk_index_table_.begin(), chunk_index_table_.end(), frame_index,
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

            if (it == chunk_index_table_.end() || frame_index < it->start_frame)
                return false;

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

            if (it == chunk_index_table_.end() || frame_index < it->start_frame)
                return nullptr;

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
            if (chunk_cache_.contains(chunk_index))
                return chunk_cache_[chunk_index].native_frames.size();
            return 0;
        }

        FrameAccessor CreateAccessor() const {
            FrameAccessor accessor;
            accessor.InitializeFromCache(property_address_cache_);
            return accessor;
        }

        void InspectChunkHeader(int32_t index) const {
            if (index < 0 || index >= chunk_index_table_.size())
                return;
            const auto& entry = chunk_index_table_[index];
            VTX_INFO("--- Chunk Inspection --- Index: {} Offset: {} Size: {}", entry.chunk_index, entry.file_offset,
                     entry.chunk_size_bytes);
        }

        VTX::FileHeader GetFileHeader() { return SerializerPolicy::GetVTXHeader(header_); }

        VTX::FileFooter GetFileFooter() { return SerializerPolicy::GetVTXFooter(footer_); }

        VTX::ContextualSchema GetContextualSchema() { return SerializerPolicy::GetVTXContextualSchema(header_); }


        VTX::PropertyAddressCache GetPropertyAddressCache() { return property_address_cache_; }

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
            return (end < 0) ? int64_t {0} : static_cast<int64_t>(end);
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
            if (footer_size == 0 || static_cast<int64_t>(footer_size) + 8 > file_size) {
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
            std::vector<PendingLoad> orphans;

            std::lock_guard<std::mutex> lock(cache_mutex_);

            bool do_lateral_prefetch = true;
            const int32_t window_size = static_cast<int32_t>(cache_backward_ + cache_forward_);
            if (last_requested_chunk_ >= 0) {
                const int32_t dist = std::abs(current_idx - last_requested_chunk_);
                constexpr float alpha = 0.3f;
                ewma_chunk_distance_ = alpha * static_cast<float>(dist) + (1.0f - alpha) * ewma_chunk_distance_;
                if (window_size > 0 && ewma_chunk_distance_ > static_cast<float>(window_size)) {
                    do_lateral_prefetch = false;
                }
            }
            last_requested_chunk_ = current_idx;

            // Reap prefetches that finished naturally since the last call.
            for (auto it = pending_loads_.begin(); it != pending_loads_.end();) {
                if (it->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    it = pending_loads_.erase(it);
                } else {
                    ++it;
                }
            }

            int32_t start = std::max(0, current_idx - static_cast<int32_t>(cache_backward_));
            int32_t end =
                std::min(static_cast<int32_t>(chunk_index_table_.size()) - 1, current_idx + (int32_t)cache_forward_);

            if (start == current_range_start_ && end == current_range_end_)
                return;
            current_range_start_ = start;
            current_range_end_ = end;

            for (auto& kv : pending_loads_) {
                if (kv.first < start || kv.first > end) {
                    kv.second.stop.request_stop();
                }
            }


            const auto evts = GetEventsSnapshot();

            for (auto it = chunk_cache_.begin(); it != chunk_cache_.end();) {
                if (it->first < start || it->first > end) {
                    if (evts.OnChunkEvicted)
                        evts.OnChunkEvicted(it->first);
                    it = chunk_cache_.erase(it);
                } else {
                    ++it;
                }
            }

            const size_t max_concurrent_loads = 3;

            auto trigger = [&](int32_t i) {
                if (chunk_cache_.contains(i))
                    return;

                const bool stale = pending_loads_.contains(i) && pending_loads_[i].stop.get_token().stop_requested();

                if (pending_loads_.contains(i) && !stale)
                    return;


                if (!stale && pending_loads_.size() >= max_concurrent_loads)
                    return;

                if (evts.OnChunkLoadStarted)
                    evts.OnChunkLoadStarted(i);

                if (stale) {
                    orphans.push_back(std::move(pending_loads_[i]));
                }

                PendingLoad pl;
                auto token = pl.stop.get_token();
                pl.future =
                    std::async(std::launch::async, [this, i, token]() { this->AsyncLoadTask(i, token); }).share();
                pending_loads_[i] = std::move(pl); // overwrites moved-from entry if stale
            };

            trigger(current_idx);
            if (do_lateral_prefetch) {
                for (int32_t i = start; i <= end; ++i) {
                    if (i != current_idx)
                        trigger(i);
                }
            }
        }

        void LoadChunkToCacheSync(int32_t idx) {
            std::stop_token dummy;
            auto data = PerformHeavyLoading(idx, dummy);
            const bool success = !data.native_frames.empty();
            if (success) {
                // Snapshot events_ before taking cache_mutex_ so we don't
                // hold two locks at once, and so a concurrent SetEvents()
                // can't race with our callback read (A4).
                const auto evts = GetEventsSnapshot();
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    chunk_cache_[idx] = std::move(data);
                    pending_loads_.erase(idx);
                }
                if (evts.OnChunkLoadFinished)
                    evts.OnChunkLoadFinished(idx);
            }
            // §READY: fire the first-chunk signal if a sync caller
            // happened to be the one resolving chunk 0.  Single-shot
            // guard inside SignalFirstChunkReady makes this safe to
            // call alongside the async path.
            if (idx == 0) {
                SignalFirstChunkReady(success,
                                      success ? std::string {}
                                              : std::string {"Failed to load first chunk"});
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

            // Capture load outcome before moving `data` into the cache:
            // used for the §READY signal below and we can't check it
            // after the move.
            const bool load_succeeded = thread_survived && !data.native_frames.empty();

            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (!stop_token.stop_requested()) {
                    chunk_cache_[idx] = std::move(data);
                }
            }

            if (thread_survived) {
                // A4: snapshot before invoking.
                const auto evts = GetEventsSnapshot();
                if (evts.OnChunkLoadFinished)
                    evts.OnChunkLoadFinished(idx);
            }

            // §READY: only chunk 0 drives the ready signal.  Skip when
            // the task was cancelled: UpdateCacheWindow()'s trigger()
            // may have respawned a fresh task for the same chunk, and
            // the respawned task's outcome (not ours) is the
            // authoritative one.  SignalFirstChunkReady is single-shot
            // so calling it from multiple tasks is safe; the first one
            // to win the lock wins the signal.
            if (idx == 0 && !stop_token.stop_requested()) {
                SignalFirstChunkReady(load_succeeded,
                                      load_succeeded ? std::string {}
                                                     : std::string {"Failed to load first chunk"});
            }
        }

        // Flip the first-chunk-ready flag exactly once per reader.
        // Called from AsyncLoadTask / LoadChunkToCacheSync when chunk 0
        // resolves, and directly from MarkReadyVacuous() for the empty
        // replay case.  Idempotent: repeated calls after the first are
        // no-ops (the dtor also uses this pattern to flip to "failed"
        // if the reader is torn down before chunk 0 landed).
        //
        // Lock order: ready_mutex_ only.  Callbacks fire OUTSIDE the
        // lock so user handlers can safely re-enter reader APIs that
        // take other locks (cache_mutex_, events_mutex_).  Events are
        // snapshotted once under events_mutex_ via GetEventsSnapshot.
        void SignalFirstChunkReady(bool success, const std::string& error) {
            {
                std::lock_guard<std::mutex> lk(ready_mutex_);
                if (ready_.load(std::memory_order_acquire) ||
                    ready_failed_.load(std::memory_order_acquire)) {
                    return; // already signalled
                }
                if (success) {
                    ready_.store(true, std::memory_order_release);
                } else {
                    ready_failed_.store(true, std::memory_order_release);
                    ready_error_ = error;
                }
            }
            ready_cv_.notify_all();

            const auto evts = GetEventsSnapshot();
            if (success) {
                if (evts.OnReady)
                    evts.OnReady();
            } else {
                if (evts.OnReadyFailed)
                    evts.OnReadyFailed(error);
            }
        }

        CachedChunk PerformHeavyLoading(int32_t idx, const std::stop_token& stop_token) {
            if (idx < 0 || idx >= chunk_index_table_.size())
                return {};
            const auto& entry = chunk_index_table_[idx];

            if (stop_token.stop_requested())
                return {};

            std::string compressed_blob;
            {
                std::ifstream local_stream(filepath_, std::ios::binary | std::ios::in);
                if (!local_stream.is_open())
                    return {};

                const int64_t file_size = GetStreamSize(local_stream);
                const int64_t chunk_end =
                    static_cast<int64_t>(entry.file_offset) + static_cast<int64_t>(entry.chunk_size_bytes);
                if (entry.chunk_size_bytes == 0 || static_cast<int64_t>(entry.file_offset) < 0 ||
                    chunk_end > file_size) {
                    VTX_ERROR("[READER] Chunk {} offset/size out of bounds (offset={}, size={}, file_size={})", idx,
                              static_cast<int64_t>(entry.file_offset), static_cast<int64_t>(entry.chunk_size_bytes),
                              file_size);
                    return {};
                }

                local_stream.seekg(entry.file_offset);
                std::string raw_buffer;
                raw_buffer.resize(entry.chunk_size_bytes);
                local_stream.read(raw_buffer.data(), entry.chunk_size_bytes);

                if (local_stream.gcount() != static_cast<std::streamsize>(entry.chunk_size_bytes)) {
                    VTX_ERROR("[READER] Chunk {} short read: expected {} bytes, got {}", idx, entry.chunk_size_bytes,
                              static_cast<int64_t>(local_stream.gcount()));
                    return {};
                }

                if (raw_buffer.size() <= 4)
                    return {};
                compressed_blob = raw_buffer.substr(4);
            }

            if (stop_token.stop_requested())
                return {};

            try {
                CachedChunk cc;
                cc.index = idx;
                SerializerPolicy::ProcessChunkData(idx, compressed_blob, stop_token, cc.native_frames,
                                                   cc.decompressed_blob, cc.raw_frames_spans);
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

        struct PendingLoad {
            std::stop_source stop;
            std::shared_future<void> future;
        };

        std::map<int32_t, CachedChunk> chunk_cache_;
        std::map<int32_t, PendingLoad> pending_loads_;

        mutable std::mutex cache_mutex_;
        ReplayReaderEvents events_;
        mutable std::mutex events_mutex_; // protects events_ (A4)

        // §READY state: chunk-0 "ready" signalling.  `ready_` and
        // `ready_failed_` are mutually exclusive and both single-shot
        // for the reader's lifetime.  `ready_cv_` notifies waiters in
        // WaitUntilReady(); `ready_error_` carries the human-readable
        // reason when `ready_failed_` wins.
        std::atomic<bool> ready_ {false};
        std::atomic<bool> ready_failed_ {false};
        std::string ready_error_;
        mutable std::mutex ready_mutex_;
        mutable std::condition_variable ready_cv_;

        uint32_t cache_backward_ = 2;
        uint32_t cache_forward_ = 2;

        int32_t current_range_start_ = -1;
        int32_t current_range_end_ = -1;

        int32_t last_requested_chunk_ = -1;
        float ewma_chunk_distance_ = 0.0f;

        PropertyAddressCache property_address_cache_ = {};
    };
} // namespace VTX
