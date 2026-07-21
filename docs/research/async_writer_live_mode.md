# R&D: "Live" async writer — recording while chunks flush to disk

Status: IMPLEMENTED (Option A) — design v2, revised after a 3-lens adversarial review
(17 confirmed findings incorporated; see §7). Landed on branch `feature/async-sink-adapter`:
`AsyncSinkAdapter<InnerSink>` (async_sink_adapter.h), facade `async_io` knobs,
`Drain()`/`GetLastError()`/`GetQueueDepth()`, group-commit journal primitives, the
`SinkFailed` surfacing hook, and `tests/writer/test_async_sink.cpp`.

VERIFIED: the full test plan of §5 is implemented and green, including the real-
`TerminateProcess` async-ON kill matrix (§5.3) in `test_crash_recovery.cpp`. Race freedom
was proven under ThreadSanitizer (gcc-13/WSL, `VTX_SANITIZE=thread`): 95 writer/crash tests
clean, and the harness itself was validated by confirming TSan reports a deliberately
injected race in the adapter. One hardening fix came out of the audit: an enqueue after the
worker had exited could block forever on a full queue (see `worker_exited_`).

Remaining follow-ups (not blocking): the async-aware per-`RecordFrame` latency benchmark
(§5.6); the network-sink decorator (§4.7); and the pre-existing inert `max_bytes`
FlatBuffers chunking fix (§8), all filed separately.
Author: Zenos Interactive.

## 1. Problem

The writer is fully synchronous. Every `RecordFrame` call runs the entire pipeline on
the calle
r's thread, and the frame that crosses a chunk boundary pays the whole chunk
bill inline (serialize N frames + zstd + write + fsync + journal commit). Measured on
NVMe (`BM_WriterDurabilityTier`, 200 small frames):

| caller-thread cost            | per frame  |
|-------------------------------|-----------|
| no journal                    | ~19 us    |
| journal, flush-only           | ~30 us    |
| journal, fsync (default)      | ~290 us   |
| boundary frame (chunk flush)  | + several ms to tens of ms |

Goal: **accepting new frames must not wait for chunk/journal I/O.** Durability and
crash-recovery guarantees must degrade in a bounded, documented way only.

## 2. Current pipeline (what exactly blocks)

Per `TryRecordFrame` (writer.h), all on the caller thread, in order:

1. Timer snapshot + `AddTimeRegistry` + `ResolveGameTimes`  — cheap, **stateful, ordered**
2. Post-processor `Process`                                  — user code, mutates frame
3. `NormalizeBucketsToSchema` + `FinalizeFrame` (content_hash) — CPU, moderate
4. `Serializer::FromNative`                                  — CPU, moderate
5. `sink_.JournalFrame`: copy frame -> 1-frame `SerializeChunk` -> zstd -> write -> **fsync**
6. On boundary, before 5: `Flush()` -> `SaveChunk`: `SerializeChunk(N)` -> zstd -> write -> **fsync** -> journal C/T commit -> **fsync**

Steps 5-6 are ~95%+ of the cost. Steps 1-4 are ~10-15 us.

## 3. Design options

### Option A — Async SINK decorator (recommended)

Cut between the writer core (steps 1-4) and the sink I/O (steps 5-6). A new sink
policy `AsyncSinkAdapter<InnerSink>` decorates `ChunkedFileSink`:

- `JournalFrame` / `SaveChunk` / `Close` become **enqueue** operations on a bounded
  FIFO; a single dedicated I/O worker drains the queue in order, calling the inner
  sink's real implementations.
- `OnSessionStart` and `JournalTiming` stay **synchronous** (session setup completes
  before recording; the 'S' record must precede everything — queuing JournalTiming
  would race the first frames).

**Why it wins over B/C:** synchronous accept/reject preserved; post-processor keeps
its documented caller-thread affinity; `GetLastFinalizedFrame`/`FindEntity` intact;
ordering trivially preserved (one FIFO, one worker); zero writer-core redesign.

### Option B — Full producer/consumer of native frames

Worker runs the entire writer. Saves ~14 us/frame over A (0.09% of a 60 fps budget)
at the cost of async rejections, post-processor thread hazards, and cross-thread
snapshot APIs. **Rejected as an SDK feature; layerable app-side on top of A.**

### Option C — Validate sync, defer the rest

Inherits B's post-processor problems for ~0 win over A. Rejected.

## 4. Detailed design (Option A, revised)

### 4.1 Work items and queue

```
struct JournalItem { FrameType frame_copy; int32 index; int64 gt, cu; };
struct ChunkItem   { vector<unique_ptr<FrameType>> frames;   // MOVED from pending_frames_
                     vector<int64> batch_utc; int32 start, total; };
struct CloseItem   { int32 total_frames; double duration_seconds;
                     vector<int64> game_times, created_utc;   // OWNED plain values
                     vector<int32> gaps, segments; };
```

- **`SessionFooter` is a non-owning view type (four raw vector pointers) and must
  never be stored in or moved through the queue.** `Close` deep-copies the four
  vectors into the `CloseItem` as owned members; the worker constructs a fresh
  `SessionFooter` at inner-`Close` call time, pointing into the item it is currently
  processing (address-stable for the duration of the call).
- `SaveChunk` moves the frames vector into the item (no copy; the writer's
  subsequent `pending_frames_.clear()` is a no-op on moved-out storage).
- `JournalFrame` copies the FrameType (parity with today's sync sink, which already
  copies). **If journaling is disabled or its open failed, the adapter enqueues
  nothing** (it knows `enable_recovery_journal` and the inner journal state at
  session start) — no dead copies.
- Queue: `std::mutex` + `condition_variable`, SPSC in practice. All waits are
  failure-aware (§4.4).

### 4.2 Ordering & crash-recovery invariants

The worker executes items strictly FIFO via the inner sink, so the on-disk effect
sequence is exactly today's: data-before-journal, F-record contiguity, append-only
journal + compaction all hold by construction; `RepairReplayFile` needs zero changes.

**What changes — durability lag, stated honestly:** a frame is crash-recoverable
only once its JournalItem (or containing batch, §4.8) is durable. The lag is bounded
by the queue capacity (§4.3), NOT "a few ms": on a saturated slow disk the queue can
hold up to `async_max_queue_frames` accepted-but-not-yet-durable frames. A hard kill
loses that suffix; contiguity still yields a clean recovered prefix. `Stop()` and
`Drain()` are the zero-lag synchronization points.

### 4.3 Backpressure

- **Bound by ITEM COUNT, not bytes**: `async_max_queue_frames` (default
  `2 * chunk_max_frames`). Byte-accounting is not implementable today —
  `FlatBuffersVtxPolicy::GetFrameSize()` returns 0 (see §8, pre-existing bug), and
  journal payload sizes are only known after worker-side serialization.
- Policy on full: **block the caller** (degrade toward sync behavior; never drop,
  never unbounded memory) — except on latched failure (§4.4).
- **Oversized-item rule**: a ChunkItem always counts as one item and is admitted
  whenever the queue is not full; item-count bounding makes the
  "single item larger than the cap" deadlock of byte-bounding structurally
  impossible.
- Telemetry: `GetQueueDepth()`; peak-memory formula documented in §6.

### 4.4 Failure protocol (fully specified)

On the first inner-sink I/O failure (DurableFile `Good()` latch / exception):

1. Worker sets `atomic<bool> failed_` + stores a `VtxError`, then **notify_all**.
2. Worker **halts consumption and discards the remaining queue** (the recording is
   dead; writing more would commit journal records for non-durable data).
3. Worker **closes the inner sink's files and journal handles** — this releases the
   deny-write handles so the on-disk state (valid journal + data up to the failure
   point) becomes repair-ready immediately, even while the app keeps running.
4. **Every blocking wait is failure-aware**: a producer blocked on a full queue, a
   `Drain()`, a `Stop()`, or the destructor wakes on `failed_` and returns promptly
   (Stop/Drain report the stored error; the blocked enqueue drops its item).
   No wait predicate can hang once the worker stops consuming.
5. Surfacing to the caller: the writer core gains one cheap hook — at the top of
   `TryRecordFrame`, `if constexpr (sink has HasFailed())` and it returns true,
   return `MadeRejected(VtxErrorCode::SinkFailed, ...)`. (Detection idiom, no
   interface change for existing sinks.) `PipelineReport::Account` classifies
   `SinkFailed` as its own counter — not `validation_errors`.
6. Facade exposes `GetLastError()`.

### 4.5 Lifecycle

- `Close(footer)`: enqueue CloseItem, worker drains, inner `Close` writes footer +
  deletes journal, worker exits. `Stop()` blocks until fully durable (or returns
  the latched error promptly per §4.4).
- Destructor without Stop: drain queue (more data becomes recoverable), close files
  without footer, join worker — failure-aware, cannot hang.
- **`Flush()` semantics CHANGE under async (documented, opt-in flag):** it still
  closes the current chunk but now only *enqueues* the write — it is no longer a
  durability barrier. The barrier is the new `Drain()`. This must be called out in
  SDK_API.md's async section explicitly.

### 4.6 Config surface

```
bool   async_io                = false;                  // opt-in v1
size_t async_max_queue_frames  = 2 * chunk_max_frames;
bool   durable_writes          = true;                   // exposed at facade at last
bool   enable_recovery_journal = true;
```

Facade constructs `AsyncSinkAdapter<ChunkedFileSink<P>>` vs `ChunkedFileSink<P>`
(two `WriterFacadeImpl` instantiations, runtime-selected).

### 4.7 Network sink

Same decorator applies later (stalled TCP peer stops hitching the game). Out of
scope v1.

### 4.8 Group commit (required, not optional — makes slow disks close)

Without batching, the worker inherits fsync-per-frame: on an HDD (~10-20 ms/fsync)
that is 50-100 frames/s < 60 fps — the queue fills and async degenerates to
blocking. Therefore the worker uses **group commit**: it drains everything currently
queued, serializes each JournalItem, appends ALL of them to the journal, and issues
ONE `SyncOrFlush` for the batch (ChunkItems keep their own internal ordering:
chunk-write+fsync, then C/T commit+fsync, as today). Recovery semantics are
unchanged — a crash lands on a batch boundary, still a clean contiguous prefix.
Amortized fsync cost makes HDD steady-state close with large margin, and NVMe lag
drop to sub-ms.

## 5. Test plan (revised)

1. **Order equivalence**: instrumented fake inner sink; async call sequence ==
   sync call sequence (batching allowed to coalesce J-syncs only).
2. **Byte equivalence**: same inputs -> async file == sync file bit-for-bit
   (same-second pair technique). This also pins the CloseItem/SessionFooter
   reconstruction (§4.1) — a dangling/garbage footer cannot pass it.
3. **Crash**: real TerminateProcess kills with async ON (both durability modes,
   compaction cadence 1): recovered file = clean contiguous prefix, exact times.
4. **Backpressure**: queue cap 2 + artificially slow inner sink -> caller blocks,
   zero loss, order preserved; then latch a failure while a producer is blocked ->
   producer unblocks promptly with SinkFailed; Stop() returns the error; files are
   repair-ready (deny-write released) while the process still lives.
5. **Error injection**: failing inner sink mid-session -> journal repairable,
   RepairReplayFile succeeds on the prefix.
6. **Benchmark**: new async-aware benchmark measuring per-`RecordFrame` latency
   histograms EXCLUDING Stop() (manual timers inside the loop, p50/p99/max),
   sync vs async, all durability tiers. (`BM_WriterDurabilityTier` as-is includes
   Stop() drain in its per-iteration time and cannot show the difference.)
7. Existing suite green with async OFF; a representative async-ON variant of the
   crash-recovery E2E tests (the fabrication-based tests are sink-agnostic and
   don't need variants; the raw-writer E2E ones parametrize on sink type).

## 6. Memory model (documented)

Peak native-frame memory ~= `pending_frames_` (up to chunk_max_frames frames)
+ queued JournalItem copies (up to async_max_queue_frames) + one moved ChunkItem
in flight — i.e. up to ~3x a chunk of frames with defaults. The v1.5 optimization
(pending_frames_ as `shared_ptr<FrameType>`, journal items share instead of copy)
removes the copy term entirely and is the planned follow-up, not a blocker.

## 7. Adversarial review disposition (v1 -> v2)

17 confirmed findings, all incorporated: CloseItem/SessionFooter dangling-view
redesign (4.1); JournalTiming kept synchronous (4.1); no dead copies when journaling
is off (4.1); honest durability-lag statement (4.2); item-count bound replacing the
unenforceable byte bound + oversized-item deadlock removal (4.3); fully specified
failure protocol with failure-aware waits, queue discard, early handle release, the
TryRecordFrame surfacing hook and PipelineReport classification (4.4); Flush()
semantic change called out (4.5); group commit as a requirement with HDD math
(4.8); benchmark methodology fix + failure/backpressure tests (5); peak-memory
formula (6).

## 8. Collateral pre-existing findings (filed separately, not part of this design)

- **`FlatBuffersVtxPolicy::GetFrameSize()` returns 0** (flatbuffers_vtx_policy.cpp)
  -> `ThresholdChunkPolicy::max_bytes` is INERT for FlatBuffers recordings: chunks
  split only by max_frames, so a byte budget is silently ignored. Needs a native-
  frame size estimator or serialized-size feedback.
- Consequently `ByteBudgetChunkingCrashRecovery` passes for the wrong reason on the
  chunk-count assertion (the >=3 chunks come from pending-frame recovery, not byte
  splitting). Test needs its assertion corrected alongside the fix.
