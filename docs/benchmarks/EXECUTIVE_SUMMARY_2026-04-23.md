# VTX SDK — Performance Report (Executive Summary)

**Date:** April 23, 2026 (revised — includes isolated measurements for the three accessor layers)
**Version measured:** `main` after PR #4 (prefetch tuning) and PR #6 (deadlock fix)
**Audience:** product, marketing, leadership, and anyone who needs to talk about VTX speed without reading the engineering report.

---

## 1. What this document is

This is a plain-language summary of a performance measurement pass we ran today on the VTX SDK. The full engineering report (with raw numbers and methodology) sits next to this file. This document has three goals:

1. Translate the numbers into sentences you can use in conversations, slide decks, and sales calls — without misrepresenting what they mean.
2. Surface the most important findings that non-engineers on the team need to know, because they affect product decisions, documentation, and how we talk about VTX externally.
3. Give an honest picture of what we *don't* yet know, so expectations stay calibrated.

You should be able to read this in 5–7 minutes and walk away with a defensible mental model of how fast VTX is, where it's fast, where it's slow, and why.

---

## 2. Quick context: what is the VTX SDK doing?

The VTX SDK is the library that game studios, esports tooling companies, and coaching platforms embed to **record, read, compare, and analyze game replays**. A "replay" here is a compact, timestamped recording of everything that happened in a match — every player position, every event, every piece of game state, frame by frame.

Three core operations dominate real-world usage:

- **Writing** — record a live match as it happens. Must be fast enough to keep up with the game without stuttering.
- **Reading** — open a saved replay and play it back, scrub through it, or extract specific moments. Powers replay viewers, highlight generators, coaching tools.
- **Diffing** — compare two frames to find what changed. Powers rewind logic, optimization checks, and analytics pipelines that care about deltas instead of absolute state.

Speed matters in each case for a different reason:
- Writing that's too slow drops frames or inflates CPU usage during live play.
- Reading that's too slow means laggy scrubbing and slow tool startup.
- Diffing that's too slow bottlenecks downstream analytics.

---

## 3. The test setup, in plain language

We ran the SDK through ~30 different benchmarks against four test files:

| File | Size | Frames | What it represents |
|---|---|---|---|
| `synth_10k.vtx` | 0.3 MB | 10,000 | Synthetic arena data, small and fast — isolates SDK overhead from disk cost. |
| `cs_fbs.vtx` / `cs_proto.vtx` | 92 MB / 83 MB | 10,656 | Real Counter-Strike 2 match. The "heavy real-world" case. |
| `rl_fbs.vtx` / `rl_proto.vtx` | 4.8 MB / 5.1 MB | ~21,000 | Real Rocket League match. "Typical mid-size esports replay." |
| `arena_from_*_ds.vtx` | 1.8 MB each | 3,600 | Synthetic replays with realistic schema shape (Player, Projectile, MatchState entities). |

Each benchmark measures one specific operation repeatedly until at least half a second of measurements accumulates, then reports the average time per operation. For the heaviest scenarios (like reading all 10,656 CS frames), we also repeat 5 times and report mean, median, and variation so we can tell noise from signal.

The hardware was a developer laptop (13th-gen Intel i9, 14 cores, 20 threads, Windows). Numbers should be taken as "representative of a modern dev box" rather than "guaranteed production number on customer hardware" — we'll cover that in section 10.

---

## 4. How the SDK organizes property access (important for reading §5 and §6)

The SDK exposes property access (reading "what is Player 7's health right now?") in **three layers**. This matters because customers ask "is your SDK fast?" and the answer depends on which layer they're asking about. We now measure each one separately.

Think of it like working in a library:

| SDK Concept | Library analogy | When the cost is paid | How often |
|---|---|---|---|
| **FrameAccessor** | Knowing which library branch has your topic | Integration startup | Once, per replay |
| **PropertyKey** | Writing down the call number of each book you'll need | Integration setup | Once, per property you care about |
| **EntityView** | Actually opening a book and reading the answer | Every time your code reads a value | Millions of times per second in a hot loop |

The three layers have wildly different costs (from 7 microseconds all the way down to 1.7 nanoseconds), so the honest answer to "how fast is the accessor?" is "which layer? here's each one". Section 5 has the numbers.

---

## 5. The headline numbers

These are the numbers worth memorizing. Each one is phrased so you can drop it into a conversation or slide directly.

### 5.1 Writing

- **~82,000 frames written per second, end to end.**
  End-to-end means: create the file, record 1,000 frames, compress, finalize, and close. It's the full cost, not just the in-memory work. In practical terms: a 30-minute match at 60 frames/second = 108,000 frames = **~1.3 seconds of CPU time to record the entire thing**. Real-time recording leaves an order-of-magnitude of headroom.

### 5.2 Reading

- **An entire Counter-Strike replay (92 MB, 10,656 frames) reads in ~5.6 seconds** (median). One iteration this run was slower (~10 s) — see §10 for why; median is the trustworthy number.
- **Generating a preview (first 1,000 frames) takes ~1 second.**
  Fast enough that a replay library can show thumbnails without noticeable delay.
- **Jumping to the middle of a replay and playing the next 300 frames takes ~0.9 seconds.**
  Supports fluid timeline scrubbing in a UI.
- **Reading properties in a hot loop with all data pre-loaded: 13 million properties per second.** (Real-world steady state.)
- **Reading one property with an `EntityView` in isolation: 585 million reads per second — 1.7 nanoseconds per read.** (Theoretical ceiling, no surrounding loop overhead.)

### 5.3 Accessor layers, in isolation

These are **new** numbers from this run (we didn't have them in the previous version). They decompose the "accessor cost" into the three layers from §4:

| Layer | Cost per call | Call frequency | Honest talking-point |
|---|---:|---|---|
| FrameAccessor creation | 6.77 microseconds | once per replay | Invisible. ~7 µs of startup cost. |
| PropertyKey resolution | 74 nanoseconds each | once per property | Paying for 10 properties ≈ 0.7 µs total. Invisible. |
| EntityView read | **1.7 nanoseconds** | every single property read in a hot loop | **585 million reads per second** is the upper bound. |

**What this tells the team.** If a customer says "your SDK's property access is slow", the first question is *which layer are they talking about*. If they're seeing <100 million reads/s in a hot loop, the problem is probably in the surrounding code (how they iterate entities, cache behavior of their loop), not in VTX.

### 5.4 Comparing (diffing)

- **Comparing two consecutive frames: 4 microseconds.** ~267,000 comparisons per second. Instant from a user perspective.
- **Detecting "these two frames are identical": ~6 microseconds** on small frames, ~66 microseconds on the big CS frames. The shortcut path we built in (a fingerprint check before the full comparison) pays off.

### 5.5 Format parsing

- **Loading the schema for a replay: 200 microseconds.** One-time cost per file. Invisible to users.

---

## 6. The findings everyone on the team should know

These aren't just performance trivia — each one changes how we should document, position, or sell the SDK.

### Finding 1 — A small cache is WORSE than no cache

This is the most counter-intuitive and the most important. It affects documentation, customer onboarding, and anyone answering "what cache size should I use?" questions.

**The setup.** When a user scrubs around a replay (jumping to different moments), the SDK can keep recently-read chunks in memory so re-visiting them is instant. This is the "cache window" — a number the SDK user configures. "Bigger cache = faster" is the intuition.

**The data.** We ran 50 random-access jumps in the CS replay with five different cache sizes:

| Cache window size | Time for 50 random jumps | Effective speed |
|---:|---:|---|
| 0 (no cache) | 14.56 s | baseline |
| 2 | **23.20 s** | **59 % SLOWER than no cache** |
| 5 | 16.87 s | 16 % slower than no cache |
| 10 | 3.27 s | **4.5× faster than no cache** |
| 20 | 3.09 s | same as 10 (cache is full anyway) |

**Why.** A too-small cache evicts chunks right before they would have been reused, so every jump pays the eviction cost *plus* the reload cost. At size 10 the cache finally holds the whole fixture, every access hits cache, and throughput jumps.

**What this means for the team.**

- **Documentation:** we need a clear user-facing guide on sizing the cache. The current default of "small" is actively harmful for scrubbing workloads. A user who reads no documentation and picks a small number may end up with worse performance than if they disabled the cache entirely.
- **Customer support:** any customer complaining about scrub performance — the first question is "what's your cache window?". Expect this to be a recurring conversation.
- **Product decisions:** we should consider changing the default, or auto-sizing based on replay length.

### Finding 2 — The accessor API is structured in three layers, with very different costs

Covered in §4 and §5.3. Recap for emphasis:

- Integration startup cost (FrameAccessor + 10 PropertyKeys): **~8 microseconds, once**. Never a problem.
- Hot-loop steady state (EntityView reads): **1.7 ns in isolation, ~80 ns in a realistic loop with entity iteration**.
- The framing matters for any conversation where a customer compares us to an alternative: "what's your per-read cost?" has three honest answers, and we should lead with the hot-loop steady state (the one users actually observe) unless they ask specifically about startup.

### Finding 3 — There is no universal "faster format"

The SDK supports two internal formats: **FlatBuffers** and **Protobuf**. A question that comes up in sales conversations is "which one is faster?". The honest answer is: *it depends on the replay*.

| Replay | FlatBuffers time (median) | Protobuf time (median) | Winner | Margin |
|---|---:|---:|---|---|
| Counter-Strike 2 (heavy) | 5.6 s | 2.9 s | **Protobuf** | 90 % faster |
| Rocket League (medium) | **0.7 s** | 2.6 s | **FlatBuffers** | 3.5× faster |

The flip is driven by how much data lives in each frame and how the schema is structured. CS2 has dense, heavy per-frame payloads that favor Protobuf's streaming decode. Rocket League has a different schema shape that lets FlatBuffers' zero-copy access win decisively.

**What this means for the team.**

- **Marketing copy / README:** we should NOT claim a universal winner. Any statement like "VTX is X% faster with format Y" is correct for one replay shape and wrong for another.
- **Sales conversations:** if a customer asks which to pick, the correct answer is "let us measure it on your replay shape". This is also a reasonable excuse to have a conversation with them about their data — a small win in the sales motion.
- **Future docs:** worth publishing a short "which format for which workload" decision guide once we have more measurements on customer-like data.

### Finding 4 — The fast-path optimizations we built are working

We built a shortcut into the comparison operation: instead of comparing two frames field-by-field, we first check a compact fingerprint (hash) to see if they're identical. If they are, we skip the full comparison.

The measured speedup of the shortcut vs. a full comparison:

- **Small frames (arena):** ~10× faster (6.3 µs vs 59 µs).
- **Big frames (CS):** ~2× faster (66 µs vs 131 µs).

The ratio shrinks on big frames because hashing large data is itself non-trivial work. But in both cases the shortcut is a clear win, and in workloads with lots of duplicate frames (pauses, menus, loading screens), the savings add up significantly.

**What this means for the team.** This is positive confirmation that recent engineering work is paying off — worth mentioning in internal status updates and release notes. No external action needed.

---

## 7. What's genuinely fast, what's slow, and what's in-between

A one-glance health check on the SDK's current state:

| Area | Verdict | Why we say this |
|---|---|---|
| Writing replays | 🟢 Fast | 82 k frames/s end-to-end. Real-time recording has ~10× headroom. |
| Reading a full replay (sequential) | 🟢 Fast enough | ~5.6 s for a 92 MB CS replay (median). Competitive with comparable industry tools. |
| Reading a preview | 🟢 Fast | 1 s — below UX noticeability threshold. |
| Scrubbing with a well-sized cache | 🟢 Fast | Sub-4-s for 50 random jumps. |
| Scrubbing with a mis-sized cache | 🔴 **Actively bad** | 23 s for the same workload. User-hostile if we don't document. |
| FrameAccessor creation | 🟢 Negligible | 6.77 µs once. |
| PropertyKey resolution | 🟢 Negligible | 74 ns per key × properties_you_care_about. |
| EntityView hot loop | 🟢 Excellent | 1.7 ns isolated / ~80 ns realistic. |
| Diffing consecutive frames | 🟢 Instant | 4 µs. Not a concern. |
| Diffing identical frames (short-circuit) | 🟢 Instant | Shortcut working as designed. |
| Schema parsing | 🟢 Negligible | 200 µs, one-time. |
| Format choice (FBS vs Proto) | 🟡 Context-dependent | No universal winner; needs per-customer measurement. |
| CS sequential-scan stability | 🟡 Noisy | One outlier iteration per run; median is trustworthy, mean fluctuates. Needs wider repeats. |

---

## 8. Recommendations (non-engineering, actionable)

Each of these is a decision for a non-engineering stakeholder. The engineering team is aware of all of them.

### For documentation / DevRel

1. **Write a "cache sizing" guide.** Highest priority. Without it, users who follow their intuition will hit Finding 1 and conclude the SDK is slow at scrubbing. Estimated effort: one short doc page.
2. **Write a "three-layer accessor" reference page.** Medium priority. The FrameAccessor / PropertyKey / EntityView distinction (§4) is genuinely useful for customers integrating the SDK. Lead with the library analogy.
3. **Write a "which format should I pick" decision guide.** Medium priority. Include the caveat that the right answer requires measurement on the specific replay shape.
4. **Add a "performance expectations" page** to the public docs, citing the top-line numbers from §5 of this report. Helps set expectations before customers run their own benchmarks.

### For marketing

1. **Do not use absolute claims about format speed.** No "VTX with Protobuf is N% faster" unless it's immediately followed by the context. Every such claim needs fixture context or it will backfire in a future demo.
2. **Lead with the end-to-end write throughput** (82 k frames/s) when talking to studios worried about real-time recording overhead. It's the number least sensitive to configuration and easiest to defend.
3. **Lead with preview time** (1 s for a 92 MB replay) when talking to tooling customers. Matches the mental model of opening a video file in an editor.
4. **Use the 585 M reads/s ceiling carefully.** It's an honest isolated number, not a realistic steady-state claim. Pair it with the ~13 M/s hot-loop number if we need a "realistic" benchmark claim, or note explicitly that the higher number is "the accessor layer alone, in isolation".

### For product

1. **Consider changing the default cache window size.** Current default is in the "worse than no cache" zone for big replays. Ideally, auto-size based on replay length, or change the default to something that covers typical scrubbing ranges.
2. **Add a "cache statistics" observability hook.** If users can see their cache hit rate in their own tooling, they self-diagnose Finding 1 without involving support.
3. **Plan a customer-data benchmark pass.** The CS and RL fixtures are representative but not exhaustive. If we have permission from a couple of customers to run benchmarks on their replays, we'll learn a lot about which format wins for whom.

### For engineering (already on their radar, included for completeness)

1. Rerun these benchmarks on Linux (our CI runners) to separate Windows-specific behavior from SDK behavior.
2. Tighten the statistical footing for the CS sequential scans (the outlier iteration moves between formats across runs — not a real format issue, but masks the honest numbers).
3. Fix the `BM_AccessorRandomWithinBucket` duplicate-push bug (flagged during this measurement pass — one of the benchmark's counters is inflated 2×).
4. Capture today's numbers as a permanent baseline and add automated regression alerts so a future change that slows the SDK down gets caught in CI.

---

## 9. Caveats — what these numbers do NOT prove

Being upfront about the limits of this measurement pass, so no one accidentally over-sells it:

- **One machine.** Windows, i9-13900H, 20 threads, SSD. Customer hardware will vary. A customer on a lower-end machine will see proportionally slower numbers, but the *ratios* (cache-size finding, format finding, short-circuit finding, accessor-layer finding) should hold.
- **One run.** Most benchmarks repeated internally by google/benchmark until a half-second budget fills, which is enough for confidence on the fast ones. Heavy benchmarks repeated 5 times for a mean/median/variation estimate. We have not yet built a multi-machine, multi-run statistical harness.
- **CS sequential-scan outliers.** This run saw an ~2× slower iteration on Proto (pulling the mean up) and on Accessor-FBS (same effect). The previous run had the outlier hit a different format. The **outlier moves around between runs**, which tells us it's OS / machine noise, not a format-specific issue. Median remains the honest number.
- **Windows background activity uncontrolled.** Antivirus, Defender real-time scanning, and other background work can add noise to open-file benchmarks. Numbers are soft upper bounds on wall time.
- **No comparison to competitor libraries.** We measure VTX against itself. Saying "VTX is faster than X" requires running the same workloads through library X, which we haven't done.
- **The internal measurement `items_per_second` uses CPU time, not wall-clock time.** Where wall ≫ CPU (lots of async I/O), that metric overstates user-observable speed. For customer-facing claims, always use the wall-time column from the raw report.
- **One known fixture bug.** `BM_AccessorRandomWithinBucket` counts each Player entity twice due to a copy-paste error in the shuffle setup, inflating its `items_per_second` by 2×. Its absolute throughput number isn't usable until fixed. Everything else in the report is trustworthy.

---

## 10. Where the underlying data lives

- **This summary (plain English, for everyone):** `docs/benchmarks/EXECUTIVE_SUMMARY_2026-04-23.md`
- **Engineering report (tables, methodology, full numbers):** `docs/benchmarks/REPORT_2026-04-23.md`
- **Spanish-language short version:** `docs/benchmarks/RESUMEN_EJECUTIVO_2026-04-23.md`
- **Raw machine-readable output (JSON, for graphing or future comparison):** `docs/benchmarks/bench_20260423_162008.json`
- **Console output from the benchmark run:** `docs/benchmarks/bench_20260423_162008.txt`

All five files are committed to the repo. Any future benchmark run should produce a similarly named file pair so we build up a time-series.

---

## 11. Glossary — terms that are hard to fully avoid

Definitions are deliberately loose. If you need precision, talk to engineering.

- **Frame.** One timestamped snapshot of the game state. A 30-minute match at 60 frames/second has 108,000 of them.
- **Entity.** One in-game thing at one moment: a player, a projectile, a match-state object. Each frame contains many entities.
- **Property.** One named field of an entity (e.g., Player.Health, Projectile.Position). Each entity has multiple properties.
- **Cache / cache window.** Short-term memory of recently-read chunks of the replay. Set by the SDK user.
- **Scrubbing.** The UI gesture of dragging a timeline to jump around a replay.
- **Sequential scan.** Reading the replay from frame 0 to the end, in order.
- **Random access.** Jumping to an arbitrary frame without reading the ones before it.
- **Preview.** Reading only the first N frames (e.g., to generate a thumbnail or decide whether to open the file fully).
- **FlatBuffers / Protobuf.** Two different internal formats we use to encode frame data on disk. Both are mature open-source formats used across the industry; which one is faster depends on the shape of the data.
- **FrameAccessor / PropertyKey / EntityView.** The three layers of the SDK's property-access API. See §4.
- **Diff / differ.** The operation of comparing two frames and producing a list of what changed.
- **Throughput.** How much work gets done per unit of time (frames per second, bytes per second, operations per second).
- **Hot loop.** A tight inner loop where the same operation runs millions of times in a row. Worst case for any overhead, best case for optimization payoff.
- **Short-circuit.** An optimization where we skip expensive work when a cheap check proves the answer is already known.
- **Isolated benchmark.** A measurement of one operation in a setup designed to exclude everything else — gives you the "theoretical ceiling" of that operation.
