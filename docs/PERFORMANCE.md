# Performance

> Numbers in this page come from the benchmark run on **2026-04-23** against `main` at commit `78b20f3`.
> Raw data for that run (re-usable for graphing or regression tracking): [`docs/benchmarks/bench_20260423_162008.json`](benchmarks/bench_20260423_162008.json).

## At a glance

A one-screen view of which areas of the SDK are fast, which are slow, and which depend on how you use them.

| Area | Verdict | Why |
|---|---|---|
| Writer throughput | 🟢 Fast | 82 k frames/s end-to-end; ~10× headroom over real-time recording. |
| Full sequential read (CS, 92 MB) | 🟢 Fast enough | 5.6 s median; competitive with peer tooling. |
| Preview (first 1 000 frames) | 🟢 Fast | ~1 s; below UX perceptibility. |
| Scrubbing with **well-sized** cache | 🟢 Fast | <4 s for 50 random jumps. |
| Scrubbing with **mis-sized** cache | 🔴 **Actively bad** | 23 s for the same workload — 59 % slower than no cache. |
| `FrameAccessor` creation | 🟢 Negligible | 6.77 µs once per replay. |
| `PropertyKey` resolution | 🟢 Negligible | 74 ns per key × keys_you_resolve. |
| `EntityView` hot-loop read | 🟢 Excellent | 1.70 ns isolated / ~80 ns in realistic loop. |
| Diff on consecutive frames | 🟢 Instant | 4 µs; 267 k comparisons/s. |
| Diff short-circuit on identical frames | 🟢 Instant | xxHash shortcut working; 10× on arena, 2× on CS. |
| Schema parse | 🟢 Negligible | 200 µs, one-time per file. |
| Format choice (FBS vs Proto) | 🟡 Context-dependent | No universal winner; fixture-shape dictates. |
| CS sequential-scan stability | 🟡 Noisy | One outlier iter per run; median trustworthy, mean fluctuates. |

## Headline numbers

| Task | Result | Plain-language reading |
|---|---|---|
| Writing a replay | ~82 000 frames/s | A 30-minute match at 60 fps (~108 k frames) costs ~1.3 s of CPU. |
| Reading the full CS2 fixture (92 MB, median) | ~5.6 s | Comparable to opening a long video in an editor. |
| Preview (first 1 000 frames) | ~1 s | Thumbnails + file-browse UI have no perceptible lag. |
| Seek to 50 % + play 300 frames | ~0.9 s | Timeline scrubbing is fluid. |
| `FrameAccessor` creation | 6.77 µs | Once per replay. Invisible. |
| `PropertyKey` resolution | ~74 ns / key | 10 properties ≈ 0.7 µs total. Invisible. |
| `EntityView` read (isolated) | **1.70 ns** | 585 M reads/s — theoretical ceiling. |
| `EntityView` read (hot loop) | ~80 ns | ~13 M reads/s — what real integrations observe. |
| Frame diff | 4 µs | 267 k comparisons/s. |

## The three-layer accessor API

Customers asking "how fast is the property-access API?" are really asking about one of three layers with very different costs:

| Layer | When you pay | How often | Cost |
|---|---|---|---|
| `FrameAccessor` (`reader->CreateAccessor()`) | Integration startup | Once per replay | **6.77 µs** |
| `PropertyKey<T>` (`accessor.Get<T>("Struct", "prop")`) | Integration setup | Once per property you care about | **~74 ns / key** |
| `EntityView` + `Get` (inside your hot loop) | Every property read | Millions of times per second | **1.70 ns isolated / ~80 ns realistic** |

Lead with the hot-loop number (~13 M reads/s) when setting customer expectations. Use the isolated ceiling (585 M/s) only when explicitly asked about the theoretical upper bound.

## Key findings

### 1. A small cache is *worse* than no cache

The reader has a configurable cache window (`SetCacheWindow(back, forward)`) that keeps recently-read chunks in RAM. Intuition says "bigger is better, and a small cache is still better than nothing". **This is false for random-access workloads.**

Measured on the CS2 fixture (50 random jumps):

| Cache window | Wall time | vs. no cache |
|---:|---:|---|
| 0 (disabled) | 14.56 s | baseline |
| 2 | **23.20 s** | **+59 % slower** |
| 5 | 16.87 s | +16 % slower |
| 10 | 3.27 s | **4.5× faster** |
| 20 | 3.09 s | plateau (fixture fits in 10) |

**Why.** A too-small window evicts the chunks that were about to be reused. Every jump pays the eviction cost *plus* the reload cost. When the window finally fits the working set, throughput jumps 5×.

**Guidance.** Size `SetCacheWindow` to the expected scrubbing span. Don't pick a small default and hope for the best.

### 2. No universal "faster" format

VTX supports both FlatBuffers and Protobuf. Which is faster depends on the replay:

| Fixture | FBS median | Proto median | Winner | Margin |
|---|---:|---:|---|---|
| CS2 (92 MB, dense per-frame payloads) | 5.56 s | 2.93 s | **Proto** | FBS 90 % slower |
| Rocket League (5 MB, different schema shape) | **0.72 s** | 2.56 s | **FBS** | Proto 3.5× slower |

The flip is driven by per-frame payload size and schema shape — CS2 favours Proto's streaming decode, RL favours FBS's zero-copy access. **Measure on your replay shape before picking a default.**

### 3. Short-circuit diffing is earning its keep

The differ fingerprints two frames (xxHash) before falling back to the full structural diff. On identical frames the shortcut pays off:

| Fixture | Identical (short-circuit) | First-vs-last (worst case) | Speedup |
|---|---:|---:|---:|
| Arena (small) | 6.3 µs | 59 µs | **~10×** |
| CS2 (big) | 66 µs | 131 µs | **~2×** |

Ratio shrinks on big frames because hashing itself becomes non-trivial — but the shortcut is still a clear win.

## What these numbers do *not* prove

- **One machine.** Windows, i9-13900H, 20 threads, SSD. Customer hardware will vary; *ratios* hold, absolute numbers don't.
- **One benchmark run.** `repeats:5` on the heavy workloads, adaptive on the rest. No multi-machine statistical harness yet.
- **No competitor comparison.** We measure VTX against itself.
- **`items_per_second` in google/benchmark uses CPU time, not wall time.** Where wall ≫ CPU (async I/O), that metric overstates user-observable throughput. For customer-facing claims use the wall-time column.
- **One known fixture bug** — `BM_AccessorRandomWithinBucket` inflates its own counter 2× due to a duplicate-push in the shuffle setup. Flagged for a follow-up fix; everything else is trustworthy.

## Running benchmarks locally

The benchmark binary is gated behind `VTX_BUILD_BENCHMARKS`. It uses google/benchmark (fetched via `FetchContent`), Release builds only.

```bash
# Configure + build
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DVTX_BUILD_BENCHMARKS=ON
cmake --build build-bench --target vtx_benchmarks --config Release --parallel

# Run the full suite (JSON + console output)
build-bench/bin/Release/vtx_benchmarks.exe \
    --benchmark_out=docs/benchmarks/bench_$(date +%Y%m%d_%H%M%S).json \
    --benchmark_out_format=json \
    --benchmark_counters_tabular=true

# Only the three isolated accessor layers
build-bench/bin/Release/vtx_benchmarks.exe \
    --benchmark_filter='BM_FrameAccessor_Creation|BM_EntityView_SingleGet|BM_AccessorKeyResolution'

# Only the cache-window sweep
build-bench/bin/Release/vtx_benchmarks.exe \
    --benchmark_filter='BM_CS_AccessorRandomAccess_CacheSweep_FBS'
```

Fixtures required: CS, RL, and arena replays under `samples/content/reader/{cs,rl,arena}/`. The small `synth_10k.vtx` fixture is generated at build time by `vtx_sample_write` whenever `VTX_BUILD_BENCHMARKS=ON`.

## Where the raw data lives

| Path | What it is |
|---|---|
| [`docs/benchmarks/bench_20260423_162008.json`](benchmarks/bench_20260423_162008.json) | Raw google/benchmark JSON — reusable for graphing or regression tracking. |
| [`docs/benchmarks/bench_20260423_162008.txt`](benchmarks/bench_20260423_162008.txt) | Console output as produced. |

This page is the canonical narrative version of that data. If the benchmarks are re-run, update the numbers here (and commit the new raw outputs alongside) rather than maintaining a parallel per-run markdown report.
