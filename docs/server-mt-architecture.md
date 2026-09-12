# Server audio pipeline & thread-sync redesign

Companion to `WORKLOG.md` / `docs/SERVER_PERFORMANCE.md`. This document describes the
*remaining* architectural bottleneck in the `-T` (multithreaded) server path and how to
remove it. The measured optimizations — a thread-pool block-count fix and the Opus64
complexity change — are covered in `SERVER_PERFORMANCE.md` (see PRs #324/#325); the
uniform-mix fast path that would bypass the pool entirely was prototyped and **dropped**
(§4), so the pool remains in the ordinary steady state and this redesign concerns the
whole `-T` path.

## Current design (server.cpp)

Every audio frame, `CServer::OnTimer()` does two dependent waves on a `CThreadPool`:

1. **Decode wave** — split the connected clients into `iNumBlocks` blocks
   (`iNumBlocks = min((n+2)/3, iMaxNumThreads)` after the fix, was `min(n, threads)`),
   enqueue `DecodeReceiveDataBlocks(start..stop)` per block, then `future.wait()` each.
2. **Mix wave** — the mix for destination *d* needs the decoded audio of *all* sources, so
   decoding must be globally complete before mixing starts. Enqueue
   `MixEncodeTransmitDataBlocks(start..stop)` per block, `future.wait()` each.

`CThreadPool` (`src/threadpool.h`) is a classic `std::thread` pool: each `enqueue()` builds
`make_shared<packaged_task>` (heap alloc), locks a queue mutex, signals a condition variable;
each `future.wait()` blocks the audio thread in the kernel (`__psynch_cvwait` on macOS).

Measured impact (16 clients, before the block-count fix): 12 tasks × 2 waves ×
(alloc + mutex + condvar + wait) each frame, 5× of total frame time was sync — see
`SERVER_PERFORMANCE.md` §Win 2.

## Why it is hard

- Mix depends on *all* decoded data ⇒ the decode phase and mix phase cannot be trivially
  fused into a single per-destination task (task B for dest 0 needs the decode results of
  the task working on dest 5).
- The audio thread must not drift: it has to finish every frame within the budget
  (2667 µs / 1333 µs for `-F`). It cannot rely on work-stealing pause or preemption.
- Not every worker is needed: at n clients the parallelism is only n (decodes/encodes are
  independent per client), and 750–1500 frames/s mean the "wave" pattern happens constantly.

## Design goal

Eliminate the *per-task* synchronization: two `.wait()`-joins and N heap allocs + N condvar
signals per frame ⇒ **one frame-level fork-join with O(1) synchronization and zero
per-frame heap allocation** on the audio thread.

## Approach 1 (recommended) — persistent job reuse + single completion counter

Replace the enqueue-per-block model with a small, dedicated audio worker pool that owns a
reusable job slab:

- The pool is created once with `W` worker threads (e.g. `min(n, cores/2)`).
- Each worker owns a **persistent job record** (no allocation per frame).
- Per frame the audio thread:
  1. Publishes a pointer to the current work descriptor (`function pointer + partition
     range + generation counter`) to every worker (SPSC-style, one atomic store each).
  2. Runs a chunk of work itself, then does exactly **one** blocking wait on a single
     `std::atomic<uint32_t>` completion counter (futex/park), spinning briefly first.
  3. Workers pull their slice, run it, and `fetch_add` the counter; the last one to finish
     notifies the audio thread.
- Work stealing is not needed with ~2–8 partitions; assign contiguous slices like today and
  let the audio thread take the largest leftover slice to balance tail latency.

This turns each wave into: `W` atomic stores + 1 futex wait + `W` `fetch_add`s.

Keep TWO waves (decode ⇒ mix) as today; the join cost is now ~O(1) per wave instead of O(W)
kernel wakes. Block-count tuning (`SERVER_PERFORMANCE.md` §Win 2) becomes less critical but
stays.

## Approach 2 (more invasive) — pipelined single wave

Because decode(i) only needs source *i* and mix(d) needs all decoded, a true single-pass
pipeline requires delaying mix by one production step:

- **Producer phase:** each worker decodes its sources and immediately *accumulates* them
  into a cross-accessible preview… but the mix is only defined after all sources
  contributed, so produce **partial sums** instead:
  - Precompute a per-frame "common core" = Σ_sources (gain·pan-normalized audio) as sums
    accumulated during decode (each worker adds only *its* sources).
  - Then per destination: `mix(d) = core + Σ_{sources with per-receiver gain≠default}
    (gain(d,s)−default)·audio_s`.
  - Only destinations with non-default rows need the correction term (usually none).
- This merges the decode and mix waves into ONE wave (single join), and is also the natural
  generalization of the "mix once" idea explored in the uniform-mix prototype (since dropped,
  §4 of `SERVER_PERFORMANCE.md`; the common-core term is precisely its "mix once"). Microphone
  -level correctness: floating accumulation order changes are negligible (-120 dB) but for
  bit-exactness the per-dest math for corrected destinations must be unchanged.

Risks: touching the carefully-tuned mixing loop (`MixEncodeTransmitData`,
`server.cpp:936-1206`), pan/fade handling (`--delaypan` uses a one-frame-delayed buffer
`vecvecsData2`), and possible float nondeterminism. This is why Approach 1 is preferred as
the immediate step; Approach 2 unlocks O(1) real cost but deserves its own hardening pass.

## Approach 3 (fallback) — just cap and document

Current code already caps blocks (`min((n+2)/3, threads)`). A pure config-level mitigation
for the shared pool is to also cap the pool thread count to avoid oversubscription when the
box hosts clients (e.g. set `iMaxNumThreads = max(2, cores/2)`). Cheapest, least risky, but
keeps the per-task sync overhead.

## Non-goals / context

- The decode-under-`Mutex` window (network thread blocks while the audio thread decodes) is a
  separate, bounded issue (~60–160 µs) and is absorbed by jitter buffering — do not restructure
  it as part of this work.
- The uniform-mix fast path (prototyped, then dropped — §4) is NOT available as a fallback;
  the pool therefore remains the steady-state mix engine and Approaches 1–2 below are the way
  to cut its sync cost.

## Suggested implementation order

1. Add a `CThreadPool::runFrame()`-style API (persistent slabs, `atomic<uint32_t>` counter,
   single wait) while keeping the old `enqueue()` for other callers.
2. Port the decode wave, then the mix wave, to the new API; keep block sizing.
3. Benchmark the fallback path (`-T --delaypan`, `-T` with a remote-controlled gain) against
   `perf_run_--delaypan_-T_8c_1789219573.csv` (mean 259 µs, p99 404 µs on 3.12.5;
   on fork `main` the fallback measured mean 252 µs, p99 379 µs).
4. Optionally pursue Approach 2 only if further gains are needed (mix cost is already
   dominated by the fallback's per-dest encodes, which are inherently O(n)).

Status: **proposal only, not implemented.** Measured numbers in
`WORKLOG.md` §7 / `docs/SERVER_PERFORMANCE.md`.