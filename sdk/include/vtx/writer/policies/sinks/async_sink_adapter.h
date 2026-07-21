/**
 * @file async_sink_adapter.h
 * @brief Sink decorator that moves chunk/journal I/O off the caller (game) thread.
 *
 * @details The synchronous writer runs its whole pipeline on the caller's thread, so the
 * frame that crosses a chunk boundary pays the full serialize + zstd + write + fsync bill
 * inline, and every journaled frame pays an fsync. `AsyncSinkAdapter<InnerSink>` cuts between
 * the writer core and the sink I/O: `JournalFrame` / `SaveChunk` / `Close` become enqueue
 * operations on a bounded FIFO, and a single dedicated I/O worker drains the queue in order,
 * calling the real `InnerSink` (a `ChunkedFileSink`). `JournalTiming` and `OnSessionStart`
 * stay synchronous -- session setup and the 'S' record must precede everything.
 *
 * Because one worker executes items strictly FIFO through the inner sink, the on-disk effect
 * sequence is exactly the synchronous one (data-before-journal, F-record contiguity,
 * append-only journal + compaction all hold by construction); the repair path needs zero
 * changes. What changes is DURABILITY LAG: a frame is crash-recoverable only once its item
 * (or containing group-commit batch) is durable. The lag is bounded by the queue capacity;
 * a hard kill loses that not-yet-durable suffix, and crash recovery still yields a clean
 * contiguous prefix. `Drain()` and `Stop()` are the zero-lag synchronization points.
 *
 * Backpressure is bounded by ITEM COUNT (`async_max_queue_frames`), not bytes: the only
 * per-frame size hook (`GetFrameSize`) returns 0 for the default FlatBuffers format, and
 * journal payload sizes are known only after worker-side serialization. On a full queue the
 * caller blocks (degrading toward synchronous behavior -- never dropping, never growing
 * memory without bound), except once a failure is latched.
 *
 * Group commit: the worker drains everything currently queued and, for a run of journal
 * frames, appends all their F records and issues ONE journal fsync for the batch. Chunk items
 * keep their own internal ordering (chunk write+fsync, then C/T commit+fsync). This amortizes
 * the per-frame fsync so a slow disk (HDD ~10-20 ms/fsync) can keep up with 60 fps instead of
 * degenerating to blocking.
 *
 * Failure protocol: on the first inner-sink I/O failure the worker latches an error, discards
 * the remaining queue, and closes the inner file + journal handles WITHOUT a footer (releasing
 * the deny-write share so the on-disk state becomes repair-ready immediately). Every blocking
 * wait -- a full-queue producer, `Drain()`, `Close()`/`Stop()`, the destructor -- is
 * failure-aware and returns promptly. The writer surfaces the latch as `SinkFailed` on the
 * next `TryRecordFrame` via the `HasFailed()` hook.
 *
 * Threading: `JournalTiming`/`OnSessionStart` run on the caller thread before the worker
 * starts; after that ONLY the worker thread touches `inner_`. The caller thread only enqueues
 * (and reads the atomic `HasFailed()` / mutex-guarded `GetLastError()`/`GetQueueDepth()`).
 *
 * @author Zenos Interactive
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "vtx/common/vtx_diagnostics.h"
#include "vtx/common/vtx_types.h"

namespace VTX {

    template <typename InnerSink>
    class AsyncSinkAdapter {
    public:
        // Forward the type surface the writer core reads off its sink policy.
        using SerializerPolicy = typename InnerSink::SerializerPolicy;
        using FrameType = typename InnerSink::FrameType;
        using SchemaType = typename InnerSink::SchemaType;
        using HeaderType = typename InnerSink::HeaderType;

        struct Config {
            typename InnerSink::Config inner;
            // Max queued items before an enqueue blocks the caller. 0 -> kFallbackQueueItems.
            // The facade resolves 0 to 2 * chunk_max_frames.
            size_t async_max_queue_frames = 0;
        };

        explicit AsyncSinkAdapter(Config config)
            : inner_(std::move(config.inner))
            , max_queue_items_(config.async_max_queue_frames == 0 ? kFallbackQueueItems
                                                                  : config.async_max_queue_frames) {}

        AsyncSinkAdapter(const AsyncSinkAdapter&) = delete;
        AsyncSinkAdapter& operator=(const AsyncSinkAdapter&) = delete;

        ~AsyncSinkAdapter() {
            // Close() joins the worker on a clean stop. If we get here with the worker still
            // running (no Close, e.g. an exception unwound the writer), shut it down: it drains
            // what is queued (making more data recoverable) then exits WITHOUT writing a footer,
            // so the recovery journal survives and the file is repairable. Never write a footer
            // here -- the caller never asked to finalize.
            if (worker_.joinable()) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    stop_requested_ = true;
                }
                not_empty_.notify_all();
                worker_.join();
            }
        }

        // --- Synchronous session setup (runs on the caller thread, before the worker) ---

        void JournalTiming(float fps, bool is_increasing) { inner_.JournalTiming(fps, is_increasing); }

        void OnSessionStart(const SchemaType& schema) {
            inner_.OnSessionStart(schema);
            // Whether the recovery journal is actually live is only known after OnSessionStart
            // (it may be disabled by config or fail to open). Cache it so JournalFrame can skip
            // enqueuing -- and copying -- frames when there is nowhere to journal them.
            journal_active_ = inner_.IsJournalActive();
            StartWorker();
        }

        // --- Enqueue path (runs on the caller thread) ---

        void JournalFrame(const FrameType& frame, int32_t frame_index, int64_t game_time, int64_t created_utc) {
            if (!journal_active_)
                return; // no dead copies when journaling is off/unavailable
            JournalItem item;
            item.frame = frame; // copy, exactly as the synchronous sink does
            item.index = frame_index;
            item.game_time = game_time;
            item.created_utc = created_utc;
            Enqueue(WorkItem {std::move(item)});
        }

        void SaveChunk(std::vector<std::unique_ptr<FrameType>>& frames, const std::vector<int64_t>& created_utc,
                       int32_t start_frame, int32_t total_frames) {
            ChunkItem item;
            item.frames = std::move(frames); // steal the batch; the writer clears the moved-from vector next
            item.batch_utc = created_utc;
            item.start_frame = start_frame;
            item.total_frames = total_frames;
            Enqueue(WorkItem {std::move(item)});
        }

        void Close(const SessionFooter& footer) {
            // SessionFooter is a non-owning view (four raw vector pointers into the caller's
            // timer). Deep-copy the vectors into the item NOW, while they are alive; the worker
            // rebuilds a fresh SessionFooter pointing into the item at inner-Close time.
            CloseItem item;
            item.total_frames = footer.total_frames;
            item.duration_seconds = footer.duration_seconds;
            if (footer.game_times)
                item.game_times = *footer.game_times;
            if (footer.created_utc)
                item.created_utc = *footer.created_utc;
            if (footer.gaps)
                item.gaps = *footer.gaps;
            if (footer.segments)
                item.segments = *footer.segments;
            // If the enqueue is dropped (a failure was already latched) the worker is already
            // exiting; either way, join it so Close() is a full durability barrier.
            Enqueue(WorkItem {std::move(item)});
            JoinWorker();
        }

        // --- Async-only surface (reached through the writer / facade passthroughs) ---

        // Cheap, lock-free check the writer polls at the top of TryRecordFrame.
        bool HasFailed() const noexcept { return failed_.load(std::memory_order_acquire); }

        // Block until every item enqueued before this call has been processed AND the journal's
        // group-commit batch is durable -- the durability barrier that Flush() no longer is under
        // async. Returns the latched error if the sink failed, else a default (None) error.
        VtxError Drain() {
            uint64_t id = 0;
            {
                std::unique_lock<std::mutex> lk(mu_);
                // No worker left to answer a barrier: after a clean close everything it held is
                // already durable, so there is nothing to wait for (and waiting would hang).
                if (failed_.load(std::memory_order_acquire) || worker_exited_)
                    return failed_.load(std::memory_order_acquire) ? last_error_ : VtxError {};
                not_full_.wait(lk, [this] {
                    return queue_.size() < max_queue_items_ || failed_.load(std::memory_order_acquire) ||
                           worker_exited_;
                });
                if (failed_.load(std::memory_order_acquire) || worker_exited_)
                    return failed_.load(std::memory_order_acquire) ? last_error_ : VtxError {};
                id = ++next_barrier_id_;
                queue_.push_back(WorkItem {BarrierItem {id}});
                not_empty_.notify_one();
            }
            std::unique_lock<std::mutex> lk(mu_);
            drained_.wait(lk, [this, id] {
                return last_barrier_done_ >= id || failed_.load(std::memory_order_acquire) || worker_exited_;
            });
            return failed_.load(std::memory_order_acquire) ? last_error_ : VtxError {};
        }

        VtxError GetLastError() const {
            std::lock_guard<std::mutex> lk(mu_);
            return last_error_;
        }

        size_t GetQueueDepth() const {
            std::lock_guard<std::mutex> lk(mu_);
            return queue_.size();
        }

    private:
        static constexpr size_t kFallbackQueueItems = 2000; // 2 * default chunk_max_frames

        struct JournalItem {
            FrameType frame;
            int32_t index = 0;
            int64_t game_time = 0;
            int64_t created_utc = 0;
        };
        struct ChunkItem {
            std::vector<std::unique_ptr<FrameType>> frames;
            std::vector<int64_t> batch_utc;
            int32_t start_frame = 0;
            int32_t total_frames = 0;
        };
        struct CloseItem {
            int32_t total_frames = 0;
            double duration_seconds = 0.0;
            std::vector<int64_t> game_times;
            std::vector<int64_t> created_utc;
            std::vector<int32_t> gaps;
            std::vector<int32_t> segments;
        };
        struct BarrierItem {
            uint64_t id = 0;
        };
        using WorkItem = std::variant<JournalItem, ChunkItem, CloseItem, BarrierItem>;

        static VtxError MakeError(VtxErrorCode code, std::string message) {
            VtxError e;
            e.code = code;
            e.severity = Severity::Error;
            e.message = std::move(message);
            e.source_api = "AsyncSinkAdapter";
            return e;
        }

        void StartWorker() {
            if (worker_started_)
                return;
            worker_started_ = true;
            worker_ = std::thread([this] {
                WorkerMain();
                // Publish the exit so no producer can block on a queue nobody will drain again
                // (see Enqueue/Drain): once the worker is gone, waiting for room is waiting
                // forever. Reached on every exit path -- clean close, stop, or latched failure.
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    worker_exited_ = true;
                }
                not_full_.notify_all();
                drained_.notify_all();
            });
        }

        void JoinWorker() {
            if (worker_.joinable())
                worker_.join();
        }

        // Blocks the caller when the queue is full (backpressure). Returns false without
        // enqueuing if a failure is latched (the recording is dead, so the item is dropped) or if
        // the worker has already exited -- e.g. an enqueue after Stop(), where waiting for room
        // would block forever because nothing drains the queue any more.
        bool Enqueue(WorkItem&& item) {
            std::unique_lock<std::mutex> lk(mu_);
            not_full_.wait(lk, [this] {
                return queue_.size() < max_queue_items_ || failed_.load(std::memory_order_acquire) || worker_exited_;
            });
            if (failed_.load(std::memory_order_acquire) || worker_exited_)
                return false;
            queue_.push_back(std::move(item));
            not_empty_.notify_one();
            return true;
        }

        // Run one inner-sink call, catching exceptions into the failure protocol. Returns false
        // if the call failed (already latched), telling the worker to stop the batch.
        template <typename Fn>
        bool SafeInner(Fn&& fn) {
            try {
                fn();
                return true;
            } catch (const std::exception& e) {
                LatchFailure(MakeError(VtxErrorCode::SinkFailed, std::string("async sink inner threw: ") + e.what()));
                return false;
            } catch (...) {
                LatchFailure(MakeError(VtxErrorCode::SinkFailed, "async sink inner threw unknown exception"));
                return false;
            }
        }

        // First-failure latch: store the error, discard the queue (writing more would commit
        // journal records for non-durable data), release the inner handles so the on-disk state
        // is repair-ready at once, and wake every waiter. Idempotent. Only the worker calls this,
        // so touching inner_ here is race-free.
        void LatchFailure(VtxError err) {
            const bool first = !failed_.exchange(true, std::memory_order_acq_rel);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (first)
                    last_error_ = std::move(err);
                queue_.clear();
            }
            try {
                inner_.AbortClose();
            } catch (...) {}
            not_full_.notify_all();
            not_empty_.notify_all();
            drained_.notify_all();
        }

        void WorkerMain() {
            for (;;) {
                std::deque<WorkItem> batch;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    not_empty_.wait(lk, [this] {
                        return !queue_.empty() || stop_requested_ || failed_.load(std::memory_order_acquire);
                    });
                    if (failed_.load(std::memory_order_acquire))
                        return; // failure protocol already ran; nothing left to do
                    if (queue_.empty() && stop_requested_)
                        return;         // graceful shutdown without a Close: nothing left to write
                    batch.swap(queue_); // take everything currently queued (the group-commit batch)
                }
                not_full_.notify_all(); // room freed; unblock producers to refill while we work

                bool journaled_unsynced = false; // F records appended this batch but not yet fsync'd
                bool did_close = false;
                uint64_t batch_barrier = 0;

                for (WorkItem& item : batch) {
                    if (failed_.load(std::memory_order_acquire))
                        break;
                    if (auto* j = std::get_if<JournalItem>(&item)) {
                        if (!SafeInner(
                                [&] { inner_.JournalFrameBatched(j->frame, j->index, j->game_time, j->created_utc); }))
                            break;
                        journaled_unsynced = true;
                    } else if (auto* c = std::get_if<ChunkItem>(&item)) {
                        if (!SafeInner(
                                [&] { inner_.SaveChunk(c->frames, c->batch_utc, c->start_frame, c->total_frames); }))
                            break;
                        journaled_unsynced = false; // CommitChunk already fsync'd the journal
                    } else if (auto* cl = std::get_if<CloseItem>(&item)) {
                        SessionFooter footer;
                        footer.total_frames = cl->total_frames;
                        footer.duration_seconds = cl->duration_seconds;
                        footer.game_times = &cl->game_times;
                        footer.created_utc = &cl->created_utc;
                        footer.gaps = &cl->gaps;
                        footer.segments = &cl->segments;
                        if (!SafeInner([&] { inner_.Close(footer); }))
                            break;
                        journaled_unsynced = false;
                        did_close = true;
                    } else if (auto* b = std::get_if<BarrierItem>(&item)) {
                        // A Drain() barrier: make everything appended so far durable before it
                        // reports done.
                        if (journaled_unsynced) {
                            if (!SafeInner([&] { inner_.SyncJournal(); }))
                                break;
                            journaled_unsynced = false;
                        }
                        batch_barrier = b->id;
                    }
                }

                // Group commit: one journal fsync for the batch's F records, unless a chunk /
                // close / barrier already synced them.
                if (journaled_unsynced && !failed_.load(std::memory_order_acquire))
                    SafeInner([&] { inner_.SyncJournal(); });

                // Primary failure detection: a short write / failed fsync latches the inner file's
                // Good() flag without throwing. Catch it here and run the failure protocol.
                if (!failed_.load(std::memory_order_acquire) && !inner_.Good())
                    LatchFailure(MakeError(VtxErrorCode::SinkFailed, "async sink: durable write to .vtx failed"));

                if (batch_barrier != 0) {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (batch_barrier > last_barrier_done_)
                        last_barrier_done_ = batch_barrier;
                    drained_.notify_all();
                }

                if (failed_.load(std::memory_order_acquire))
                    return; // LatchFailure closed the inner sink; worker is done
                if (did_close)
                    return; // clean finalize; worker's job is complete
            }
        }

        InnerSink inner_;
        const size_t max_queue_items_;

        mutable std::mutex mu_;
        std::condition_variable not_empty_; // worker waits for work
        std::condition_variable not_full_;  // producers wait for room
        std::condition_variable drained_;   // Drain() waits for its barrier
        std::deque<WorkItem> queue_;

        std::thread worker_;
        bool worker_started_ = false;
        bool stop_requested_ = false;
        bool worker_exited_ = false; ///< set by the worker on exit; guarded by mu_
        bool journal_active_ = false;

        std::atomic<bool> failed_ {false};
        VtxError last_error_;

        uint64_t next_barrier_id_ = 0;
        uint64_t last_barrier_done_ = 0;
    };

} // namespace VTX
