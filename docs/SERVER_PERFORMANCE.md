# Jamulus server real-time performance — findings and optimizations

This document records a measurement-driven investigation of the Jamulus 3.12.5 server
audio path and the optimizations that followed. Raw per-frame data and the step-by-step
log live in `WORKLOG.md`; this file is the consolidated result.

All measurements used the 3.12.5 code line (`r3_12_5`, git `0b7c78eb`); the code PR that
implements the optimizations is rebased onto this repo's `main`.
The `tmp/` raw CSV files referenced in the text are local-only harness output, not part of
the repo.

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

**Result (fastupdate, 8 clients):**

| | mean | p50 | p95 | p99 | over 1333 µs |
|---|---|---|---|---|---|
| before | 929 | 846 | 1829 | 2118 | 26.7% |
| after | **150** | **145** | **230** | **300** | **0.0%** |

### Win 2 — thread-pool block count caused ~5× sync overhead

**Root cause.** `OnTimer` split each frame into `iNumBlocks = min(iNumClients, iMaxNumThreads)`
blocks and enqueued one pool task per block. `CThreadPool::enqueue` allocates a
`shared_ptr<packaged_task>` + bound closure per task, takes a mutex, and signals a condition
variable; the timer thread then `future.wait()`s each one. With 16 clients this meant
**12 tasks per wave, each doing only 1–2 encodes**, and two waves per frame (decode + mix).
The per-task sync cost dwarfed the useful work, which is why `-T` showed no median benefit.

**Fix.** Target ~3 clients per block so each task carries real work:

```cpp
// was: iNumBlocks = std::min ( iNumClients, iMaxNumThreads );
iNumBlocks = std::min ( ( iNumClients + 2 ) / 3, iMaxNumThreads );
// 16 clients -> 6 blocks (~3 clients each) instead of 12 blocks (1-2 each)
```

**Result (`-T`, 16 clients, 128s):**

| | mean | p50 | p95 | p99 | over 2667 µs |
|---|---|---|---|---|---|
| before | 2052 | 2131 | 2695 | 3780 | 5.3% |
| after | **406** | **378** | **594** | **649** | **0.0%** |

## 4. The remaining optimization (implemented)

The steady-state server sends every client the *same* mix: gains/pans default to 1.0 / 0.5
and the mix includes every source (there is no self-exclusion in `MixEncodeTransmitData`).
Only when a client adjusts the mixer (per-receiver gains/pans) or during fade-in do the
per-receiver mixes differ.

When all per-receiver gain/pan rows and channel formats are uniform, the server can **mix and
encode once per frame and replay the same encoded packet to every client** instead of doing N
encodes. With identical mixes this is bit-identical output. The code falls back to the
current per-destination path when rows differ (mixer adjustments, fade-ins, mixed
mono/stereo, mixed frame sizes) or with `--delaypan`. This turns the encoding cost from O(N)
per frame to O(1) in the common case.

**Result (`-T`, 16 clients, 128s, with Steps 1+2+3):**

| | mean | p50 | p95 | p99 | over 2667 µs |
|---|---|---|---|---|---|
| baseline | 2052 | 2131 | 2695 | 3780 | 5.3% |
| after all 3 | **276** | **218** | **576** | **673** | **0.0%** |

Falls back correctly (`--delaypan`): 8c `-T` mean 259 µs, p99 404 µs, no over-budget.

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

> Note: the `//### TEST` timing instrumentation and the encoder/block changes are on top of
> the 3.12.5 tree; the instrumentation is temporary and must be removed before any upstream
> patch. See `WORKLOG.md` for the live status.