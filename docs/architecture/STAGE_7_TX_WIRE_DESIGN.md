# Stage 7 — TX Wire-Consumer Design Paper

**Status:** PLAN-BEFORE-CODE — awaiting operator sign-off + (optional) 2-agent red-team check.
**Operator directive (2026-06-09):** "A and Act as an expert principal C++ developer and Qt6 architect / Port the TX side of Thetis to Lyra CPP in modern C++23 and Qt6. Verify as you go."
**Methodology:** Mirrors `STEP14_STAGE2B_DESIGN.md` discipline — this document is the complete plan-paper, file:line-grounded, scope-pinned. No code lands until reviewed.

---

## 1. Scope

Stage 7 is the **TX-side wire-consumer rebuild** — the consumer half that fills the TX-rip (Task #112) vacuum and activates the Stage A/B/C/D ports just shipped. After Stage 7 lands, an operator keying MOX into a dummy load produces real RF on the new TX path.

**This stage is independent of Stage 2a/2b (RX-side wire-LIVE).** Surface isolation pinned in §5. Both can proceed in parallel without merge conflict.

**Pre-Stage-7 invariants (today's branch `tx-rebuild` @ `744ba88`):**
- `TxChannel` (`src/wdsp/TxChannel.{h,cpp}`) shipped, reference-faithful, NEVER instantiated in production (Stage E.0 audit verified).
- `ILV` (`src/wdsp/ILV.{h,cpp}`) shipped Stage C, `pilv[]` central bank, bit-exact unit-tested.
- `xcmaster(int stream)` + `xcmasterTickTx(xmtr_id, mic_in, n)` + `pxmtr[]` (`src/wire/CMaster.{h,cpp}`) shipped Stage D, bank+dispatch unit-tested.
- `CMaster.cpp:88` `SendpOutboundTx` wires through to `SetILVOutputPointer(0, cb)`.
- `RadioNet.cpp:304` Stage-A no-op `SendpOutboundTx` stub REMOVED (Stage C.3 B.6.b discipline).
- `Hl2Ep6MicSource` (`src/mic_source.{h,cpp}`) shipped, set-once-before-start contract.
- `HL2Stream::TxControl` struct (`hl2_stream.h:430-480`) shipped: 9 std::function callback slots. Currently all empty.
- `HL2Stream::registerTxControl()` (`hl2_stream.h:874`, body `hl2_stream.cpp:1995`) shipped. Currently never called by production code.
- `HL2Stream::_tx_iq` deque + `setInjectTxIq(bool)` / `registerTxIqSource` — the EP2-packer TX-side consumer plumbing — shipped.
- `TxDspWorker` / `TciMicSource` deliberately RIPPED (Task #112 Phase 1 Q2). Their replacement is Stage 7.

---

## 2. Surface Map — 5 surfaces, file:line cites

The reference's TX wire architecture (per `cmaster.c:381-407` case 1 + the surrounding lifecycle) reduces for HL2 SSB v0.2 to 5 Lyra-cpp surfaces:

### Surface 1 — `Radio::start()` TX lifecycle

**Today:** `Radio.start()` (via `main.cpp:250-265`) constructs `WdspNative` + `WdspEngine` + `HL2Stream`. NO `TxChannel`. NO `ILV`. NO `create_xmtr_hl2`. The TxChannel `.h/.cpp` are dead code; the `pxmtr[]` bank stays empty.

**Stage 7 adds, at Radio TX-construction time (after WdspNative loads + before HL2Stream::open):**

1. `auto txChannel = std::make_unique<lyra::wdsp::TxChannel>(wdsp, /*channel_id=*/1, TxConfig{});`
2. `txChannel->open();` (allocates buffers + WDSP `OpenChannel(1, …)`)
3. `auto* ilv = lyra::wdsp::create_ilv(/*xmtr_id=*/0, /*run=*/0, /*outbound_id=*/1, /*insize=*/txChannel->outSize(), /*ninputs=*/2, /*what=*/3, /*Outbound=*/{});`
   - Matches `cmaster.c:226-232` byte-exact (`run=0` = bypass mode for HL2; `ninputs=2`; `what=3`). Outbound left empty for now — registered via `SendpOutboundTx` next.
4. `lyra::wire::create_xmtr_hl2(/*xmtr_id=*/0, txChannel.get(), /*ilv_xmtr_id=*/0);` — publishes into `pxmtr[0]` so `xcmasterTickTx(0, …)` resolves.
5. `lyra::wire::SendpOutboundTx(outbound_lambda);` — see Surface 2.
6. `txChannel->start();` (`SetChannelState(1, 1, 0)` — channel running).

**Teardown in `Radio::stop()` — symmetric reverse order:**

1. `txChannel->stop();` (`SetChannelState(1, 0, 1)` — blocking flush, per `cmaster.c:265` parity).
2. `lyra::wire::SendpOutboundTx({});` — clear callback BEFORE destroying ILV (so any in-flight xilv dispatch hits empty Outbound = no-op, not deref-null).
3. `lyra::wire::destroy_xmtr_hl2(/*xmtr_id=*/0, txChannel.get());` — clears `pxmtr[0]` defensively.
4. `lyra::wdsp::destroy_ilv(ilv, /*xmtr_id=*/0);` — clears `pilv[0]`.
5. `txChannel->close();` (`CloseChannel(1)`).
6. `txChannel.reset();`

**Ownership:** `txChannel` lives on `Radio` as `std::unique_ptr<lyra::wdsp::TxChannel> txChannel_;`. ILV pointer non-owning view into `pilv[0]` (Stage C bank manages lifetime via `create_ilv`/`destroy_ilv`).

### Surface 2 — Real `SendpOutboundTx` handler (the wire-effective hook)

**Today:** `pcm->OutboundTx` is empty `std::function`. Stage C.3's `SendpOutboundTx → SetILVOutputPointer(0, cb)` chain works but sits inert because nothing registers a real `cb`.

**Stage 7 adds:** a single lambda registered in Surface 1 step 5:

```cpp
lyra::wire::SendpOutboundTx(
    [hl2_stream_weak](int /*obid*/, int n_samples, double* iq) {
        // n_samples = complex tuple count; iq = interleaved {I, Q} doubles.
        // For HL2 SSB ILV bypass mode: n_samples == txChannel->outSize();
        // iq == txChannel->outBuffers()[0] (memcpy'd by xilv from out[0]).
        auto s = hl2_stream_weak.lock();
        if (!s) return;
        s->pushTxIqDoubles(iq, n_samples);  // NEW HL2Stream API — see §3
    });
```

**Threading:** Outbound fires on the **caller's thread** — which is the consumer thread that called `xcmasterTickTx` (Surface 3 — the mic source's rx-loop thread). So `pushTxIqDoubles` MUST be cheap + lock-bounded (matches the existing `_tx_audio` deque pattern in `hl2_stream.cpp`).

**B.6.b-class landmine check (CRITICAL — this is the Stage B.6.b root-cause pattern):**
- `SendpOutboundTx({})` MUST be called in `Radio::stop()` BEFORE `destroy_ilv`/`destroy_xmtr_hl2`. Otherwise a late xilv dispatch (rx-loop in flight at stop time) hits a stale `pilv[0]->Outbound` lambda holding a dangling `hl2_stream_weak` ref. The lambda is `weak_ptr`-safe (returns early on dead stream), but discipline says: clear the registration first.
- Verified: `RadioNet.cpp:304` Stage-A stub REMOVED in Stage C.3. NO other production path registers `SendpOutboundTx`. Confirmed by `grep -nE "SendpOutboundTx\s*\(" src/` returning only `CMaster.cpp:73/82` (decl/def site) + `RadioNet.cpp:246` (a comment, not a call).

### Surface 3 — TX consumer pump (the mic → xcmasterTickTx hookup)

**Today:** `Hl2Ep6MicSource::Consumer` slot exists (`mic_source.h:53`). Nothing registers a consumer. Mic samples arrive at `Hl2Ep6MicSource` but the consumer callback is empty → samples dropped.

**Stage 7 adds:** Register a consumer lambda in `Radio::start()` (after Surface 1's `txChannel->start()`):

```cpp
hl2EpMicSource_ = std::make_unique<lyra::dsp::Hl2Ep6MicSource>(*hl2Stream_);
hl2EpMicSource_->setConsumer(
    [](int n_samples, const double* iq_pairs) {
        // Mic arrives as {I=mic, Q=0} pairs at 48 kHz per Hl2Ep6MicSource
        // contract (matches reference Inbound(inid(1,0), n, prn->TxReadBufp)
        // byte-shape — see Hl2Ep6MicSource header).
        lyra::wire::xcmasterTickTx(/*xmtr_id=*/0,
                                   const_cast<double*>(iq_pairs),
                                   n_samples);
    });
```

**Threading:** Consumer fires on the **Ep6 recv-loop thread** (synchronous per `mic_source.h:30-35`). xcmasterTickTx → TxChannel::process → fexchange0 → xilv → SendpOutboundTx → pushTxIqDoubles all run on this single thread. No queueing, no fan-out, no cross-thread races on the hot path.

**const_cast rationale:** `xcmasterTickTx` takes `double*` (matching ILV / WDSP API surface); `Hl2Ep6MicSource::Consumer` declares `const double*` for callee discipline; the cast is a known idiom-translation seam (same as `TxChannel::process` cast at `TxChannel.cpp:172`). Mic samples are NOT modified by the chain (verified: TxChannel::process only reads through fexchange0's input arg).

**B.6.b-class landmine check:**
- `Hl2Ep6MicSource::setConsumer` must be called **before** `hl2Stream_->open()` per the `Hl2Ep6MicSource::set_mic_sink` set-once-before-start contract (`mic_source.h:22-29`). Stage 7's `Radio::start()` ordering: create Hl2Ep6MicSource → setConsumer → THEN open HL2Stream. Documented contract honored.
- Symmetric teardown: `hl2EpMicSource_.reset()` happens AFTER `hl2Stream_->stop()` joins the rx-loop, so no consumer callback fires post-teardown.

### Surface 4 — `HL2Stream::TxControl` callback population

**Today:** `HL2Stream::registerTxControl(TxControl)` (`hl2_stream.h:874`) exists; never called by production code. Operator UI setters (`setMicGainDb`, `setMode`, etc.) forward to empty `txControl_.setX` callbacks → no-op.

**Stage 7 adds:** Build the `TxControl` struct in `Radio::start()` (after TxChannel construction, before HL2Stream::open) and register it:

```cpp
HL2Stream::TxControl ctl{};
ctl.start            = [tx = txChannel_.get()]() { tx->start(); };
ctl.stop             = [tx = txChannel_.get()]() { tx->stop();  };
ctl.setInjectTxIq    = [/*nothing yet — see below*/](bool on) { /* ... */ };
ctl.setMode          = [tx = txChannel_.get()](int m) {
    tx->setMode(static_cast<lyra::wdsp::TxaMode>(m));
};
ctl.setBandpass      = [tx = txChannel_.get()](double lo, double hi) {
    tx->setBandpass(lo, hi);
};
ctl.setMicGainDb     = [...](double db) {  /* SetTXAPanelGain1 via wdsp */ };
ctl.setAlcMaxGainLinear = [...](double lin) { /* SetTXAALCMaxGain */ };
ctl.setAlcDecayMs    = [...](int ms)     { /* SetTXAALCDecay */ };
ctl.setLevelerOn     = [...](bool on, double topLin) { /* SetTXALevelerSt + SetTXALevelerTop */ };
ctl.setLevelerDecayMs = [...](int ms)    { /* SetTXALevelerDecay */ };
hl2Stream_->registerTxControl(std::move(ctl));
```

**The WDSP setters (`SetTXAPanelGain1` etc.) are already cffi-resolved** in `wdsp_native.cpp:246-261`. Lambdas reach them via the `WdspNative& wdsp` captured by reference.

**`setInjectTxIq` is the keying gate.** The FSM (`src/tx/PttFsm.{h,cpp}` per Stage 7's separate work, OR an interim direct setter call) drives `HL2Stream::setInjectTxIq(true)` on keydown → forwards to `ctl.setInjectTxIq(true)`. For Stage 7 minimum: leave the lambda as a no-op stub (HL2Stream's own internal `inject_tx_iq_` flag is what the EP2 packer reads, per `hl2_stream.cpp:1960`); the FSM wire-up to `setInjectTxIq` is a separate commit if not already covered.

**TxControl-vs-DSP race:** The callbacks fire on the **Qt main thread** (operator UI changes). TxChannel's WDSP setter calls are short (microseconds inside a DLL call). Per `TxChannel.h` docstring + the operator's setter cadence (rare slider moves), no special locking required at the seam — TxChannel's own `channelMtx_` (if it has one — verify) handles WDSP-setter-vs-process serialization.

### Surface 5 — Hl2Ep6MicSource lifecycle (already shipped, needs Radio integration)

`Hl2Ep6MicSource` is fully implemented (`src/mic_source.{h,cpp}`). Stage 7 just needs Radio to construct + register it (covered in Surface 3).

**No new code in `mic_source.{h,cpp}`.** Stage 7 is purely consumer-side wiring.

---

## 3. New API surface in `HL2Stream`

One new method needed:

```cpp
// In hl2_stream.h, alongside the existing _tx_audio / _tx_iq plumbing.
// Pushes interleaved {I, Q} doubles (from the TX-DSP chain Outbound) into
// the _tx_iq deque that the EP2 packer drains.  Symmetric with the existing
// _tx_audio path; uses the same drop-oldest discipline + S2 timer-paced
// drain.  Called on the Ep6 rx-loop thread from SendpOutboundTx's lambda.
void pushTxIqDoubles(const double* iq, int n_samples);
```

**Implementation:** Mirror the existing `_tx_audio` push path (search `hl2_stream.cpp` for `_tx_audio_lock` to find the pattern). Take `_tx_iq_lock`, append to deque, drop-oldest if at cap, release. No allocation on hot path (deque is pre-sized).

**Open question for the operator:** does the existing TX I/Q packer already expect samples via `registerTxIqSource(TxIqSource)` (`hl2_stream.h:880`) as a *pull* callback instead of a *push* deque? If yes, Surface 2's lambda becomes a TxIqSource registration, not a deque push. **I need to read hl2_stream.cpp's EP2-packer source-of-truth before D-1 — see §6.**

---

## 4. Sub-commit plan

5 commits, each build-verified, mirrors the Stage B/C/D cadence:

| Commit | Scope | Wire effect |
|---|---|---|
| **7.1** | `HL2Stream::pushTxIqDoubles` (or `TxIqSource` registration, pending §6 verification). Build verify. | Inert (no caller). |
| **7.2** | `Radio` constructs + owns `TxChannel` (Surface 1 steps 1-2). `txChannel->open()` called at stream start; `close()` at stop. No ILV, no xmtr, no consumer — just channel lifecycle. Build verify + RX-only smoke check (no regression). | TxChannel.cpp objects start being built/destroyed; WDSP TXA channel 1 lifecycle exercised but unused. |
| **7.3** | `Radio` calls `create_ilv` + `create_xmtr_hl2` at start; symmetric destroy at stop. Stage D activates: pxmtr[0] populated. Build verify. | xcmasterTickTx now has a valid bank slot — but no consumer calls it yet. |
| **7.4** | `Radio` registers real `SendpOutboundTx` lambda (Surface 2) + populates `HL2Stream::TxControl` callbacks (Surface 4). Build verify. | TX-out chain wired end-to-end except: no producer driving xcmasterTickTx yet. |
| **7.5** | `Radio` constructs `Hl2Ep6MicSource` + registers consumer lambda calling `xcmasterTickTx(0, …)` (Surface 3 + 5). Build verify. **FIRST-RF CANDIDATE COMMIT.** Operator HL2 bench-gate: opt in to PA, key MOX into dummy, expect RF on the Palstar. |

**Each commit is independently revertable.** 7.5 is the gate; 7.1-7.4 are wire-quiescent buildup that doesn't change runtime behavior until the consumer activates them.

---

## 5. Scope isolation from Stage 2a/2b (RX wire-LIVE work)

**Stage 2a/2b touches** (per `STEP14_STAGE2B_DESIGN.md`):
- `Ep6RecvThread.{h,cpp}` — RX recv-loop migration, new counters, mic-sink scaffolding.
- `HL2Stream` — RX worker rip (`rxWorker_` jthread retire), `Ep6RecvThread` member integration, `txSeq_` migration to shared `g_metis_out_seq_num`.

**Stage 7 touches:**
- `Radio` — adds TxChannel/ILV/xmtr lifecycle, mic-consumer wiring (NEW surfaces).
- `HL2Stream` — adds `pushTxIqDoubles` method (NEW); populates `txControl_` via the existing `registerTxControl` API.
- `Hl2Ep6MicSource` — used as-is.

**Zero overlap with Stage 2b file touches.** Both can land in any order. Stage 2b's TX-side work (`txWorker_` shared counter) is on the EP2 *writer* path; Stage 7's `pushTxIqDoubles` is on the EP2 *producer* path (writes to `_tx_iq` deque the writer drains). No collision.

---

## 6. Open question — verify-don't-guess before D-1

Before writing 7.1, I must verify: **does the existing EP2 TX-packer read from a push'd `_tx_iq` deque (mirroring `_tx_audio`), OR from a pull-based `TxIqSource` callback registered via `registerTxIqSource`?**

`hl2_stream.h:876-880` says `registerTxIqSource` is "for the EP2 packer to pull from when (MOX && injectTxIq) is true on a given datagram" — strongly suggests PULL model. If so, Surface 2's lambda doesn't push to a deque; it stores into a TxIqSource closure that the EP2 packer calls. Different lifecycle, different threading.

**Action before 7.1 code:** `grep` for `registerTxIqSource` callers + read the EP2 packer body in `hl2_stream.cpp` to determine the correct shape. ~15 min, no code change.

---

## 7. Bench gate criteria (commit 7.5)

**No-regression (pre-existing operator-confirmed RX behavior — applies to 7.1-7.4):**
- Branch builds clean (EXITCODE=0, zero warnings) at every commit.
- `_b.bat` post-commit smoke: lyra.exe launches, RX functions normally on real HL2+, audio out works (the Stage B AAMix RX path is byte-identical at every step — Stage 7 doesn't touch RX wiring).

**First-RF gate (commit 7.5 only):**
1. Operator opts in to `Settings → TX → Advanced → "Enable PA"` (default OFF, per Stage 3.6 PA-enable safety).
2. Operator keys MOX (or TUN) into a dummy load on real HL2+.
3. **Expected:** Palstar reads ~5W at full drive, PA-current telemetry banner reads ~1.8A (matches the operator's known-good Thetis A/B anchor per the MEMORY trail).
4. **Stuck-carrier gate (mandatory before any real-antenna keying):** `taskkill /F` Lyra mid-TX into dummy → HL2 gateware watchdog drops PA-bias within N seconds (TX-UNVERIFIED today; this gate verifies it).

**Failure modes that block first-RF:**
- Build error / link error / runtime crash → revert + diagnose.
- RX regression → revert; Stage 7 must be RX-byte-identical for 7.1-7.4.
- TX I/Q reaches EP2 wire but no RF → likely Surface 2 lambda mis-wired; check pilv[0]->Outbound is registered after pxmtr[0] populated.
- TX keys but mic produces silence → likely Surface 3 consumer not registered before HL2Stream::open (set-once-before-start contract violated); check ordering.

---

## 8. Charter §6 questions (per operator-locked standing red-team gate)

**(a) Will this cause TX hangs / break-up / stutter on TX or RX, now or once the full TX/PS/EQ/Combinator/RTA load is added?**

No, by construction:
- Stage 7 adds zero new threads. The Ep6 rx-loop thread that drives Hl2Ep6MicSource is the SAME thread that drives xcmasterTickTx → TxChannel::process → fexchange0 → xilv → SendpOutboundTx → pushTxIqDoubles. Single-thread sequential dispatch — no fan-out, no queueing, no GIL-equivalent contention.
- All Stage 7 hot-path work (mic block arrival → EP2 deque push) runs in the time budget of one ~ms EP6 datagram window. WDSP TXA chain CPU is small (~hundreds of μs) per the operator's Python-tree A/B data.
- Future TX/PS/EQ/Combinator/RTA load lands INSIDE the TXA chain (as additional WDSP cffi stages OR Lyra-native pre-processors per §15.19). The CPU budget growth is bounded by what the Ep6 rx-loop period (~ms) permits; if a future feature exceeds that, it's the future feature's responsibility to move off the rx-loop thread (e.g., dedicated TX-DSP thread), NOT a Stage 7 concern.

**(b) How do Thetis / HPSDR family / other SDR apps that drive HL2/ANAN/Brick handle this exact problem?**

Reference `cmaster.c::xcmaster(int stream)` case 1 (lines 381-407) is called by the per-stream pump thread (`xcmaster` is a public PORT function). The pump thread reads mic samples (via `Inbound()` → cmbuffs ring), calls `xcmaster`, which calls `xilv` → `Outbound` → the host-registered TX-out callback (`SendpOutboundTx`-registered → the EP2 writer's input ring).

Lyra-cpp Stage 7 mirrors this exactly:
- Hl2Ep6MicSource = Lyra's `Inbound`-equivalent (mic samples → consumer callback).
- xcmasterTickTx = Lyra's per-stream pump body (just case 1; no RX migration into the central pump — RX stays in WdspEngine per Lyra's pre-existing arch).
- ILV.Outbound = reference's `pcm->OutboundTx`-registered callback.
- pushTxIqDoubles → `_tx_iq` deque = reference's CMB-buffers / Outbound-pull equivalent for EP2.

Architecture parity: identical sequential-dispatch model.

---

## 9. Findings + Recommendation

**Findings summary:**
- 5 well-scoped surfaces; all the prerequisite infrastructure (TxChannel, ILV, xcmaster, Hl2Ep6MicSource, TxControl) is already shipped + tested.
- Surface isolation from Stage 2a/2b is verified clean — no file collision, no API collision.
- B.6.b-class landmines reviewed at every surface; teardown ordering pins (clear callbacks before destroying registry) preserve discipline.
- One open verify-don't-guess question (§6) resolves in ~15 min of grep before 7.1.
- 5-commit cadence matches Stage B/C/D rhythm; each independently revertable.
- First-RF bench gate is well-defined + observable (PA-current telemetry, Palstar watt meter).

### Recommendation

**LAND STAGE 7 PER THIS PLAN.** 5 sub-commits, plan-paper-then-code, per the locked methodology. Commit 7.5 produces first-RF on the rebuilt path.

**Operator decision points:**
1. **Approve the plan as written?** Or request a 2-agent red-team pass first (mirrors the `STEP14_STAGE2B_DESIGN.md` "must be reviewed by 2 independent senior agents" discipline)?
2. **Resolve §6 open question — should I read the EP2 packer source-of-truth + amend the plan before 7.1?** (Recommended yes.)
3. **TxControl setInjectTxIq wiring scope:** does Stage 7.4 wire the existing FSM in `src/tx/PttFsm.{h,cpp}` to call `HL2Stream::setInjectTxIq`, or is the FSM-wiring a separate downstream commit? Affects 7.4 scope.

I won't proceed to code until you signal on these. Stage E.1 (TX analyzer port to reference parity, PS prereq) remains the locked next step AFTER Stage 7 ships first-RF.

---

## 10. References

- This audit: `docs/architecture/STAGE_E_TXCHANNEL_AUDIT.md` (Stage E.0, scope-anchor for Stage 7)
- Wire-rebuild parent plan: `docs/architecture/STEP14_PLAN.md` + `STEP14_STAGE2B_DESIGN.md`
- Locked methodology: `docs/THETIS_DIRECT_PORT_PLAN.md` §Locked Methodology
- Stage commits on `tx-rebuild`:
  - Stage A `RadioNet.cpp:272` (B.6.b-fix, SendpOutboundRx no-op stub removed)
  - Stage B `0440186`→`533b06b` (aamix.c port + Stage B.6.b RX migration)
  - Stage C `0440186`→`2511099` (ilv.c port + SendpOutboundTx hand-off + Stage A no-op stub removed)
  - Stage D `f564c7b`→`744ba88` (xcmaster pump body + pxmtr[] bank + bit-exact unit test)
  - Stage E.0 audit `b0a202f`+`9b52b2e` (zero MUST-FIX; surface #5 TX-analyzer reclassified MUST-FIX post-Stage-7 per PS prereq)
- File:line cites grounded in current `tx-rebuild` branch state @ `744ba88`.
