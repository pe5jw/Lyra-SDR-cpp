I have everything I need. Continuing without task-list churn — Task #112 is the parent of this plan, that's what's in_progress; the operator manages its decomposition. Now I'll produce the staged plan as requested.

---

# Phase 2 / Step 14 — Wire-Up Plan, lyra-cpp Task #112

## OPERATOR CORRECTIONS APPLIED 2026-06-06

The original draft of this plan framed three load-bearing points as
"deviations" / "operationally equivalent" / "operator must decide"
soft language. The standing rule is **"do as reference, period,
NO PATCHING."** Three corrections folded in below; each replaces
soft framing with hard mandatory invariants tied to the reference
file:line:

1. **Stage 2 priming-pass LOCATION (Change A).** Priming runs
   INSIDE `Ep6RecvThread::run_loop()` body, AFTER thread spawn,
   BEFORE WSAEventSelect/WSAWaitForMultipleEvents loop entry —
   exact reference order at `networkproto1.c:427-434`. The
   "hoist START to `open()`" sub-piece STAYS (matches
   `netInterface.c:50`). The `prn->hReadThreadInitSem` semaphore
   handshake (reference `netInterface.c:61-63`) is mirrored
   verbatim: caller `open()` blocks on the EP6 thread's
   `init_sem` until WSAEventSelect setup completes. No
   "operationally equivalent" / "operator preference" /
   "recommended" framing remains.

2. **Stage 4/5 sink-dispatch shape + chunk granularity
   (Change B).** Pre-Step-14 verification of the §5.8 Router
   callback-dispatch question (recorded in the new "Verification
   verdict" block in Stage 4 below) confirms Lyra's
   `std::function<void(int, const double*)>` dispatch is
   semantically equivalent to the reference's xrouter
   function-id dispatch for ALL known sink consumers — RX1 IQ,
   RX2 IQ, mic Inbound, PS feedback (via `xrouter` source-1
   twist), HW-PTT, telemetry — AND for the future PS calcc
   thread per the v0.3 PureSignal arc. The §5.8 deviation
   stays SIGNED-AS-IS (Stage 2A commit `2b78d9e`). No code
   revert needed pre-Step-14. Chunk granularity is now stated
   plainly: 2×19 samples per datagram per the reference's
   per-USB-frame dispatch at `networkproto1.c:470` + `:550`;
   WDSP consumes whatever the reference produces. No "WDSP has
   its own ring so should be fine" hedging.

3. **Seq-counter continuity in Stage 2 (Change C).** `txSeq_`
   is seeded from `metis_out_seq_num()` after `prime()` returns
   so the gateware observes a single continuous
   `MetisOutBoundSeqNum` across the priming pass and the
   steady-state EP2 path. Matches reference's single-counter
   posture at `networkproto1.c:30, 221, 231` — period. Because
   Change A moves priming into the EP6 thread the seed point
   shifts (it executes after the init-sem releases and before
   `txWorker_` takes its first composeCC pass); the invariant
   is **"ONE counter across priming and steady-state,"** not
   the specific seed point.

§5.8 verification finding (in support of Change B): the
reference's xrouter function-id table is two indirection arrays
(`function[port][i][ctrl]` selects Inbound vs InboundBlock;
`callid[port][i][ctrl]` is the WDSP channel id) populated at
runtime by `LoadRouterAll(...)` from
`Console/cmaster.cs::CMLoadRouterAll(HPSDRModel)`. Lyra's
`std::function` collapses both indirections into the registered
closure (the closure captures both the dispatch flavor and the
channel-id binding). The (port × call_idx × ctrl_word) table
structure is preserved at `Router::sinks[port][call_idx]
[ctrl_word]`; the `set_control_word()` runtime mutation
primitive (load-bearing for v0.3 PS state-product re-routing)
is preserved at `Router.cpp:89-93`. PS calcc consumes its inputs
through the existing channel-master `Inbound()` path; the
Lyra-native equivalent is `register_sink(...)` whose closure
pushes into the PS calcc input ring — same dispatch semantics,
same per-port × per-ctrl-word table, same `ctrl` axis. **Verdict:
SAFE-AS-IS.** One non-blocking flag for the v0.3 PS port team:
`kRouterMaxCtrlWords` is currently 4 (`Router.h:43`) while
reference `rtNVAR` is 8 (`router.h:30`); if v0.3 PS exercises
the full 8-slot ctrl-word axis the constant needs a one-line
bump. No Rule-3 implications; no action in Step 14.

## Pre-Plan Verification Of The Reference Session-Open Sequence

Before laying out stages, here is the verified reference session-open flow for **HL2 / Protocol-1 / Hermes-Lite operating point**, file:line grounded so the stage gates can cite it.

The reference path is **`StartAudioNative()` (`netInterface.c:32-94`) → branch on `RadioProtocol`**. Important note: `RadioProtocol == USB` selects the **Metis/EP6** branch (`netInterface.c:47-74`), not "real USB" — that constant is just the legacy name. The **else** branch (`netInterface.c:76-85`) is a different Metis variant via `StartReadThread()` that the HL2 driver does NOT take. HL2 P1 always lands in the USB-named/Metis branch. So the canonical HL2-P1 open sequence is:

```
StartAudioNative()                               netInterface.c:32
  audio_running = 1
  HaveSync = 1                                   netInterface.c:38
  if (IOThreadRunning || prn == NULL) return 3   netInterface.c:40       ← prn must already be valid here
  UpdateRadioProtocolSampleSize()                netInterface.c:45
  cm_asioStart(RadioProtocol)                    netInterface.c:46
  // USB branch (HL2)
  rc = SendStartToMetis()                        netInterface.c:50
       └── ForceCandCFrame(1) ×5 + sendPacket+MetisReadDirect loop
                                                 networkproto1.c:33-69
  prn->hReadThreadInitSem = CreateSemaphore(...)
  prn->hReadThreadMain = _beginthreadex(MetisReadThreadMain)
                                                 netInterface.c:61
  WaitForSingleObject(hReadThreadInitSem, INF)   netInterface.c:63
  prn->hWriteThreadInitSem = CreateSemaphore(...)
  prn->hWriteThreadMain = _beginthreadex(sendProtocol1Samples)
                                                 netInterface.c:66
  prn->hsendEventHandles[0,1] = CreateSemaphore  netInterface.c:68-69
  prn->hobbuffsRun[0,1]     = CreateSemaphore    netInterface.c:70-71
```

Then **inside `MetisReadThreadMain` itself** the HL2 branch's read loop does its OWN priming before settling into the WSAEventSelect loop:

```
MetisReadThreadMainLoop_HL2()                    networkproto1.c:422
  mic_decimation_count = 0
  SeqError = 0
  FPGAReadBufp  = calloc(1024)                   networkproto1.c:427
  FPGAWriteBufp = calloc(1024)                   networkproto1.c:428
  ForceCandCFrame(3)         ← 3 priming passes  networkproto1.c:430
  prn->hDataEvent = WSACreateEvent()             networkproto1.c:433
  WSAEventSelect(listenSock, hDataEvent, FD_READ) networkproto1.c:434
  while (io_keep_running) { WSAWaitForMultipleEvents + MetisReadDirect + dispatch }
```

Key implication for the plan: **the reference's priming pass (`ForceCandCFrame(3)`) runs INSIDE the RX thread, AFTER both threads have been spawned, BEFORE the EP6 wait-loop begins.** `sendProtocol1Samples` (the EP2 send thread) is already running by then but parked on `WaitForMultipleObjects(hsendEventHandles)` (`:1220`) — it has nothing to send until `outbound_push_lr` / `outbound_push_iq` release those semaphores from upstream DSP.

So the reference's session-open order is:

1. `prn` is non-null at this point (allocated earlier — for Lyra it's the singleton `radio_net()`).
2. `cm_asioStart` (DSP wiring) — Lyra equivalent already runs in the existing WDSP engine open path; orthogonal to step 14.
3. `SendStartToMetis()` — sends the START packet to wake the gateware. **This already happens in Lyra** at `hl2_stream.cpp:2452` (the txWorkerLoop sends `buildControlPacket(true)` on entry). Order-equivalent.
4. Spawn EP6 read thread; wait on its init semaphore.
5. Spawn EP2 write thread; semaphores allocated.
6. *Inside* the EP6 read thread: allocate FPGA buffers, run `ForceCandCFrame(3)`, set up WSAEventSelect, enter wait loop.

Lyra's CURRENT `HL2Stream::open()` (`src/hl2_stream.cpp:554-672`) order is:

1. Open native UDP socket on a fresh local port.
2. Snapshot UDP stats (Task #48).
3. Defensive clear of MOX / tune / FSM state (cb58bcb).
4. Spawn `rxWorker_` jthread → `rxWorkerLoop` (the recv loop).
5. Spawn `txWorker_` jthread → `txWorkerLoop` which **inside** the thread does `sendto(START)` then enters the producer-paced sendDatagram loop.

The deviations from the reference:

- **There is NO priming-pass equivalent** today. The current `composeCC()` round-robin starts cold from `ccIdx_=0` and the gateware sees its first valid C&C frame at whatever slot 0 maps to. Reference does 3 passes of `ForceCandCFrame` (TX-VFO then RX1-VFO, 10 ms sleeps) BEFORE the read-loop main while begins.
- **`prn` is never assigned** today; the new wire layer's `prn` singleton (`src/wire/RadioNet.cpp:25 RadioNet* prn = nullptr;`) is wire-inert.
- **`metis_wire_bind()` is never called.** Today's send path uses `socket_` member + per-send `sockaddr_in dest{}` constructed in `txWorkerLoop` (`hl2_stream.cpp:2441`). The new layer's TU-scope socket/dest in `wire/MetisFrame.cpp` is wire-inert.
- **`outbound_init()` is never called.** The reference's `prn->hobbuffsRun`/`hsendEventHandles` semaphores are allocated by `StartAudioNative` *before* the write thread starts; Lyra's new `outbound_init()` mirror is wire-inert.
- **Sink registration**: today `iqSink_` (a `std::function`) is set ONCE in `main.cpp:252` before `open()` ever runs (the WDSP engine grabs it). The rx-loop calls `iqSink_(iq, n)` synchronously per datagram (`hl2_stream.cpp:1278-1280`). `micConsumer_` is set similarly via `Hl2Ep6MicSource` (`mic_source.cpp:15`). Telemetry decode writes atomics that QML polls. The new `Router::register_sink()` mechanism is wire-inert.

So step 14's job, from the reference's order:

- (a) Assign `prn` + size all `_radionet` buffers in `open()` BEFORE the threads spawn.
- (b) Bind the wire socket/dest via `metis_wire_bind()` in `open()` BEFORE threads.
- (c) Call `outbound_init()` in `open()` BEFORE threads.
- (d) Insert a `ForceCandC::prime(3, tx_freq, rx_freq)` pass — order-equivalent to the reference's `:430` priming, which means **AFTER `metis_wire_bind` but BEFORE `Ep6RecvThread::start()` enters its wait loop**.
- (e) Spawn `Ep6RecvThread` + `Ep2SendThread` (the new free-function-y classes) in place of the existing two jthreads.
- (f) Migrate sink registration: the WDSP engine's IQ callback that today is `iqSink_` becomes `Router::register_sink(router, port=0, call_idx=0, ctrl_word=0, sink)`; the mic consumer that today is `micConsumer_` becomes `Ep6RecvThread::set_mic_sink(...)`; the telemetry banner that today reads atomics becomes a subscriber on `Ep6RecvThread::set_telemetry_sink(...)` (or stays on atomics — see Stage 5 for the trade-off).

---

## Stage 1 — Wire-Layer Singleton + Bind, Wire-INERT (compile-and-link)

**Title.** Insert `prn = lyra::wire::radio_net()` + `metis_wire_bind(socket_, &dest, sizeof(dest))` + `outbound_init()` into `HL2Stream::open()` **before** the existing `rxWorker_` / `txWorker_` jthread launches. Do NOT remove anything yet. The new wire layer becomes constructible from `open()` but no new code reads `prn` for live traffic — old rxWorkerLoop/txWorkerLoop are still the live path.

**Reference provenance.**
- `prn` allocation + non-null contract before `StartAudioNative` proceeds: `netInterface.c:40` (`if (... || prn == NULL) return 3;`).
- `prn->hobbuffsRun[0,1]` + `prn->hsendEventHandles[0,1]` semaphore allocation in session-open (= Lyra's `outbound_init()`): `netInterface.c:68-71`.
- File-scope `listenSock` global = Lyra's TU-scope socket fd bound via `metis_wire_bind()`: implicit at every `sendPacket(listenSock, ...)` site (e.g. `networkproto1.c:55, 89, 234`).

**Lyra files touched.**
- `src/hl2_stream.cpp::open()` — add 3 calls between line 597 (UDP stats snapshot) and line 665 (`rxWorker_ = std::jthread(...)`):
  - `prn = lyra::wire::radio_net();` (assigns the global singleton; first time anything writes the new layer's `prn`)
  - `lyra::wire::metis_wire_bind(socket_, &dest, sizeof(dest));` — `dest` is the sockaddr_in for `targetIp_`+`kRadioPort`, hoisted from `txWorkerLoop` to `open()` (one extra ~6-line block).
  - `lyra::wire::outbound_init();` — allocates the LR/IQ outbound buffers + semaphores on `prn`.
- `src/hl2_stream.cpp::close()` — add the symmetric teardown of `prn`/wire-bind state at the end (after both jthreads joined + socket closed): a `metis_wire_bind(-1, nullptr, 0)` clear + a `prn = nullptr` (or just leave `prn` non-null but socket -1 — see Rollback Risk below).
- `src/hl2_stream.h` — `#include "wire/RadioNet.h"`, `#include "wire/MetisFrame.h"`, `#include "wire/OutboundRing.h"`.

**What stays inert / what becomes live.**
- INERT: ALL existing wire bytes. `rxWorkerLoop` still reads from `socket_` via `recvfrom`; `txWorkerLoop` still writes via `sendto` with the existing template. The new `metis_socket_fd()` returns a valid fd that nothing yet calls. `outbound_init()` allocates `prn->outLRbufp`/`outIQbufp`/`hsendLRSem`/`hsendIQSem` but nothing pushes to them and nothing waits on them.
- LIVE-but-orthogonal: only that `prn` is now a non-null singleton. Reads of `prn->` from any new free function would NOT crash — but no caller exists yet.

**Parity-check entry (`docs/architecture/PARITY_CHECKPOINTS.md`).**

> Step 14 Stage 1 — `HL2Stream::open()` assigns `prn` + calls `metis_wire_bind()` + `outbound_init()` before thread spawn. Wire bytes byte-identical to pre-step-14. Reference provenance: `netInterface.c:40-71`. Signed-off: pending bench.

**Bench gate.**
Build-clean + unit-test-clean + a 60-second `LYRA_WIRE_DEBUG`-style capture run side-by-side with HEAD: the EP2 send rate, EP6 recv rate, and seq-error counter must match HEAD within ±0.1%. **No HL2 hardware interaction differs.** Operator can do this on the existing dummy-load bench in five minutes; no MOX, no antenna.

**Rollback risk.**
**Tiny.** Three new calls, all defensively no-op on null inputs (Lyra's `metis_wire_bind` is idempotent; `outbound_init` early-returns on `prn==nullptr`; `prn = radio_net()` is a singleton). Blast radius if it breaks: build failure (caught by CI) or a startup crash if the singleton's ctor is buggy. Escape: revert the three-line `open()` insert + the 3 includes. The new wire layer files themselves stay unchanged.

---

## Stage 2 — `ForceCandC::prime(3, tx_freq, rx_freq)` Insert, Wire-LIVE on First HL2 Talk

**Title.** Insert the reference's `ForceCandCFrame(3)` priming pass into the EP6 read thread body — `Ep6RecvThread::run_loop()`, AFTER thread spawn, BEFORE the WSAEventSelect/WSAWaitForMultipleEvents loop entry. The priming pass talks REAL HL2 hardware via `metis_write_frame()`. Old workers still do the steady-state RX/TX.

This is **the first stage where any new wire-layer code emits bytes onto the wire.** It is bench-critical and gets its own commit.

**Reference order is the order, period.** The reference at `networkproto1.c:427-434` runs (1) allocate FPGA buffers → (2) `ForceCandCFrame(3)` → (3) `WSACreateEvent` → (4) `WSAEventSelect(listenSock, hDataEvent, FD_READ)` → (5) enter the wait loop. Lyra mirrors this verbatim inside the new EP6 thread body. **No order deviation; the priming pass lives in the EP6 thread, not the caller thread.**

**The hReadThreadInitSem semaphore handshake (reference `netInterface.c:61-63`) is respected verbatim.** `open()` returns to its caller (the Radio facade / Phase-2 `Radio::start()`) only AFTER the EP6 thread has completed its priming pass AND its `WSAEventSelect` setup. Add an init-semaphore mirror to `Ep6RecvThread::start()`: the caller blocks on a `std::counting_semaphore<1>` (or `std::promise<void>` + `future<void>::wait()`) that the EP6 thread releases at the point matching reference's `SetEvent(prn->hReadThreadInitSem)` — i.e. AFTER `WSAEventSelect` returns, BEFORE the wait-loop's first iteration. This matches `prn->hReadThreadInitSem` semantics at `netInterface.c:61-63`. Without this barrier, `open()` could return to its caller while the priming pass is mid-flight, breaking the reference invariant that the gateware has received a valid TX-VFO + RX1-VFO setup before any other host code (e.g. operator state setters) runs.

**The "hoist START to open()" sub-piece STAYS.** Reference's caller-thread emits `SendStartToMetis()` at `netInterface.c:50`, BEFORE the EP6 read thread is spawned at `:61`. Lyra mirrors: hoist `buildControlPacket(true)` + `::sendto(...)` from `txWorkerLoop:2451-2461` up into `open()` (right after `metis_wire_bind`, before `Ep6RecvThread::start()`). The txWorker then drops its open-time START send. So the LOCKED order in Stage 2 is:

```
HL2Stream::open() body:
  1. UDP socket open + stats snapshot + defensive state clear (existing)
  2. prn = radio_net(); metis_wire_bind(); outbound_init();        (Stage 1)
  3. SendStartToMetis equivalent — buildControlPacket(true) + sendto on socket_
     (hoisted from old txWorkerLoop; matches netInterface.c:50)
  4. Ep6RecvThread::start(socket_) ────────────────┐ caller blocks on init-sem
                                                    │
  In Ep6RecvThread::run_loop():                     │
    a. allocate FPGA buffers (calloc(1024))         │ matches networkproto1.c:427-428
    b. ForceCandCFrame(3, tx_freq, rx_freq)         │ matches networkproto1.c:430
    c. WSACreateEvent                               │ matches :433
    d. WSAEventSelect(socket, event, FD_READ)       │ matches :434
    e. init_sem.release() ──────────────────────────┘ matches SetEvent(hReadThreadInitSem)
    f. while (io_keep_running) { WSAWaitForMultipleEvents + dispatch }
  5. caller unblocks once init-sem released → continues:
     spawn Ep2SendThread (later stage)
     spawn or keep old workers (per stage)
```

**Lyra files touched.**
- `src/wire/Ep6RecvThread.h` — add `std::counting_semaphore<1> init_sem_{0};` member; `start()` takes the socket fd, returns after the thread is constructed; new `wait_initialized()` method blocks on `init_sem_.acquire()` (or fold the acquire into the tail of `start()` so the caller doesn't see it explicitly).
- `src/wire/Ep6RecvThread.cpp` — `run_loop()` body emits the 5-step priming + WSAEventSelect + `init_sem_.release()` sequence verbatim per the reference order at `networkproto1.c:427-434`. `ForceCandC` instance + invocation `fcc.prime(3, tx_freq_, rx_freq_)` lives at the priming step; the freqs are read off `prn->tx[0].frequency` / `prn->rx[0].frequency` per Stage-7's mirrored state (Stage 2 still reads off Stage-1's `txFreqHz_.load()` / `rx1FreqHz_.load()` because Stage 7 hasn't migrated setters yet — see invariant below).
- `src/hl2_stream.cpp::open()` — insert (after Stage 1's outbound_init, BEFORE the jthread spawns at line 665):
  ```
  // Hoist START packet from old txWorkerLoop to open(), matching
  // reference SendStartToMetis at netInterface.c:50.  Before
  // spawning the EP6 thread (which will do the priming pass
  // inside its run_loop).
  {
      const auto pkt = buildControlPacket(/*start=*/true);
      sockaddr_in dest_v{};
      // ... fill dest_v from targetIp_ + kRadioPort ...
      ::sendto(socket_, reinterpret_cast<const char*>(pkt.data()),
               static_cast<int>(pkt.size()), 0,
               reinterpret_cast<const sockaddr*>(&dest_v),
               sizeof(dest_v));
  }

  // Spawn the new EP6 thread; it does the priming pass + WSAEventSelect
  // inside its run_loop and releases its init-sem before entering the
  // wait loop.  open() blocks here until that release.
  ep6Thread_.start(socket_);   // blocks on init_sem until run_loop reaches WSAEventSelect-done
  ```
- `src/hl2_stream.h` — `#include "wire/Ep6RecvThread.h"`, `#include "wire/ForceCandC.h"`; add `Ep6RecvThread ep6Thread_` member (constructed in Stage 1 inert mode — see Stage 1 amendment).
- `src/hl2_stream.cpp::txWorkerLoop` — DELETE the open-time START send (lines 2451-2461). The body then enters its existing producer-paced sendto loop directly.

**Seq counter continuity (mandatory invariant, single counter across priming + steady-state).**
`txSeq_` is seeded from `metis_out_seq_num()` after `prime()` returns so the gateware observes a single continuous `MetisOutBoundSeqNum` across the priming pass and the steady-state EP2 path. Matches reference's single-counter posture at `networkproto1.c:30, 221, 231` — period. Because the priming pass now runs inside the EP6 thread (Change A above) the seed-from-prime ordering shifts: the seed must execute AFTER the EP6 init-sem releases (i.e. after `prime()` returns inside `run_loop()` and the init-sem is signalled) but BEFORE the old `txWorker_` jthread takes its first composeCC pass. The cleanest implementation: have `Ep6RecvThread::wait_initialized()` (the init-sem-acquire that `open()` blocks on) return; `open()` then immediately reads `lyra::wire::metis_out_seq_num()` and writes it into `txSeq_` before spawning `txWorker_`. The invariant is "ONE counter across priming and steady-state," not the specific seed point.

**What stays inert / what becomes live.**
- LIVE: the 6 priming-pass datagrams (3 passes × 2 frames). Each is a 1024-byte payload composed by `ForceCandCFrames` and emitted by `metis_write_frame(2, buf)` — `g_metis_out_seq_num` increments 6 times (post-increment per reference, so the first emitted frame ships seq=0), 6 sendto() calls on the TU-scope socket, ~20 ms wall-clock total (3 passes × 2 frames × ~negligible + 2 × 10 ms sleeps + 1 trailing 10 ms sleep, per `ForceCandCFrame:136,138`). LIVE: the WSAEventSelect setup that follows.
- INERT: the EP6 wait loop itself is structurally INERT in Stage 2 (no sinks registered yet — Stage 3/4/5 territory). The thread parks in `WSAWaitForMultipleEvents` and either spins parsing-but-discarding (per Stage 3's tap pattern) or simply does not register sinks. **Stage 2 ONLY adds the priming pass + START hoist + thread start; sink wiring is later.** Wait — this conflicts with the original Stage-3 inline-tap design. Revised: **Stage 2 spawns `Ep6RecvThread` early (Stage 1's inert construction was already in `open()`), and the new thread becomes the actual recv path in one step.** That requires Stage 3's inline-tap pattern to fold INTO Stage 2 or be revisited. Operator should review whether Stage 2 spawns the EP6 thread as the live recv path (collapsing Stage 3+6 into Stage 2) or if Stage 2 leaves the old `rxWorker_` jthread as the live recv path and the new EP6 thread only does the priming-and-park dance (still possible — `WSAWaitForMultipleEvents` on a socket that another thread is also `recvfrom`-ing splits datagrams 50/50 per the Stage 3 warning; mitigation: the new EP6 thread sets up WSAEventSelect for the priming-pass-side ONLY, then loops on an event that never fires until Stage 6 wires it). The CLEANEST path is to fold Stage 3's tap discipline into Stage 2 (i.e. priming pass + WSAEventSelect happen on the new thread, but the new thread DOES NOT call `recvfrom` until Stage 6 wires it as the live recv path; Stage 3's tap pattern still runs from `rxWorker_` inline). **This is a stage-boundary call for the operator; the priming-pass location is locked, the wait-loop wiring is the open question.**
- INERT: everything past the priming pass — `g_metis_out_seq_num` is now the single outbound seq counter; the old `txWorker_` jthread reads its seed from `metis_out_seq_num()` after `prime()` returns and continues from there.

**Parity-check entry.**

> Step 14 Stage 2 — `Ep6RecvThread::run_loop()` body emits the 5-step priming + WSAEventSelect + init-sem release sequence verbatim per reference order at `networkproto1.c:427-434`. START packet hoisted from `txWorkerLoop` to `open()` (matches `netInterface.c:50`). `open()` blocks on `Ep6RecvThread`'s init-sem until WSAEventSelect setup completes (matches `prn->hReadThreadInitSem` at `netInterface.c:61-63`). `txSeq_` seeded from `metis_out_seq_num()` after init-sem releases, preserving one continuous outbound seq counter. Wire bytes: 6 priming datagrams shipped before the first steady-state EP2 frame, gateware receives valid TX-VFO + RX-VFO setup before any audio. Reference provenance: `networkproto1.c:106-139, 427-434`; `netInterface.c:50, 61-63`. Signed-off: pending bench.

**Bench gate.**
**This is THE bench gate** — first time the new wire layer talks to real HL2 on the operator's bench. Operator hooks up dummy load, opens the stream cold (cold = HL2 powered up but not currently streamed), expects:

- Lyra opens stream cleanly (no fatal-error path).
- EP6 datagrams arrive at the expected ~5050 dg/s rate (RX is unaffected — same composeCC round-robin in the old txWorker).
- RX1 audio works: tune to a known signal (e.g. operator's local AM bcst or a band where carriers are present at the moment), confirm the panadapter shows it at the right place, confirm audio comes through the AK4951.
- Telemetry banner updates (PA current, supply, temp).
- No abnormal seq errors, framing errors, or audio glitches over a 5-minute run.

The priming pass is RX-bias-neutral — it sets TX-VFO + RX1-VFO to the operator's currently-tuned freqs. So if the operator was already on 14.250 MHz USB before the previous session closed, the priming frames set DDC0 to 14.250 MHz before the steady-state cycle takes over. This stage should be **byte-equivalent or better** for RX traffic vs HEAD — there is no scenario where 6 priming frames worsen RX.

**Rollback risk.**
**Moderate.** The priming pass is short and well-bounded but if `prime()` or `metis_write_frame()` has a defect (e.g. wrong sock fd, wrong dest addr, payload composition off-by-one) the gateware may receive corrupt C&C bytes and `setRx1FreqHz` / mode behavior could be wrong on first key. Operator's escape: revert Stage 2's commit; Stage 1 stays (it's inert). Blast radius limited to the 20ms priming window — old txWorker steady state immediately retakes the wire after.

If Stage 2 fails the bench (no RX audio / wrong-band tuning / framing errors) the operator reverts to Stage 1 commit and the project is exactly at the Stage-1 inert state.

---

## Stage 3 — Stand Up `Ep6RecvThread` In Parallel, Mute Its Sinks, Read-Only Compare

**Title.** Construct `Ep6RecvThread` in `HL2Stream::open()` and call `start(socket_)`. Do NOT yet register any sinks on its `Router*` / mic sink / telemetry sink — leave them null so the new thread parses every EP6 datagram into `prn->RxBuff`/`prn->TxReadBufp` but DOES NOT dispatch anywhere. The existing `rxWorker_` jthread still runs side-by-side and IS still the live path.

This stage validates the new EP6 parser's correctness against real wire traffic, with the new wire layer reading the SAME UDP socket as the old.

**WARNING — Windows recvfrom-on-shared-socket caveat:** Winsock allows two threads to read from one UDP socket concurrently, but each datagram is delivered to exactly ONE recv call. So if `Ep6RecvThread` and `rxWorkerLoop` are both blocking in `recvfrom` on `socket_`, datagrams get split between them roughly 50/50 — each thread sees half. This BREAKS the existing rxWorkerLoop's iqSink dispatch (panadapter goes to ~half rate). **Stage 3 must either (a) duplicate the socket — `WSADuplicateSocketW` + `socket()` create a sibling fd that receives the same gateware traffic ONLY IF the bind is to the multicast/per-port — which it is NOT, so this doesn't work for HL2; or (b) tap inline in `rxWorkerLoop` after recvfrom — call `Ep6RecvThread::process_datagram()` (a method that takes a pre-received buffer) on the SAME thread, before the old `iqSink_` dispatch.**

Option (b) is the right approach for this stage. The new `Ep6RecvThread::process_datagram` is already structured as a function that takes `(const uint8_t* data, std::size_t size)` (`Ep6RecvThread.h:152`). For Stage 3, expose it publicly and call it from `rxWorkerLoop` after the existing datagram validation, BEFORE the existing iqSink + telemetry path. The new thread's actual `start()` (which runs WSAEventSelect on its own) does NOT spin in Stage 3 — only `process_datagram()` is invoked from the existing rx-loop. The whole `Ep6RecvThread` thread machinery sits dormant until Stage 4.

Re-stating the stage with this correction: Stage 3 introduces a **parser tap** — every datagram the old rxWorkerLoop receives is ALSO parsed by the new `Ep6RecvThread::process_datagram()`, but no new sinks fire (the new thread's `router_`/mic_sink_/telemetry_sink_ are null). The two parsers run in parallel, and the operator can A/B their decoded results.

**Reference provenance.**
- `MetisReadThreadMainLoop_HL2` body: `networkproto1.c:422-586`.
- Per-datagram USB-frame loop + sync-header check: `networkproto1.c:439-473`.
- Per-DDC IQ unpack: `:528-542`.
- Mic decimation + `Inbound(inid(1,0), ...)`: `:562-579`.

**Lyra files touched.**
- `src/wire/Ep6RecvThread.h` — promote `process_datagram(const uint8_t*, size_t)` from private to public (or add a public `parse_one(...)` wrapper).
- `src/hl2_stream.h` — add an `Ep6RecvThread tap_;` member.
- `src/hl2_stream.cpp::open()` — instantiate `tap_` (no `start()` call).
- `src/hl2_stream.cpp::rxWorkerLoop` — after the existing framing checks pass and `totalDg_` increments (`hl2_stream.cpp:1068`), call `tap_.process_datagram(u, n);`. Old iqSink path continues unchanged.

**What stays inert / what becomes live.**
- LIVE: `Ep6RecvThread::process_datagram` runs on every datagram, decoding C0..C4 status + per-DDC IQ into `prn->RxBuff[]` and mic into `prn->TxReadBufp[]`. Decoded telemetry C0-class switch (`networkproto1.c:498-524`) writes into `prn->ptt_in`, `prn->adc[0].adc_overload`, `prn->tx[0].fwd_power`, etc. Sequence-number tracking in the new TU-scope `MetisLastRecvSeq` / `SeqError` statics advances.
- INERT: no new sink fires. The `Router*` is null, so the new code's `xrouter(0, 0, 0, spr, prn->RxBuff[0])` call at the equivalent of `:550` is a no-op. The mic sink is null, so the `Inbound(inid(1,0), ...)` mirror is a no-op. Telemetry sink is null. **No new behavior is observable from QML or from any operator-facing surface.**

**Parity-check entry.**

> Step 14 Stage 3 — `Ep6RecvThread::process_datagram()` tapped inline in old `rxWorkerLoop`; new parser decodes every datagram into `prn->RxBuff/TxReadBufp` + TU-scope SeqError. All sinks null. Wire bytes unchanged. Reference provenance: `networkproto1.c:422-586`. Signed-off: pending bench.

**Bench gate.**
Build-clean + a 5-minute capture with `LYRA_PARSER_COMPARE_DEBUG=1`-style env var (add a diagnostic that compares `prn->ptt_in` against the existing `lastPttIn_`, `prn->tx[0].fwd_power` against `telFwdRaw_`, sequence-error counts against `seqErrors_`). Operator confirms the two parsers agree byte-for-byte over a multi-minute run. **No HL2 RF behavior changes vs Stage 2.**

**Rollback risk.**
**Tiny.** Inline tap adds ~5 μs per datagram (a parse pass that does no allocation; it writes to `prn->*` buffers that nothing reads). Worst case: a defect in the new parser corrupts `prn->RxBuff` — but `prn->RxBuff` isn't read anywhere yet, so no observable effect. Escape: comment out the `tap_.process_datagram(...)` line, rebuild.

---

## Stage 4 — Migrate The First Consumer (WDSP IQ Sink) Off `iqSink_` Onto `Router::register_sink()`

**Title.** Replace the WDSP engine's `stream->setIqSink([wdspEngine](const double *iq, int n) { ... });` (`main.cpp:252`) with `lyra::wire::register_sink(lyra::wire::router_instance(0), /*port=*/0, /*call_idx=*/0, /*ctrl_word=*/0, [wdspEngine](int n, const double* iq) { ... })`. Then **delete** the inline `iqSink_(iqScratch, kFramesPerDatagram)` call at `hl2_stream.cpp:1278-1280`, and in its place make `Ep6RecvThread::process_datagram`'s xrouter dispatch (still happening on the same rx-loop thread via the Stage-3 tap) fire the WDSP sink.

This is the first stage where the new Router's `register_sink` table becomes the live dispatch for RX1 IQ.

**Reference provenance.**
- `xrouter(0, 0, source, 2*nsamples, prn->RxReadBufp)` invocation from `twist`: `networkproto1.c:273`.
- `xrouter` direct invocation for HL2 nddc=4 (a) `xrouter(0,0,0, spr, prn->RxBuff[0])` for DDC0 (RX1) at `:550`, (b) `xrouter(0,0,2, spr, prn->RxBuff[1])` for DDC1 (RX2) at `:552`, (c) `twist(spr, 2, 3, 1)` paired DDC2/3 for PS at `:551`. **HL2 nddc=4 case branch starts at `:549`.**
- The `xrouter` dispatch by callback function-id (1 = `Inbound`, 2 = `InboundBlock`): `router.c:71-108`. Lyra's `std::function<void(int, const double*)>` callback IS semantically equivalent to the reference's xrouter function-id dispatch for ALL known sink consumers — verified across the §1-C Stage 2A audit (commit `2b78d9e`) and the pre-Step-14 verification of the §5.8 / Router callback question.

**Verification verdict (Stage 4 sink-registration shape, recorded here so Stage 14 doesn't re-litigate it):** The reference's `xrouter` body dispatches by `switch (a->function[bport][i][ctrl])`: case 1 → `Inbound(a->callid[bport][i][ctrl], nsamples, data)`, case 2 → `InboundBlock(a->callid[bport][i][ctrl], sps, ptrs)` (`router.c:84-105`). `function[]` is a "which channel-master API to call" selector (Inbound vs InboundBlock); `callid[]` is the WDSP channel id (`id(0,0)` = RX1, `id(1,0)` = TX1, `id(2,0)` = RX2; PS calcc consumes its inputs as additional Inbound destinations bound to PS-feedback channel ids). The reference's runtime population happens via `LoadRouterAll(...)` from `Console/cmaster.cs::CMLoadRouterAll(HPSDRModel)` (`cmaster.cs:561+`), which is a per-radio-model `switch` that writes the `function[port][i][ctrl]` + `callid[port][i][ctrl]` arrays.

Lyra's `std::function<void(int n, const double* iq)>` collapses this two-table indirection into ONE callable: each registered sink is a closure that already captures BOTH the dispatch flavor (Inbound vs InboundBlock — for RX1/RX2 it's always Inbound; InboundBlock is the de-interleaving variant used when `nstreams[bport] > 1`) AND the channel-id binding (the closure either calls `wdspEngine->processIq(channel_id, n, iq)` for RX-side consumers, or the equivalent PS-feedback push for the PS calcc thread). The (port × call_idx × ctrl_word) table structure is preserved verbatim (`Router::sinks[port][call_idx][ctrl_word]` at `Router.h:68`). The `set_control_word()` runtime mutation primitive is preserved (`Router.cpp:89-93`) — that is the load-bearing piece for v0.3 PS state-product re-routing.

For the PureSignal calcc thread (Phase 5 in the v0.3 PS arc): calcc does NOT bind directly to xrouter — it consumes samples that arrive through the existing channel-master `Inbound()` path (`cmbuffs.c::Inbound` → CM ring → cmaster pump → `fexchange0`). The Lyra-native equivalent is a `register_sink(router, port, call_idx, ctrl_word=PS_state_value, sink)` whose closure pushes the IQ pair into the PS calcc input ring. Same dispatch semantics, same per-port × per-ctrl-word table, same `ctrl` axis. **Verdict: SAFE-AS-IS.** Lyra's `std::function` dispatch is semantically equivalent to xrouter function-id dispatch for every known and projected sink consumer, including future PS calcc per the v0.3 PureSignal arc — per the §5.8 sign-off + this Step-14 verification pass.

One caveat the operator should be aware of (NOT a blocker, NOT a Rule-3 violation, just a capacity question that may need a 1-line bump before v0.3 PS lands): the Router's `kRouterMaxCtrlWords` constant is currently 4 (`Router.h:43`) while reference `rtNVAR` is 8 (`router.h:30`). The ctrl-word axis is the runtime-mutable re-routing dimension PS uses to swap consumer mappings on the (MOX × ps_armed) state product. v0.3 PS may need 8 ctrl-word slots if it exercises the full `rtNVAR=8` axis. Bumping `kRouterMaxCtrlWords` from 4 to 8 is a one-line constant change with no Rule-3 implications. Flag for the v0.3 port team but no action in Step 14.
- Capability struct for HL2 nddc=4 routing: per CLAUDE.md §6.7 discipline #6 + this code's `RadioCapabilities` in `lyra/protocol/p1_hl2.py` for HL2:
  - `source 0 → host channel 0 (RX1, DDC0)` — DDC0 single-stream xrouter dispatch.
  - `source 1 → host channel 3 (PS inner pair, DDC2+DDC3 twisted)` — gateware-disabled on HL2, slots are zeros.
  - `source 2 → host channel 2 (RX2, DDC1)` — RX2 future-Phase work, not this step.

So for Step 14's HL2-only scope: `register_sink(router, /*port=*/0, /*call_idx=*/0, /*ctrl_word=*/0, wdsp_iq_callback)` — the WDSP engine consumes source-0 (= DDC0 = RX1).

**Lyra files touched.**
- `src/main.cpp:252-281` — replace `stream->setIqSink(...)` with `lyra::wire::register_sink(lyra::wire::router_instance(0), 0, 0, 0, [wdspEngine](int n, const double* iq){ ... });`. Same lambda body (calls WDSP `processIq`).
- `src/wire/Ep6RecvThread.cpp` — confirm the existing `process_datagram` / nddc-switch already calls `xrouter(router_, router_id_, 0, spr, prn->RxBuff[0])` for source 0 (per `networkproto1.c:550`). It should — this is the §5 reference port. Verify.
- `src/hl2_stream.cpp::rxWorkerLoop:1276-1280` — **delete** the `if (iqSink_) iqSink_(...)` block. The new dispatch fires from inside `Ep6RecvThread::process_datagram` via xrouter → registered sink.
- `src/hl2_stream.h` — also wire `Ep6RecvThread::set_router(...)` from `open()`: pass `lyra::wire::router_instance(0)` to the tap so `xrouter` knows where to dispatch. (Today the `tap_` was constructed in Stage 3 without setting a router; Stage 4 sets it.)
- `src/hl2_stream.h:599-602` — `setIqSink` method is now unreferenced; mark deprecated but keep the symbol (sink-less callers, if any, still link). Removal happens in Stage 8.

**What stays inert / what becomes live.**
- LIVE: WDSP RX1 audio path now flows EP6 → `Ep6RecvThread::process_datagram` (called inline by old rxWorker tap) → unpack to `prn->RxBuff[0]` → `xrouter(0, 0, 0, spr, prn->RxBuff[0])` → registered sink → WDSP `processIq()`. This is byte-identical to the old path in terms of the IQ doubles delivered (same scale: `1/2^31`), same buffer layout (interleaved I,Q,I,Q), same per-datagram cadence.
- **Chunk granularity is 2×19 samples per datagram per the reference's per-USB-frame xrouter dispatch at `networkproto1.c:470` + `:550`.** The reference dispatches one xrouter call per USB frame inside the two-frame loop in `MetisReadThreadMainLoop_HL2` (`:443-580`). spr = `504 / stride` samples per frame; for nddc=4 stride is `6*4+2 = 26` so spr = 19. Two USB frames per datagram → 2×19 = 38 samples total per datagram, but dispatched as **two 19-sample xrouter calls**, not one 38-sample call. The OLD `iqSink_` path bundled both frames into a single 38-sample scratch (`kFramesPerDatagram = 38` at `hl2_stream.cpp:1189-1190`) which is a Lyra-native deviation from the reference dispatch cadence. Stage 4 retires that bundling and matches the reference's per-USB-frame dispatch verbatim. WDSP consumes whatever the reference produces. **The total sample rate at WDSP's input is identical (~5050 datagrams/s × 2 frames × 19 samples = ~192000 samples/s);** only the chunk-size at each `Inbound` call changes from 1×38 to 2×19, and that change brings Lyra to reference parity, not away from it.
- INERT: mic / telemetry / PTT-in still go through the old `rxWorkerLoop` paths in `hl2_stream.cpp` (the existing `setMicConsumer` path, the existing telemetry atomics, the existing `hwPttEnabled_` forwarder). They migrate in Stage 5.

**Parity-check entry.**

> Step 14 Stage 4 — WDSP RX1 IQ sink migrated from `HL2Stream::iqSink_` to `Router::register_sink(router_instance(0), port=0, call_idx=0, ctrl_word=0, ...)`. Dispatch path now: EP6 → `Ep6RecvThread::process_datagram` → `xrouter(0,0,0, spr, prn->RxBuff[0])` → WDSP. Sample data byte-identical to pre-step-14; chunk granularity changes 1×38 → 2×19 per datagram (reference-faithful). Mic / telemetry / PTT-in unchanged. Reference provenance: `networkproto1.c:549-558` (nddc=4 dispatch). Signed-off: pending bench.

**Bench gate.**
Operator: same dummy-load setup as Stage 2; tune to a known signal (a known SSB QSO, a known AM bcst, or a steady carrier from another local SDR), confirm:

- RX audio quality is identical to HEAD (no crackle, no chunky stutter, no level shift).
- Panadapter is identical (same noise floor, same signal-level placement).
- All WDSP DSP modes (USB / LSB / AM / FM / NR / ANF / NB / AGC) work normally.
- 30-minute soak with no audio underrun increment, no Stream errors.

If WDSP audio sounds different (chunkier, more underruns, weird AGC behavior) the cause is the 1×38 → 2×19 chunk migration. WDSP should handle it but bench-confirm.

**Rollback risk.**
**Moderate.** This is the first stage where the live RX audio path actually changes. If anything goes wrong (no audio, wrong sideband, framing errors propagated), the operator's listening experience is broken until they revert. Escape: revert Stage 4's commit, rebuild — Stage 3's tap stays inert, Stages 1-2 stay live (priming pass + prn assignment), and the old `iqSink_` path resumes as the live consumer.

---

## Stage 5 — Migrate Mic + Telemetry + PTT-In Consumers Off `HL2Stream` Onto `Ep6RecvThread::set_*_sink()`

**Title.** Move the remaining EP6-derived consumers from the old `HL2Stream` member-method delivery paths to the new `Ep6RecvThread` sink callbacks.

**Three migrations, one stage** because they share the Stage-4 routing pattern:

- **Mic samples.** Today: `micConsumer_` registered by `Hl2Ep6MicSource::Hl2Ep6MicSource(stream)` (`mic_source.cpp:15`). Fires synchronously on rxWorkerLoop thread per datagram (`hl2_stream.cpp:1293-1298`). Tomorrow: `Ep6RecvThread::set_mic_sink(...)` (`Ep6RecvThread.h:137`). Reference mirror: `Inbound(inid(1,0), mic_sample_count, prn->TxReadBufp)` at `networkproto1.c:579`. The callback signature differs slightly (today: `(const float*, int)` of int16-scaled mic; tomorrow per `Ep6MicSink` typedef: `(int n_samples, const double* iq_pairs)` with I=mic-normalized, Q=0). Operator should decide whether `Hl2Ep6MicSource` does the float↔double conversion or whether the wire layer matches the old signature. Reference posture: doubles with Q=0 per `networkproto1.c:570-573`. Lyra-native: doubles match TX-DSP-worker expectations; convert at the consumer.
- **Telemetry banner.** Today: rxWorkerLoop decodes C0..C4 status into `telA0c12Raw_` / `telSupplyRaw_` / `telTempRaw_` / `telFwdRaw_` / `telRevRaw_` / `telPaCurRaw_` atomics (`hl2_stream.cpp:1089-1131`). QML polls via stats signal. Tomorrow: `Ep6RecvThread::set_telemetry_sink(...)` (`Ep6RecvThread.h:136`) fires per status frame with an `Ep6Telemetry` struct carrying (class_id, control[4]). Two options:
  - **Migrate.** Replace the atomic-set in rxWorkerLoop with a telemetry sink that does the same atomic-set in HL2Stream. Same external observable.
  - **Don't migrate (stay on atomics).** The reference also uses field-scope globals (`prn->tx[0].fwd_power`, `prn->adc[0].adc_overload`) that are atomic-ish writes; Lyra's existing telemetry atomics ARE the reference equivalent. Stage 5 could simply route the new Ep6RecvThread's decode pass to write the SAME atomics. The sink-callback indirection adds nothing.

  **Recommendation: don't migrate.** Telemetry is fundamentally globally-published state. Keep the atomic-set inline in the new parser (already in `Ep6RecvThread::decode_status_header`); QML keeps reading the same atomics. The optional telemetry sink stays for future consumers that want a push-style stream (e.g. a future logger).
- **HW PTT-in forwarder.** Today: rxWorkerLoop's bit-0 edge detect at `hl2_stream.cpp:1157-1179` fires `QMetaObject::invokeMethod(this, "requestMoxFromHwPtt", QueuedConnection, ...)`. Tomorrow: same logic moves to `Ep6RecvThread::decode_status_header` — but the FSM dispatch needs a way to reach `HL2Stream::requestMoxFromHwPtt`. Cleanest: telemetry sink fires with the new ptt_in level, `HL2Stream` subscribes via a small `Ep6PttSink` callback (or piggybacks on telemetry_sink) and does the edge detect + `invokeMethod`. The hwPttEnabled_ opt-in gate stays at the HL2Stream level.

After this stage the OLD `rxWorkerLoop`'s per-slot decode and per-frame status decode no longer fires for any operator-observable purpose. But the OLD `rxWorkerLoop` thread still EXISTS and still calls `recvfrom` — it's now purely a transport layer for the inline tap call into `Ep6RecvThread::process_datagram`. Stage 6 deletes the loop entirely.

**Reference provenance.**
- `Inbound(inid(1,0), mic_sample_count, prn->TxReadBufp)`: `networkproto1.c:579`.
- C0-class telemetry decode switch: `:499-524`.
- HW PTT bit (C0 bit 0) at `prn->ptt_in = ControlBytesIn[0] & 0x1`: `:496`.

**Lyra files touched.**
- `src/mic_source.cpp` — `Hl2Ep6MicSource` ctor calls `Ep6RecvThread::set_mic_sink(...)` instead of `stream.setMicConsumer(...)`. Needs a getter on `HL2Stream` to reach the `tap_` member.
- `src/hl2_stream.cpp::rxWorkerLoop` — DELETE the per-slot mic accumulation (`micAcc`, `micIdx`, `micScratch` — lines 979-988, 1196-1197, 1233-1273, 1293-1298). DELETE the C0-class telemetry decode block (lines 1071-1131). DELETE the HW PTT-in edge detect block (lines 1133-1179). Each block's responsibility now lives in `Ep6RecvThread::process_datagram`.
- `src/hl2_stream.cpp::rxWorkerLoop` — the IQ accumulation for the RX1 dBFS readout (`rmsAcc`, `kRmsWindowSamples`, lines 938-963, 1221-1231) is HL2Stream-internal state used to emit the QML banner. Easiest path: register a Lyra-native side-tap on the router (a second `register_sink` callback at port=0, call_idx=1, that computes RMS) and delete the inline accumulator. Or leave it inline — it's purely an aggregation pass over the same RxBuff doubles. Recommendation: side-tap, for parity with the rest of the migration.

**What stays inert / what becomes live.**
- LIVE: mic samples now reach `Hl2Ep6MicSource` via `Ep6RecvThread::set_mic_sink` (same data, possibly different scaling — operator confirms).
- LIVE: telemetry atomics still get set, but from inside `Ep6RecvThread::decode_status_header` rather than the old rxWorkerLoop block. Same atomics, same QML observables.
- LIVE: HW PTT-in edge detect still fires `requestMoxFromHwPtt` via QueuedConnection, but the edge detect runs inside `Ep6RecvThread::decode_status_header`.
- INERT: the old `rxWorkerLoop` thread still spins, still calls `recvfrom`, still calls `tap_.process_datagram()`. Everything past that in the loop is a no-op (no iqSink, no mic accumulator, no telemetry decode). The thread is structurally now a thin reader feeding a parser tap.

**Parity-check entry.**

> Step 14 Stage 5 — Mic / telemetry / HW-PTT-in consumers migrated from `HL2Stream::rxWorkerLoop` inline decode blocks to `Ep6RecvThread` sink callbacks + inline telemetry-atomic writes. Old rxWorkerLoop thread retained as transport-only tap (no observable consumers run inside it). Wire bytes unchanged. Reference provenance: `networkproto1.c:496-524, 579`. Signed-off: pending bench.

**Bench gate.**
Operator: 30-minute soak. Verify mic level meter (`micDbFs_`) reads correctly during voice. Verify PA-current / supply / temp banner updates. Verify foot-switch HW PTT (if opted-in) still keys MOX. Verify panadapter unchanged. No new audio glitches.

**Rollback risk.**
**Moderate.** Telemetry and PTT-in are operator-observable; mic-level meter is operator-observable. A defect in any sink path breaks one of these surfaces but leaves the rest working. Escape: revert Stage 5 commit; rxWorkerLoop's inline decode returns to live status, Stage 4's IQ-sink router migration stays live.

---

## Stage 6 — Activate `Ep6RecvThread::start()` As Its Own std::thread, Delete `HL2Stream::rxWorkerLoop`

**Title.** Replace the Stage-3 inline-tap pattern with the new layer's own thread machinery. `Ep6RecvThread::start(socket_fd)` spawns its own `std::thread` running the WSAEventSelect-driven `run_loop()` (`Ep6RecvThread.h:101-118`, `Ep6RecvThread.cpp:172-179` shows the binding). The old `HL2Stream::rxWorker_` jthread and `rxWorkerLoop` method are deleted.

This is the structural cleanup step: after Stage 5 the old rxWorkerLoop was a thin transport-only shim; Stage 6 retires it.

**Reference provenance.**
- `MetisReadThreadMain` thread entry + main loop dispatch: `networkproto1.c:240-261`.
- `MetisReadThreadMainLoop_HL2` body (the actual recv loop, now lives in `Ep6RecvThread::run_loop` via `process_datagram`): `:422-586`.
- WSAEventSelect + WSAWaitForMultipleEvents primitive in the reference loop: `:434, 443-453`.

**Lyra files touched.**
- `src/hl2_stream.cpp::open()` — replace the `rxWorker_ = std::jthread([...]{rxWorkerLoop(...);})` line at 666-668 with `tap_.start(socket_);` (where `tap_` is now repurposed to be the live RX thread, NOT just an inline-tap parser).
- `src/hl2_stream.cpp::close()` — replace the `rxWorker_.request_stop(); rxWorker_.join();` lines at 738+741 with `tap_.stop();`. The Ep6RecvThread already implements correct stop/join.
- `src/hl2_stream.h` — delete `rxWorker_` member, delete `rxWorkerLoop` declaration, delete `iqSink_` / `setIqSink` / `setMicConsumer` / `micConsumer_` / `micConsumerMtx_` (all unused after stages 4+5). Repurpose `tap_` → `ep6Thread_`.
- `src/hl2_stream.cpp` — DELETE `rxWorkerLoop` body (lines 921-1300, ~380 lines).

**What stays inert / what becomes live.**
- LIVE: the new `Ep6RecvThread` now owns the EP6 socket recv. Its WSAEventSelect loop is identical-in-shape to the reference's `:443-453`. It calls `metis_wire_bind(socket_fd, nullptr, 0)` at start (`Ep6RecvThread.cpp:179`) — this is **the same wire-bind that Stage 1 did from `open()`**. It's idempotent (no-op if already bound to the same values) per the `metis_wire_bind` contract documented in `MetisFrame.h:50-55`. Good.
- INERT: old `rxWorker_` jthread and its loop don't exist anymore.

The HL2Stream class is now structurally a "TX worker + new EP6 thread + state owner" rather than its old "RX worker + TX worker" shape. The TX worker still does the round-robin C&C composeCC — that migrates next.

**Parity-check entry.**

> Step 14 Stage 6 — `Ep6RecvThread::start(socket_fd)` becomes the live EP6 recv thread; old `HL2Stream::rxWorker_` jthread + `rxWorkerLoop` deleted. WSAEventSelect-driven event loop matches reference `networkproto1.c:443-453`. RX data flow byte-identical to Stage 5 (same parser, just different thread). Wire bytes unchanged. Reference provenance: `networkproto1.c:240-261, 422-586`. Signed-off: pending bench.

**Bench gate.**
Operator: same 30-minute soak. Plus a stop/restart cycle test: stop the stream, start it again, confirm RX comes up cleanly (no dead-RX, no stuck mic, no spurious MOX — § cb58bcb hardening still holds). The new `Ep6RecvThread::stop()` MUST join cleanly within `close()`'s implicit time budget (the existing close() has no explicit timeout; the new thread should respect the same bounded-shutdown discipline).

**Rollback risk.**
**Moderate-to-high.** This is the largest single deletion in the arc. If `Ep6RecvThread::run_loop` has a defect that the inline-tap pattern of Stage 3-5 didn't exercise (e.g. WSAEventSelect timeout handling, the `prn->wdt` watchdog path at `networkproto1.c:443`), RX could stall. Escape: revert the commit; rxWorker_ comes back, `tap_.start(...)` does NOT run, and the inline-tap path resumes. **Stages 4-5 stay live** — the new sink-registration layer keeps working because the inline tap is still active in the restored rxWorkerLoop. (This is the load-bearing property of the staged design: stages 4-5 are independent of WHICH thread feeds `Ep6RecvThread::process_datagram`.)

---

## Stage 7 — Migrate TX-Side Setters Onto `FrameComposer` Free Functions

**Title.** Today's `HL2Stream::setTxFreqHz` / `setTxDriveLevel` / `setPaEnabled` / `setMicBoost` / `setMox` / `setTxStepAttnDb` / `setRx1FreqHz` write into the existing `_cc_registers`-like state that the existing `composeCC()` round-robin reads. After this stage they ALSO call the new wire-layer free-function setters `lyra::wire::set_tx_freq()` / `set_drive_level()` / `set_pa_on()` / `set_rx_freq()` / etc. (`FrameComposer.h` — verify the exact symbol names against the existing `wire/FrameComposer.h`).

Setter migration is BEFORE the txWorkerLoop migration so the new `write_main_loop_hl2()` has live state to read when Stage 8 wires it in.

**Reference provenance.**
- `WriteMainLoop_HL2` reads `prn->tx[0].frequency`, `prn->rx[0].frequency`, `prn->tx[0].drive_level`, `prn->tx[0].pa`, `prn->mic.mic_boost`, etc. directly out of the global `prn` (`networkproto1.c:976-1180`).
- The setters in the reference (e.g. `SetTxFreq`, `SetDriveLevel`) all mutate the corresponding `prn` fields directly — Lyra's `FrameComposer::set_tx_freq()` mirrors this by writing into `prn->tx[0].frequency` (see `FrameComposer.cpp` — verify).
- Operator-code call paths: `setTxFreqHz` → `Radio::set_tx_freq` (UI), `setRx1FreqHz` → tuner panel, etc.

**Lyra files touched.**
- `src/hl2_stream.cpp::setTxFreqHz` — after the existing `txFreqHz_.exchange(...)` and log emit, ALSO call `lyra::wire::set_tx_freq(static_cast<std::int32_t>(hz));`.
- `src/hl2_stream.cpp::setTxDriveLevel` — after the existing `txDriveLevel_.exchange(...)`, ALSO call `lyra::wire::set_drive_level(clamped);`.
- Likewise for `setPaEnabled` → `set_pa_on(on)`, `setMicBoost` → (verify FrameComposer setter), `setRx1FreqHz` → `set_rx_freq(hz)`, `setTxStepAttnDb` → `set_tx_step_attn_db(db)`, and the corresponding `setRxStepAttnDb` / `setMox` paths.
- Operator-facing call sites in `radio.cpp`/`tci_server.cpp` already go through `HL2Stream::setX` methods — no change there.

**What stays inert / what becomes live.**
- LIVE: every operator-facing TX/RX state setter dual-writes — once to the existing `HL2Stream` atomics that `composeCC` reads, AND once to the `prn->*` fields that the new `write_main_loop_hl2()` will read. The two register banks now mirror each other on every operator state change.
- INERT: the new `write_main_loop_hl2()` is still not called by anyone (still wire-inert). The old `composeCC` is still the live emitter.

This stage is the BUFFER stage that makes Stage 8's swap atomic: by the time Stage 8 swaps the emit path, `prn->*` is already fully populated with current operator state.

**Parity-check entry.**

> Step 14 Stage 7 — Operator-facing TX/RX setters dual-write to both `HL2Stream::*` atomics AND `prn->*` (via `FrameComposer::set_*` free functions). New `prn->*` state mirrors live operator state. Wire bytes unchanged (new `write_main_loop_hl2()` still not invoked). Reference provenance: `networkproto1.c:976-1180` (read sites). Signed-off: pending bench.

**Bench gate.**
Build-clean + verify (via in-app log + debug atomic-reads) that on every operator action (freq dial, mode change, drive slider, PA toggle, MOX click without keying), the `prn->*` field reads back the expected value. **No RF behavior changes vs Stage 6.**

**Rollback risk.**
**Tiny.** Pure additive. Worst case: a FrameComposer setter has a defect and writes garbage to a `prn->*` field — but nothing reads `prn->*` yet for live wire emit. Escape: comment out the new setter calls, rebuild.

---

## Stage 8 — Migrate The EP2 Egress Onto `Ep2SendThread` + `write_main_loop_hl2()`. SECOND Bench Gate.

**Title.** Replace `HL2Stream::txWorkerLoop`'s `sendDatagram(...)` per-fire C&C-composition + sendto with `Ep2SendThread::start(socket_, &dest, sizeof(dest))`. The new thread runs `process_one_pair()` per iteration, calling `outbound_wait_pair_ready()` → `quantize_and_pack()` → `write_main_loop_hl2()` → `metis_write_frame(0x02, ...)`. The producer of `outbound_push_lr` / `outbound_push_iq` is the AAmixer / WDSP TXA / TUN generator / tci pipeline.

This is **the SECOND stage that flips the live wire egress.** Bench-critical. Gets its own commit.

**Reference provenance.**
- `sendProtocol1Samples` thread body: `networkproto1.c:1204-1267`.
- `WaitForMultipleObjects(2, prn->hsendEventHandles, TRUE, INFINITE)` — wait for BOTH LR + IQ ready (`:1220`).
- `MetisWriteFrame(0x02, FPGAWriteBufp)` emit + per-iteration semaphore release (`:1198-1200`).
- `WriteMainLoop_HL2` body composing the round-robin C&C bytes in the same buffer: `:869-1201`.

**Lyra files touched.**
- `src/hl2_stream.cpp::open()` — replace the existing `txWorker_ = std::jthread([...]{txWorkerLoop(...);})` at line 669-671 with `ep2Thread_.start(socket_, &dest, sizeof(dest));`.
- `src/hl2_stream.cpp::close()` — replace `txWorker_.request_stop(); txWorker_.join();` lines with `ep2Thread_.stop();`.
- `src/hl2_stream.h` — delete `txWorker_` member, delete `txWorkerLoop` declaration, add `Ep2SendThread ep2Thread_;`. Delete the entire EP2 template / sendDatagram lambda infrastructure that's now dead code.
- `src/hl2_stream.cpp` — DELETE `txWorkerLoop` body (lines 2437-2729, ~290 lines). DELETE the helper functions it called (`buildEp2KeepaliveTemplate`, `composeCC`, possibly `buildControlPacket` — `buildControlPacket(true)` was moved to `open()` in Stage 2; `buildControlPacket(false)` for STOP packet stays in `close()`).
- `src/hl2_stream.cpp::close()` — change the STOP packet send path: today uses raw `sendto` on `socket_`; tomorrow can either keep raw `sendto` (the STOP packet is a one-shot, not part of the wire-layer's metis_write_frame path) or use a new wire-layer `metis_send_stop()` helper. Operator preference. Reference uses `sendPacket(listenSock, ...)` in `SendStopToMetis()` (`networkproto1.c:89`), the same as the priming pass. Recommendation: keep the raw `sendto` for now — STOP is a one-shot and not bench-critical.

Source of LR + IQ samples: today the AAmixer feeds `HL2Stream::queueTxAudio(...)` which fills `audioBuf_` (lines 2691-2720 in old txWorkerLoop). The TX-DSP-worker / TUN generator feeds `txIqSource_` (lines 2602-2620). After Stage 8 those upstream producers need to call `lyra::wire::outbound_push_lr(...)` / `outbound_push_iq(...)` instead.

- `src/mic_source.cpp` (or wherever the TX-DSP-worker lives) — change the producer-side calls to push into `outbound_*` instead of `HL2Stream::queueTxAudio`.
- AAmixer / `Radio::pushAudioFrame` (or whatever fills `audioBuf_`) — change to `outbound_push_lr`.

**What stays inert / what becomes live.**
- LIVE: full TX wire egress now flows through the new layer. `g_metis_out_seq_num` is the single outbound seq counter (already seeded by Stage 2's priming pass). `write_main_loop_hl2()` composes the 19-slot C&C round-robin from `prn->*` state (mirrored by Stage 7). `Ep2SendThread::process_one_pair()` is producer-paced via `outbound_wait_pair_ready()` exactly like the reference's `WaitForMultipleObjects` at `:1220`.
- The MOX-edge IQ zeroing at `:1227` is now in `Ep2SendThread::process_one_pair()` via `XmitBit` (verify the XmitBit source — likely a global from `prn` or a reference-mirrored TU-scope; check `Ep2SendThread.cpp:193-195`).
- INERT: nothing — this stage retires the last piece of old wire-egress code.

**Bench gate (THE second one — TX-aware).**
This is bench-critical but operator does NOT key MOX on this stage. The gate is RX-side soak + steady-state EP2 cadence:

- Open stream cold, confirm RX comes up cleanly (priming pass works, EP6 recv works, AK4951 audio works).
- 30-minute soak with normal RX listening: panadapter healthy, audio clean, no underrun, no gateware EP2-timeout (gateware watchdog is ~13 s — any EP2 cadence drop wedges the stream).
- Stop/start cycles ×5: stream comes up clean each time.
- **Operator does NOT key MOX during this bench.** TX path is wired but not exercised — that's a later phase per the standing rule "no MOX during step 14".
- Optional: with PA disabled (default), operator presses MOX briefly to confirm MOX-state plumbing into `XmitBit` works and the wire bit flips — but does NOT enable PA. PA-current should stay at idle ~0.2 A; no RF leaves the box.

**Rollback risk.**
**HIGH.** Replacing the live EP2 egress is the highest-risk single change in the arc. If `write_main_loop_hl2()` composes any C&C byte incorrectly the gateware may misbehave (wrong RX freq, wrong filter selection, EP2 timeout, MOX stuck). Escape: revert the Stage-8 commit; old `txWorker_` + `txWorkerLoop` come back; old `composeCC` is again the live emitter. Stages 1-7 stay live. Old txWorkerLoop is fully equivalent to HEAD txWorkerLoop because none of Stages 1-7 mutated it.

The fallback Stage-7 dual-write ensures `prn->*` state is correct — if the bug is in `write_main_loop_hl2`'s C&C composition rather than the upstream state, then reverting Stage 8 alone restores the working composeCC path.

---

## Stage 9 — Delete Stage-7's Dual-Write, `HL2Stream::queueTxAudio`, `txIqSource_`, And Unused State

**Title.** Cleanup. After Stage 8 the old `HL2Stream` atomics (`txFreqHz_`, `txDriveLevel_`, `paOn_`, `txStepAttnDb_`, `micBoost_`, `rx1FreqHz_`, `txSeq_`, `audioBuf_`, `audioCv_`, etc.) are no longer read by any wire-egress code. Stage 9 deletes them and lets `prn->*` be the single source of truth.

Some of these atomics double as QML data sources (e.g. QML reads `rx1FreqHz_` for the VFO LED). Those need to migrate to reading `prn->rx[0].frequency` directly OR `HL2Stream` keeps a thin shadow that mirrors `prn->*` for QML's binding consumers.

**Reference provenance.** No new reference. This is Lyra-native cleanup of the migration scaffolding.

**Lyra files touched.**
- `src/hl2_stream.h` — delete: `txFreqHz_`, `txDriveLevel_`, `paOn_`, `txStepAttnDb_`, `micBoost_`, `rx1FreqHz_` (or repurpose as QML-only shadows), `txSeq_`, `audioBuf_`, `audioBuf_*` member group, `audioCv_`, `audioMtx_`, `audioRd_`, `audioCount_`, `audioCap_`, `audioCount_*`, `audioWr_`, `injectAudio_`, `injectTxIq_`, `txIqSource_`, `txIqSourceMtx_`, `txTotalDg_`, `txWindowDg_`, `txSendErrors_`, `lastPttIn_`, `ccIdx_`, the EP2 template byte arrays.
- `src/hl2_stream.cpp` — delete: `queueTxAudio`, `clearTxAudio`, `composeCC`, `buildEp2KeepaliveTemplate`. Delete the Stage-7 dual-write half from every setter (old atomic write goes away; `prn->*` write stays).
- `src/main.cpp` — operator state setters now go straight to `lyra::wire::set_*` free functions, optionally still via `HL2Stream` wrappers for the QML signal emit + log emit semantics.
- QML data sources — bind to either thin shadows or directly to `prn->*` (a small adapter QObject for QML's needs).

**What stays inert / what becomes live.**
- The old atomics are gone. Single source of truth: `prn->*`.
- Wire bytes unchanged from Stage 8.

**Parity-check entry.**

> Step 14 Stage 9 — Migration scaffolding deleted. `prn->*` is single source of truth for TX/RX/PA/mic/MOX state. Old `HL2Stream` atomics retired. QML reads via thin shadows or direct `prn->*` adapter. Wire bytes unchanged. Reference provenance: `prn->*` is the reference's single source of truth (`network.h` _radionet struct definition + all read/write sites in `networkproto1.c`). Signed-off: pending bench.

**Bench gate.**
Functional regression test: every operator surface (freq dial, mode picker, drive slider, PA checkbox, mic boost, MOX/TUN buttons, foot-switch, telemetry banner, panadapter, audio output) works as before Stage 9. 1-hour soak.

**Rollback risk.**
**Moderate.** A lot of code deletion in one stage. If any QML binding is missed and silently reads a stale shadow, an operator surface could go dark. Escape: revert; the dual-write from Stage 7 returns; QML binds are unaffected.

---

## Stage 10 — Final Cleanup + Doc Sweep

**Title.** Final pass: delete every `// Step 14 Stage N` migration comment that no longer reflects shipped state. Update `docs/architecture/PARITY_CHECKPOINTS.md` to mark all 9 prior stages signed-off. Add the §1-C Stage 5 + Stage 6 + Stage 14 sign-off entries to the architectural ledger. Run the Rule-2 forbidden-token grep across `src/wire/`, `src/hl2_stream.cpp`, `src/main.cpp` to confirm no `Thetis|PowerSDR|Console.cs|OpenHPSDR` snuck in.

No code changes beyond comments + docs. No bench gate (build-clean only).

---

## Summary Table

| Stage | Wire state | Risk | Bench Gate? | Reverts cleanly to |
|:-----:|:-----------|:----:|:------------|:-------------------|
| 1 | INERT (singleton+bind+init) | tiny | no — build-clean | HEAD |
| 2 | LIVE — 6 priming datagrams | moderate | **YES — first HL2 talk** | Stage 1 |
| 3 | INERT — parser tap, no sinks | tiny | yes — comparator | Stage 2 |
| 4 | LIVE — WDSP RX1 via Router | moderate | yes — RX audio A/B | Stage 3 |
| 5 | LIVE — mic+telem+PTT migrated | moderate | yes — operator surfaces | Stage 4 |
| 6 | LIVE — own EP6 thread | mod-high | yes — soak+restart | Stage 5 (inline-tap) |
| 7 | INERT — dual-write `prn->*` | tiny | no — debug read-back | Stage 6 |
| 8 | LIVE — Ep2SendThread+wmlh | **HIGH** | **YES — second HL2 talk** | Stage 7 |
| 9 | Cleanup — atomics retired | moderate | yes — functional regression | Stage 8 |
| 10 | Doc sweep | tiny | no | Stage 9 |

Operator signs off stage-by-stage; each stage gets its own commit; co-author trailer mandatory on every commit. **Stages 2 and 8 are the two bench-critical commits** — neither lands on `main` without an explicit operator green-light after the bench observation.

---

## Answers To The Specific Questions

**Q1 — Reference session-open sequence.** Verified above. The ordering invariants the plan respects: `prn` non-null BEFORE `StartAudioNative` body runs; START packet emitted BEFORE thread spawn; `prn->hobbuffsRun` / `hsendEventHandles` semaphore allocation BEFORE the EP2 thread enters its `WaitForMultipleObjects`; `ForceCandCFrame(3)` priming AFTER thread spawn but BEFORE the EP6 read-loop main `while`. Lyra mirrors verbatim: `prn = radio_net()` + `metis_wire_bind` + `outbound_init` in `open()` body (Stage 1); START hoisted to `open()` body BEFORE the EP6 thread spawn (Stage 2 — matches `netInterface.c:50`); `ForceCandCFrame(3)` lives INSIDE `Ep6RecvThread::run_loop()` body, AFTER thread spawn, BEFORE `WSAEventSelect` setup, BEFORE the wait-loop entry — exact reference order at `networkproto1.c:427-434`. `open()` blocks on the EP6 thread's init-sem until WSAEventSelect setup completes, matching `prn->hReadThreadInitSem` semantics at `netInterface.c:61-63`. **No order deviation, no operational-equivalent hand-wave** — the priming pass lives in the EP6 thread because the reference puts it in the EP6 thread.

**Q2 — Lyra deviations from reference order.** Today: no `prn` assignment, no wire-bind, no `outbound_init`, no priming pass, START sent from inside txWorkerLoop instead of `open()`. The plan FIXES the START location (hoist from txWorkerLoop to `open()`) in Stage 2 — that's "no deviation, no patching" per the standing rule.

**Q3 — Current sink registration mechanism.** Today: **the WDSP RX1 IQ consumer** attaches via `stream->setIqSink(lambda)` in `main.cpp:252`; the lambda calls into the WDSP engine's `processIq`. The rxWorkerLoop fires it synchronously per-datagram (38 samples). **Mic samples** flow via `stream.setMicConsumer(...)` set by `Hl2Ep6MicSource::Hl2Ep6MicSource(stream)` in `mic_source.cpp:15`. **Telemetry** is not really a "sink" — it's atomic writes inside rxWorkerLoop's per-frame status decode (lines 1089-1131) that QML polls via the stats signal. **HW PTT-in** is an edge detect inside rxWorkerLoop (1157-1179) that fires `QMetaObject::invokeMethod(requestMoxFromHwPtt, QueuedConnection)`.

Migration:
- WDSP RX1 IQ: `register_sink(router_instance(0), port=0, call_idx=0, ctrl_word=0, wdsp_callback)` (Stage 4). Reference `xrouter` dispatch source 0 = DDC0 per `networkproto1.c:550`.
- Mic: `Ep6RecvThread::set_mic_sink(...)` (Stage 5). Reference: `Inbound(inid(1,0), ...)` at `:579`.
- Telemetry: stay on atomics — write them from inside `Ep6RecvThread::decode_status_header` (Stage 5). Optional push-style telemetry_sink is wired but unused.
- HW PTT-in: edge-detect moves to `Ep6RecvThread::decode_status_header`, FSM dispatch via `HL2Stream::requestMoxFromHwPtt` (Stage 5).

For future RX2 (Phase 5, Task #96), the same pattern adds `register_sink(router_instance(0), port=2, call_idx=0, ctrl_word=0, rx2_wdsp_callback)` — source 2 = DDC1 = RX2 per `networkproto1.c:552` (HL2 nddc=4 branch).

**Q4 — `prn` assignment lifetime.** Reference: `prn` is a single global pointer set once at startup to point to a `_radionet` struct allocated somewhere upstream of `StartAudioNative` (see `network.h:414` — verify in the reference tree). Lyra: `prn` is the singleton at `src/wire/RadioNet.cpp:25` (`RadioNet* prn = nullptr;`), pointed at the static singleton returned by `lyra::wire::radio_net()`. Lyra's lifetime is "global, valid for the whole process lifetime once `radio_net()` is first called." Stage 1's `prn = lyra::wire::radio_net()` is the first call; thereafter `prn` stays non-null until process exit. `close()` does NOT null `prn` — it nulls the wire-bind state via `metis_wire_bind(-1, nullptr, 0)` so subsequent `metis_socket_fd()` returns -1, but the singleton object remains. This matches reference behavior (reference's `prn` is non-null between Lyra session-close and next session-open too — operator can re-open without re-allocating).

**Q5 — Existing C&C cycle / round-robin.** Yes, it exists: `HL2Stream::composeCC(ccIdx_, moxBit, cc)` (called twice per datagram in txWorkerLoop at `hl2_stream.cpp:2509, 2518`), with `ccIdx_ = (ccIdx_ + 1) % 19`. This is Lyra's port of the reference's `out_control_idx` round-robin at `networkproto1.c:617-857` (the case-switch on `out_control_idx` is `composeCC`'s body — verify; we did NOT read composeCC source in this plan exploration, operator should confirm during Stage 7/8 implementation). Stage 8 hands off cleanly: `Ep2SendThread::process_one_pair()` calls `write_main_loop_hl2(g_fpga_write_bufp.data())` which is the reference-faithful port of `WriteMainLoop_HL2`. The new `write_main_loop_hl2()` owns its OWN `out_control_idx` TU-scope static (per `FrameComposer.cpp`'s TU-scope statics design). The old `ccIdx_` becomes dead state in Stage 9 cleanup.

**Q6 — Thread lifetime + clean shutdown.** `Ep6RecvThread::stop()` (`Ep6RecvThread.h:112`) sets `stop_request_`, frees the WSA event, joins. `Ep2SendThread::stop()` (`Ep2SendThread.h:66`) sets `stop_request_`, calls `outbound_unblock()` to wake the parked `outbound_wait_pair_ready()`, joins. Both are bounded by the standard Windows shutdown discipline (no infinite wait — the WSAWaitForMultipleEvents in EP6 has the `prn->wdt ? 3000 : WSA_INFINITE` timeout per `networkproto1.c:443` — Lyra must honor this same WDT trick to avoid hangs). HL2Stream::close() stop order in Stage 8: (a) `force_release_all` (existing — clears MOX + PA-safety); (b) `ep2Thread_.stop()` (drains TX); (c) send STOP packet (so gateware acks); (d) `ep6Thread_.stop()` (drains RX); (e) close socket. Matches reference `IOThreadStop()` cleanup at `network.c:1443+` and the MetisStopReadThread / MetisStopWriteThread pattern.

**Q7 — Operator code TX/RX writes.** Today: `Radio::setTxFreq(hz)` → `stream->setTxFreqHz(hz)` → `txFreqHz_.store(hz)` (atomic write, read by `composeCC`). The migration in Stage 7 dual-writes both `txFreqHz_.store(hz)` AND `lyra::wire::set_tx_freq(hz)` (which writes `prn->tx[0].frequency`). In Stage 9 the `txFreqHz_` atomic goes away and only `set_tx_freq` survives. Same pattern for drive level, PA enable, mic boost, step-attn, MOX, RX freq.

For the QML observable, freq display etc.: today the QML binds to `HL2Stream::rx1FreqHz`/etc. via Q_PROPERTY. In Stage 9 the property reads `prn->rx[0].frequency` instead. The Q_PROPERTY getter is a thin adapter.

**Q8 — Rule 24 invariants on `_radionet` fields.** The §1-C Stage 4A audit closed every field; step 14 is the first time fields are read on live wire traffic. Re-verify per field touched:
- `prn->RxBuff` read in `Ep6RecvThread::process_datagram`'s nddc=4 dispatch (Stage 3) → `xrouter(... prn->RxBuff[0])` matches `networkproto1.c:550`. ✓
- `prn->TxReadBufp` written in mic-decimation loop → matches `:570-573`. ✓
- `prn->outLRbufp` / `outIQbufp` read in `Ep2SendThread::process_one_pair` → matches `:1215-1216`. ✓
- `prn->OutBufp` written by `quantize_and_pack` → matches `:1257-1258`. ✓
- `prn->hDataEvent` + WSA event field → matches reference's `network.h:105-106` per §1-C Stage 4B. ✓
- `prn->tx[0].frequency` / `rx[0].frequency` / `tx[0].drive_level` / `tx[0].pa` etc. — read by `write_main_loop_hl2()` in Stage 8 → matches `:976-1180`. ✓
- TU-scope `g_metis_out_seq_num` (NOT in `_radionet`) — matches reference's file-scope `MetisOutBoundSeqNum` at `:30`. ✓
- TU-scope `MetisLastRecvSeq` / `SeqError` in `Ep6RecvThread.cpp` (NOT in `_radionet`) — matches reference's file-scope at `:28, 26`. ✓
- TU-scope `g_fpga_read_bufp` / `g_fpga_write_bufp` (NOT in `_radionet`) — matches reference's file-scope `FPGAReadBufp` / `FPGAWriteBufp`. ✓

No field crosses a `_radionet` vs file-scope-global boundary in the wrong direction. Step-14 implementation must NOT introduce any NEW `prn->` write that doesn't have a reference cite (e.g. adding a Lyra-native flag to `_radionet` is a Rule-3 violation).

---

### Critical Files for Implementation

- `Y:\Claude local\SDRProject\lyra-cpp\src\hl2_stream.cpp` — `open()` (line 554), `close()` (674), `rxWorkerLoop` (921), `txWorkerLoop` (2437), all TX/RX setters (1302-1424).
- `Y:\Claude local\SDRProject\lyra-cpp\src\hl2_stream.h` — class declaration, thread/atomic members, sink typedefs (599-620, 993-994, 1274-1288).
- `Y:\Claude local\SDRProject\lyra-cpp\src\wire\Ep6RecvThread.cpp` — `start()`, `run_loop()`, `process_datagram()` (the new EP6 thread).
- `Y:\Claude local\SDRProject\lyra-cpp\src\wire\Ep2SendThread.cpp` — `start()`, `process_one_pair()` (the new EP2 thread; lines 77-260).
- `Y:\Claude local\SDRProject\lyra-cpp\src\main.cpp` — `setIqSink` registration (252), shutdown teardown (336-337).
- Reference cross-files for verification at every commit: `D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\ChannelMaster\networkproto1.c` and `netInterface.c`.