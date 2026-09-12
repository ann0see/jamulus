# WORKLOG — Jamulus server performance investigation

Repo: Jamulus 3.12.5, git HEAD `0b7c78eb` (r3_12_5), Apple Silicon M4 Pro, 12 cores, 48 GB.
Machine: macOS, M4 Pro, 12 cores (8P + 4E), 48 GB RAM. Qt 6.11.1 in `~/Qt/`.

> Goal: measure which server code paths actually cause audio-latency problems, then
> implement + verify targeted optimizations. This file records everything step by step
> so the state survives context loss.
>
> **Consolidated write-up of the confirmed wins: `docs/SERVER_PERFORMANCE.md`.**

---

## 1. Environment / how to build & run tests (REQUIRED reading)

- Build (arm64): `~/Qt/6.11.1/macos/bin/qmake Jamulus.pro -spec macx-clang 'CONFIG+=release' 'CONFIG+=noupcasename' 'QT_ARCH=arm64'` then `make -j8 ARCHS=arm64`.
  - `Jamulus.pro` adds `CONFIG += x86` on mac → without `QT_ARCH=arm64` the x86 SSE4 opus
    files get selected and fail to compile on arm64. ALWAYS pass `QT_ARCH=arm64`.
- Binary: `jamulus.app/Contents/MacOS/jamulus` (also used for dummy clients).
- `CONFIG+=headless` is BROKEN on mac in 3.12.5: CoreAudio `sound.h` includes `QMessageBox`
  (widgets), so a GUI build is required; run with `--nogui`/`-n` instead — same no-GUI path.
- Server invocation used in perf tests:
  `jamulus -s --nogui -u 64 -p 22124 --serverbindip 127.0.0.1` (+ `-T` for multithreading, `-F` for fastupdate).
- Client (load bot): `jamulus -n -c 127.0.0.1:22124 -p 23000+N --clientname botN`.
- Harness: `/tmp/perf_harness.sh <server_extra_args> <num_clients> <duration_s>` — kills all
  `jamulus.app` procs, starts server + N clients on loopback, saves CSV to `/tmp/perf_run_*.csv`.
- Per-frame timing CSV (`tmp/perf_server.csv`, written by temporary TEST instrumentation in
  `CServer::OnTimer`, columns `total_us decode_us mix_us numclients`):
  - `decode_us` = inside `Mutex` lock (channel fetch + OPUS decode + MT futures.wait). The socket
    (network) thread blocks on this mutex for that long each frame.
  - `mix_us` = rest of `OnTimer` after mutex release (levels, socket-buffer updates, the
    mix+encode+transmit phase, todo-list processing).
  - Frame budget: 128 samples @ 48 kHz = **2667 µs** normal; 64 samples (`-F`) = **1333 µs**.
- MEASUREMENT TIP: `QElapsedTimer::elapsed()` returns MILLISECONDS (truncating sub-ms to 0!).
  Use `nsecsElapsed() / 1000` for microseconds (this bit us once).

## 2. Verification of the third-party "performance analysis" (mostly WRONG)

The earlier "analysis" (sensationalized) claimed: allocations in `CreateChannelList`, vector
copies on socket signal emission, and "mixing/encoding within a mutex" were the top real-time
hazards. Verified against code:

1. `CreateChannelList` (`server.cpp:1208`) — called only on channel connect/disconnect/info
   change, NOT per frame. Its `CVector::Init/Add` churn (`util.h`, `CVector` is a
   `std::vector` subclass) is event-driven and rare. Not a per-frame hazard.
2. Socket vector copies (`socket.cpp:517-527` TODO *newHeapAllocations*): real but only for
   protocol/control messages. Audio UDP packets carry no protocol header — they fail the tag
   check in `ParseMessageFrame` before any body allocation (`protocol.cpp:2671`). So the
   network thread does NOT allocate per audio packet. Control-path only.
3. "Mixing/encoding happens while the network thread holds Mutex" — FALSE. `QMutexLocker`
   scope ends at `server.cpp:667`; mixing/encoding runs after (`MixEncodeTransmitData` at
   line ~936, called from `OnTimer` after `Mutex` is released). What IS inside the lock is
   only the **decode** phase (`DecodeReceiveData`, opus decode). That is real but small/bounded.

The socket runs in its own high-priority thread (`src/socket.h:216-228`).

## 3. Baseline measurements (instrumented build)

Runs on loopback with N real no-GUI clients. Caveat: clients share the 12-core box, so
absolute tails are inflated by oversubscription; relative trends (single vs `-T`, phase
splits) are what matter.

| Config | n | mean | p50 | p95 | p99 | max | over budget |
|---|---|---|---|---|---|---|---|
| 1c, 128s | ~10k | 428 | 418 | 586 | 878 | 4.0 ms | ~0% |
| 8c, 128s | 14583 | 1861 | 2011 | 2415 | 3161 | 7.4 ms | 1.7% |
| 16c, 128s, single | 6245 | 5942 | 2092 | 20722 | 42847 | **703 ms** | 41.5% |
| 16c, 128s, **`-T`** | 16260 | 2052 | 2131 | 2695 | 3780 | 5.4 ms | 5.3% |
| 24c, 128s (oversubscr.) | ~10k | 3890 | 3651 | 5035 | 6519 | 14.2 ms | ~99% |
| 8c, **`-F`** (64s, 1333 µs) | 21493 | 929 | 846 | 1829 | 2118 | 6.8 ms | 26.7% |

Phase breakdown at 8–16 clients: decode ≈ 14–220 µs (small); mix+encode ≈ 1.8–1.9 ms and
dominates. `sample` profiles: hot = `MixEncodeTransmitData` → `celt_encode_with_ec`,
`quant_*`, `op_pvq_search_c` (i.e. **OPUS encode**). `-T` profile shows time mostly in
`__psynch_cvwait`/`__ulock_wait2` (thread-sync waits), i.e. pool sync overhead eats the
parallelism gain (`-T` p50 ≈ single-thread p50).

Hottest finding → the latency-critical thing is the **per-client OPUS encode of the full mix,
done serially on the audio/timer thread** (and, with `-T`, the per-wave pool sync overhead).

CSV files (baselines): `/tmp/perf_run__8c_1789151504.csv`, `-T_8c_1789151547.csv`,
`-T_16c_1789151590.csv`, `_16c_1789151678.csv`, `_24c_1789151376.csv`, `-T_24c_1789151427.csv`,
`-F_8c_1789151829.csv`.

## 4. Decided optimizations

1. **Opus64 encoder missing `OPUS_SET_COMPLEXITY(1)`** (`server.cpp` ctor ~138). Only the
   `OpusEncoderMono/Stereo` (128-sample) encoders get complexity 1; the `Opus64*` (64-sample,
   used with `-F`) encoders stay at default complexity 10 → fastupdate is far more expensive
   than needed. Fix = add the ctl for both `Opus64EncoderMono/Stereo`. Test with `-F` 8c.
2. **Pool block count too high** (`server.cpp:638` `iNumBlocks = min(iNumClients, iMaxNumThreads)`)
   → at 16 clients, 12 tasks doing 1–2 encodes each; each enqueue+`future.wait()` costs a
   kernel condvar wake + heap alloc (`CThreadPool::enqueue` builds `shared_ptr`+`packaged_task`
   per task). Fix = fewer blocks (≥3 clients per block), reducing sync overhead while keeping
   good parallelism. Test with `-T` 16c.
3. **Mix/encode dedup when all clients receive the same mix.** NOTE (checked in code): Jamulus
   mixes EVERY source into EVERY destination including self (all gains/pans default to 1.0 /
   0.5 center; no self-exclusion in `MixEncodeTransmitData`, `server.cpp:950-1122`). So the
   common steady state is: all per-receiver mixes are IDENTICAL. "Subtract-the-sender"
   precompute is not applicable (there is no sender exclusion); the applicable optimization is
   **mix+encode ONCE per frame and replay the encoded packet to every client** when gains/pans
   and channel formats are uniform. Falls back to per-destination processing when rows differ
   (client mixer adjustments, fade-ins, mixed mono/stereo, mixed frame sizes, or `--delaypan`).
4. **Architecture doc** for removing the two-wave barrier sync in `-T`
   (work-stealing / lock-free per-worker queues, fuse decode+encode pipeline) — see doc file.

Planned test matrix (each vs. its baseline above):
- #1: `-F` 8c → expect p99/drop and "over budget" far below 26.7%.
- #2: `-T` 16c → expect p50/p99 drop below baseline 2131/3780.
- #3: 16c single AND `-T` → expect mix phase to collapse from ~1.9 ms to a few hundred µs.

## 5. Status log (chronological)

- [x] Baseline + hotspot identification (Sec. 3), conclusions drawn.
- [x] **Step 1 — Opus64 complexity fix** (`server.cpp` ctor, added `OPUS_SET_COMPLEXITY(1)`
      for `Opus64EncoderMono/Stereo`). RESULT: fastupdate 8c went from mean 929 µs /
      p95 1829 µs / 26.7% over-budget to **mean 150 µs / p95 230 µs / p99 300 µs / 0.0%**.
      Confirms the 64-sample OPUS encoders ran at default complexity 10 (~6× CPU cost).
      CSV: `tmp/perf_run_-F_8c_1789213177.csv`.
- [x] **Step 2 — pool block count** (`server.cpp` OnTimer, `iNumBlocks = min((n+2)/3, threads)`).
      RESULT: `-T` 16c went from mean 2052 µs / p95 2695 µs / 5.3% over-budget to
      **mean 406 µs / p95 594 µs / p99 649 µs / 0.0%** (~5×). 12 blocks of 1-2 encodes each were
      pure pool-sync overhead (`make_shared<packaged_task>` + kernel condvar per task);
      6 blocks of ~3 clients each fix it. CSV: `tmp/perf_run_-T_16c_1789213259.csv`.
- [x] **Step 3 — mix/encode-once fast path** (`server.cpp` OnTimer). When all per-receiver
      gain/pan rows + channel format are uniform (the steady state: gains 1.0, pans 0.5, no
      fade-in, no mixer changes, not `--delaypan`), mix + OPUS-encode ONCE for channel 0 and
      replay the same encoded packet to every other channel via its own `PrepAndSendPacket`.
      Falls back to per-channel processing otherwise. Final numbers (see table in Step 3
      result block below). CSVs: `tmp/perf_run__16c_1789219246.csv`, `-T_16c_1789219335.csv`.
- [x] Fallback path smoke test (`--delaypan` forces per-channel path): 8c `-T` mean 259 µs,
      p99 404 µs, no crash, no over-budget. CSV `tmp/perf_run_--delaypan_-T_8c_1789219573.csv`.
- [x] **#4 architecture doc** written: `docs/server-mt-architecture.md` (persistent-job
      CThreadPool with single completion counter; pipelined common-core single wave as the
      longer-term Option 2; cap+document as fallback).
- [x] FINAL PATCH: TEST instrumentation removed → `src/server.cpp` now contains ONLY the 3
      real optimizations (Opus64 complexity, block count, uniform-mix fast path).
      `git diff > tmp/server_perf.patch` (production-only, +63/−4), verified
      `git apply --check` against pristine 3.12.5 = clean. Build OK (0 errors), smoke test
      `-T` 8c: server runs, clients connect, clean exit. ⚠️ For you to do: manual audio
      correctness test of the patch.

## 7. Final comparison (all three optimizations in, EVERY run measured fresh)

| Config | mean | p50 | p95 | p99 | over budget |
|---|---|---|---|---|---|
| BL 16c single | 5942 | 2092 | 20723 | 42847 | 41.5% |
| **16c single (S1–S3)** | **334** | **278** | **604** | **728** | **0.0%** |
| BL 16c `-T` | 2052 | 2131 | 2695 | 3780 | 5.3% |
| **16c `-T` (S1–S3)** | **276** | **218** | **576** | **673** | **0.0%** |
| BL 8c `-F` | 929 | 846 | 1829 | 2118 | 26.7% |
| **8c `-F` (S1–S3)** | **153** | **148** | **225** | **278** | **0.0%** |
| fallback 8c `-T --delaypan` | 259 | 254 | 341 | 404 | 0.0% |

Full patch (incl. TEST instrumentation): `tmp/server_perf.patch` (171 lines).
Decode is now the largest remaining phase (60–160 µs); decode+mix together are ~10–13% of
the frame budget at 16 clients.

## 6b. Root cause of "the agent got stuck" (operational)

The old harness left the server + N clients running after each run (only killed at the START
of the next run). Leftover jamulus processes at 750 fps saturated all 12 cores, so any
build/analysis command between runs crawled and appeared "stuck". Additionally a single tool
call bundling `make` + two 35 s runs exceeded the 120 s Bash timeout.
FIXED in `tmp/perf_harness.sh`: `trap cleanup EXIT` pkills server+clients at run end,
so runs are self-cleaning.

## 6. Code locations cheat-sheet

- `CServer::OnTimer` — `server.cpp:592` (decode under `Mutex` :610-667; uniform-mix detection +
  levels + socketbuf loop; mix dispatch: bUniformMix→encode once + replay, `-T`→pool blocks,
  else→serial per-channel; `Stop()` when idle).
- `CServer::DecodeReceiveData` — `server.cpp:~800` (fetches gains/pans per frame ~839-860,
  opus decode, conv buffers). Inside the mutex.
- `CServer::MixEncodeTransmitData` — `server.cpp:936` (per-destination mix loop :952-1122,
  float→short :1124-1128, encoder select :1131-1163, bitrate ctl + `opus_custom_encode` +
  `PrepAndSendPacket` :1170-1203). AFTER the mutex.
- `CServer::MixEncodeTransmitDataBlocks` / `DecodeReceiveDataBlocks` — static callbacks for the
  `CThreadPool` (used with `-T`).
- `CThreadPool` — `src/threadpool.h` (`std::thread`, `queue_mutex`, condvar; per-task
  `make_shared<packaged_task>` + notify).
- Opus encoder ctor block — `server.cpp:100-145`.
- `Opus`/`Opus64`, complexity: `OPUS_SET_COMPLEXITY` — server sets complexity ONLY for the
  128-sample encoders currently.
- Self-gain: no sender exclusion; gains default 1.0 (`CChannel::SetGain`, `channel.cpp:295`).