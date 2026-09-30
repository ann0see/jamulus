# Jamulus server real-time performance — findings and optimizations

This document records a measurement-driven investigation of the Jamulus 3.12.5 server
audio path and the optimizations that followed. Raw per-frame data and the step-by-step
log live in `WORKLOG.md`; this file is the consolidated result.

Initial measurements used the 3.12.5 code line (`r3_12_5`, git `0b7c78eb`); the code changes
were then also measured on this repo's `main` (fork `main` HEAD `292506e`) — see §5b. The
changes live in separate PRs (#324, #325; the uniform-mix fast path in §4 was prototyped and
dropped). The `tmp/` raw CSV files referenced in the text are local-only harness output, not
part of the repo.

Environment: macOS, Apple M4 Pro (12 cores, 8P+4E), Qt 6.11.1, Jamulus 3.12.5 (git `0b7c78eb`).

---

## 1. Method

The server's audio path runs in `CServer::OnTimer()`, invoked once per audio frame:

- normal mode: 128 samples @ 48 kHz → **2667 µs** frame budget
- fastupdate (`-F`): 64 samples @ 48 kHz → **1333 µs** budget

Temporary instrumentation (now under `//### TEST` blocks) split each frame of `OnTimer` into:

- `decode_us` — time holding `Mutex` (channel fetch + OPUS decode + MT `future.wait`). The
  network thread blocks on this mutex for that long.
- `mix_us` — everything after the mutex (levels, socket-buffer updates, the
  **mix + OPUS encode + transmit** phase).

Measured with `/tmp`-style harness now in `tmp/perf_harness.sh`: one server plus N real
no-GUI clients on loopback (`-T` = multithreading, `-F` = fastupdate). Clients share the
12-core box, so absolute tails are an upper bound; the phase split and the *relative*
before/after numbers are what matter.

> Measurement gotcha: `QElapsedTimer::elapsed()` returns **milliseconds**. Use
> `nsecsElapsed() / 1000` for microseconds, otherwise sub-millisecond frames read as `0`.

## 2. What actually matters (baseline)

At 8–16 clients the frame is dominated by the **mix + OPUS encode** phase (`mix_us` ≈ 1.8–1.9 ms);
decode is small (14–220 µs). `sample` profiles confirm the hotspot is
`MixEncodeTransmitData` → `celt_encode_with_ec` / `quant_*` / `op_pvq_search_c`, i.e. the
per-client OPUS encode of the full mix performed on the audio/timer thread.

| Config | mean | p50 | p95 | p99 | over budget |
|---|---|---|---|---|---|
| 8c single, 128s | 1861 | 2011 | 2415 | 3161 | 1.7% |
| 16c single, 128s | — | 2092 | — | — | (unstable tails) |
| 16c `-T`, 128s | 2052 | 2131 | 2695 | 3780 | 5.3% |
| 8c `-F`, 64s | 929 | 846 | 1829 | 2118 | 26.7% |

Two conclusions drove the fixes:

1. OPUS encode is essentially the whole cost. Anything that reduces encode work or the
   number of encodes wins directly.
2. `-T` (thread pool) did **not** help at the median (`-T` p50 ≈ single-thread p50). The
   pool's per-task synchronization overhead was eating the parallelism.

### What does NOT matter (contrary to a circulating "analysis")

- Per-frame heap allocations in `CreateChannelList` — event-driven (connect/disconnect only).
- Socket signal-copy TODOs in `socket.cpp` and the protocol-body alloc in
  `ParseMessageFrame` (`protocol.cpp:2671`) — control messages only. Audio UDP packets carry
  no protocol header and fail the tag check *before* any allocation, so the network thread
  does not allocate per audio packet.
- "Mixing/encoding under `Mutex`" — false. The `QMutexLocker` scope ends before the mix
  phase; only the (small) decode is inside the lock.

## 3. The wins

### Win 1 — fastupdate was running OPUS at maximum complexity

**Root cause.** The server creates four encoders per channel: mono/stereo × 128-sample/64-sample.
`OPUS_SET_COMPLEXITY(1)` was applied only to the 128-sample encoders:

```cpp
// server.cpp (before)
opus_custom_encoder_ctl ( OpusEncoderMono[i],   OPUS_SET_COMPLEXITY ( 1 ) );
opus_custom_encoder_ctl ( OpusEncoderStereo[i], OPUS_SET_COMPLEXITY ( 1 ) );
// Opus64EncoderMono / Opus64EncoderStereo keep the default complexity (10)
```

So every `-F` session encoded at the library default complexity 10 — roughly 6× the CPU of
complexity 1 — even though this is the server's *mix-down* stream where the low-delay,
low-complexity setting already in use for 128-sample frames is appropriate.

**Fix.** Set complexity 1 on the 64-sample encoders too:

```cpp
opus_custom_encoder_ctl ( Opus64EncoderMono[i],   OPUS_SET_COMPLEXITY ( 1 ) );
opus_custom_encoder_ctl ( Opus64EncoderStereo[i], OPUS_SET_COMPLEXITY ( 1 ) );
```

**Result.**

On 3.12.5 the isolated Step-1 measurement (WORKLOG §5, CSV) was 8c `-F` mean 929 → 150 µs,
p95 1829 → 230 µs, over-budget 26.7% → 0.0% — the fastupdate encode path genuinely ran at
default complexity 10 there.

Isolated measurement on fork `main` (BASE → with this change alone):

| | mean | p50 | p95 | p99 | over 1333 µs |
|---|---|---|---|---|---|
| 8c `-F` before → after | 132 → 134 | 131 → 122 | 222 → 239 | 292 → 312 | 0.0% → 0.0% |
| 16c `-F` before → after | 235 → 244 | 209 → 215 | 449 → 464 | 522 → 580 | 0.0% → 0.0% |

Neutral within measurement noise on `main`, which is already ~13× faster there. The change is
kept for consistency/safety margin, with the quality-vs-CPU tradeoff at 64-sample frames still
open for review.

### Win 2 — thread-pool block count caused ~5× sync overhead

**Root cause.** `OnTimer` split each frame into `iNumBlocks = min(iNumClients, iMaxNumThreads)`
blocks and enqueued one pool task per block. `CThreadPool::enqueue` allocates a
`shared_ptr<packaged_task>` + bound closure per task, takes a mutex, and signals a condition
variable; the timer thread then `future.wait()`s each one. At high client counts
(`n > threads`) the pool did **12+ tasks per wave each carrying only 1–2 encodes**, and two
waves per frame (decode + mix); the per-task sync cost is real but only dominates when blocks
outnumber useful work, i.e. roughly at ≥ 2× the core count.

**Fix.** Target ~3 clients per block so each task carries real work:

```cpp
// was: iNumBlocks = std::min ( iNumClients, iMaxNumThreads );
iNumBlocks = std::min ( ( iNumClients + 2 ) / 3, iMaxNumThreads );
// e.g. 24 clients -> 8 blocks (~3 clients each) instead of 12 blocks (2 each)
```

**Result.**

On 3.12.5 the isolated Step-2 measurement (WORKLOG §5, CSV) was `-T` 16c mean 2052 → 406 µs,
p95 2695 → 594 µs, over-budget 5.3% → 0.0% (the further drop to 276 µs in the old combined
run came from all three steps incl. the since-dropped uniform mix).

Isolated measurement on fork `main` (BASE → with this change alone, pooled over 3 runs):

| config | mean before → after | p95 before → after | over 2667 µs |
|---|---|---|---|
| 16c `-T` | 363 → 380 µs | 602 → 617 µs | 0.0% → 0.0% (neutral, in noise) |
| 24c `-T` | 440 → 391 µs (-11%) | 839 → 778 µs (-7%) | 0.0% → 0.0% (consistent per run) |

Per-run 24c means (baseline → with change): 574 → 525, 365 → 299, 404 → 370 µs. The gain is
consistent but concentrates at ≥ 2 blocks per thread (i.e. higher client counts), where the
pool round-trip is what the frame budget hits first.

## 4. The uniform-mix fast path — prototyped, then dropped

The steady-state server sends every client the *same* mix: gains/pans default to 1.0 / 0.5
and the mix includes every source (there is no self-exclusion in `MixEncodeTransmitData`).
Only when a client adjusts the mixer (per-receiver gains/pans) or during fade-in do the
per-receiver mixes differ.

A prototype detected that uniform state (identical per-receiver gain/pan rows, channel count,
compression type, frame-size conversion blocks, coded byte count, no `--delaypan`) and then
mixed + OPUS-encoded once per frame, replaying the same encoded packet to every client —
turning the encode cost from O(N) to O(1) in that case.

Isolated measurement on fork `main` (BASE → with the prototype): 16c single mean 446 → 343 µs,
p95 682 → 569 µs; 16c `-T` mean 389 → 255 µs, p95 602 → 449 µs. The per-frame uniform check's
cost in the non-uniform (busy-mixer) case was not isolated-measured — only estimated as small
(O(n²) float compares, n ≤ 16-24) — so that number is not claimed here. It is **correct**
(replay is bit-identical while the invariant holds; review additions: compression-type and
`vecNumFrameSizeConvBlocks` equality gates) but **not proposed**: the uniform steady state does
not map to real-world usage, because any mixer adjustment or format difference instantly
drops it to the per-target fallback. **Dropped** as of 2026-09-12; closed draft PR, not part
of #324/#325.

Note: the `-F` 3.12.5 "929 → 150 µs" figure in §3 Win 1 predates this fast path (it was the
Step-1 opus64-only measurement); the same change is neutral on fork `main`.

## 5b. Re-measurement on the fork's `main` (2026-09-12)

The code PRs were rebased onto the fork's `main` (HEAD `292506e`, ~300 commits past 3.12.5).
Fork `main` already carries its own audio-path improvements ("Avoid per-frame deep copy in
CreateLevelsForAllConChannels", `std::atomic` cross-thread audio params, "Bound panning",
channel-info mutex work), so its baseline is far lower than the 3.12.5 baseline. The same
harness was rerun on both fork `main` (BASE) and fork `main` + changes (OPT).

Combined run (all three changes at once — the uniform-mix one is since dropped from the PRs,
but retained here as the CPU-ceiling measurement):

| Config | BASE mean | OPT mean | BASE p95 | OPT p95 | BASE over budget | OPT over budget |
|---|---|---|---|---|---|---|
| 16c single (2667 µs) | 446 | **310** | 682 | 615 | 0.0% | 0.1% |
| 16c `-T` (2667 µs) | 389 | **229** | 602 | 417 | 0.0% | 0.0% |
| 8c `-F`, 64 s (1333 µs) | 132 | 135 | 222 | 226 | 0.0% | 0.0% |
| 8c `-T --delaypan` (2500 µs) | 232 | 253 | 336 | 344 | 0.0% | 0.0% |

Isolated per-change (the numbers that actually back the surviving PRs):
| PR | change | config | BASE → OPT mean | verdict |
|---|---|---|---|---|
| #324 | Opus64 complexity 1 | 8c `-F` | 132 → 134 µs | neutral (noise) |
| #324 | Opus64 complexity 1 | 16c `-F` | 235 → 244 µs | neutral (noise) |
| #325 | block cap | 16c `-T` | 363 → 380 µs | neutral (noise) |
| #325 | block cap | 24c `-T` | 440 → 391 µs (pooled, 3 runs) | consistent -11% |
| (dropped) | single-mix replay | 16c single | 446 → 343 µs | large, but not proposed |

Interpretation: the large 16-client gains in the combined rows were dominated by the dropped
uniform-mix fast path. The surviving PRs together buy the 24-client `-T` headroom (#325,
~-11%) plus a consistency/safety-margin change (#324, neutral on `main`); both stay far under
the frame budget on this hardware. The combined single-thread 16c OPT run showed 0.1% of
frames marginally over 2667 µs (a few frames, no sustained overruns).

## 5. Reproduce

```sh
# build (arm64; QT_ARCH=arm64 is mandatory, otherwise x86 SSE4 opus files are selected)
~/Qt/6.11.1/macos/bin/qmake Jamulus.pro -spec macx-clang \
  'CONFIG+=release' 'CONFIG+=noupcasename' 'QT_ARCH=arm64'
make -j8 ARCHS=arm64

# run a test (from repo root; writes CSV into ./tmp/)
./tmp/perf_harness.sh "-T" 16 40
```

Analysis of a CSV: column 1 = total µs, 2 = decode µs, 3 = mix µs, 4 = connected clients.

> Note: the `//### TEST` timing instrumentation is temporary and must be removed; the
> encoder/block changes live in PRs #324/#325 on fork `main`. See `WORKLOG.md` for the live
> status.