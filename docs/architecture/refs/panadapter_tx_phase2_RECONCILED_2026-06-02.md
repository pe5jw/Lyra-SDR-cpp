# Task #44 Phase 2 — RECONCILED v2 / v2.2 plan (post red-team + PS-fidelity correction)

**Status:** v2.2 LOCKED 2026-06-02 PM — operator green-lit
("go") after the §F PS-fidelity correction (pre-iqc tap via
`TXAGetaSipF1`).  Implementation in progress.

**v2 → v2.2 delta:** v2's "feed Spectrum0 from `txIqBuf_` at
TX worker block-pack site" tap point was post-iqc (verified
Agent B amendment A.1).  Operator instinct: that would cause
real PS UX problems (PS-active TX would render visible
predistortion spurs on the panadapter — operator sees "spurs
→ my TX is splattering" → turns off PS → defeats feature).
WDSP exposes `TXAGetaSipF1(channel, float* out, int size)`
at `wdsp/siphon.c:268-282` — pulls pre-iqc complex samples
from the TX sip1 ring (16384-sample capacity per
`TXA.c:394-403`).  v2.2 swaps the data source to that API
call — same MOX-edge swap, same analyzer reuse, same locking
discipline; only the source of the samples fed to Spectrum0
changes.  See §F below for full delta + revised step-4 stub.

**Original status (preserved):** Plan v1
(`panadapter_tx_phase2_PLAN_2026-06-02.md`) + 2-agent red-team
reviews reconciled 2026-06-02 PM.

**Methodology:** §15.25 lock — Plan → 2 independent red-teams →
reconcile → operator-approve → implement → operator HL2 bench
gate.  Both red-teams CONFIRM-WITH-AMENDMENTS; no BLOCKS-SHIP
defects.

---

## Red-team verdicts at a glance

| Lens | Verdict | Amendments |
|---|---|---|
| Concurrency / threading / lifecycle / wire-cadence (Agent A) | **CONFIRM-WITH-AMENDMENTS** | 8 |
| WDSP API / analyzer reconfig / sip1 tap correctness (Agent B) | **CONFIRM-WITH-AMENDMENTS** | 4 (1 structural) |

Both verified the §1 premise correction (RX channel runs through
MOX; `setTxMuted` only gates audio — `wdsp_engine.cpp:2087-2092`).
Both verified the source-swap mechanism is sound (live SetAnalyzer
is WDSP-supported per `wdsp/analyzer.c:1213-1335`, channelMtx_
serializes lifecycle vs feed per `wdsp_engine.cpp:1972`).

Single STRUCTURAL correction: PS forward-compat tap-point claim
(Agent B Amendment 1) — see §A.1 below.  Defensible for v0.2;
deliberate divergence for v0.3 PS or revisit then.

---

## §A.  Reconciled amendments (folded from both red-teams)

### A.1.  STRUCTURAL — PS forward-compat tap-point claim corrected (Agent B #1)

**Original plan claim:** "sip1 is BEFORE iqc.  Feeding panadapter
from EP2-output buffer (`txIqBuf_` = post-WDSP-TXA output) means
panadapter shows post-ALC pre-iqc TX I/Q."

**Verified-wrong against TXA.c:580-590:**

```
xmeter(alcmeter)              // ALC meter
xsiphon(sip1, 0)              // line 586 — pre-iqc, dsp_rate
xiqc(iqc.p0)                  // line 587 — PureSignal correction
xcfir(cfir)                   // P2-only compensating FIR
xresample(rsmpout)            // output resampler dsp_rate → out_rate
xmeter(outmeter)              // output meter
                              // → outbuff → fexchange0 caller
```

So `sip1` captures at **dsp_rate (96 kHz) BEFORE iqc + BEFORE
rsmpout**.  `outbuff` (what fexchange0 returns into
`tx_channel.cpp:414` `outBuf_`, which lyra-cpp then de-interleaves
into `txIqBuf_`) is **AFTER iqc + AFTER rsmpout**, at 48 kHz.

**In v0.2** (today): `iqc.run = 0`, xiqc is a pass-through.
Observationally identical to a pre-iqc stream — panadapter
content is correct.

**When v0.3 PS lands:** iqc.run=1; `txIqBuf_` carries the
post-PS-corrected signal (same as the wire).  Reference TX
panadapter shows the pre-iqc signal via sip1 (TXA.c:586).
Lyra would diverge: panadapter shows post-PS-correction
content.

**Resolution (operator decision deferrable to v0.3 PS):**
* **(a) Accept the divergence + document it.**  Operator sees
  the actual wire output (= what hits the antenna).  Arguably
  what most operators want — "show me what I'm transmitting"
  is the post-PS signal.  Reference-divergent but operator-
  intuitive.  Zero code work today; v0.3 PS docs note the
  divergence.
* **(b) Plumb a real pre-iqc sip1 tap.**  Requires TxChannel-
  level changes (intercept dsp_rate samples before rsmpout
  feeds them into the output resampler).  Large enough for
  its own design pass.  Defer to v0.3 PS work where the
  semantics actually matter.

**v2 LOCK: option (a).**  v0.2 ships from `txIqBuf_` (current
plan unchanged).  TXA chain order comment updated.  v0.3 PS
work re-evaluates if operator wants a Settings toggle "TX
panadapter: pre-PS / post-PS".  Honest doc trail in §13.7-
follow-up + future v0.3 design doc.

### A.2.  Keydown sequence — eliminate §1 Q5 vs §2 step 5 contradiction (Agent A #2)

**Defect:** Plan §1 Q5 step 1 said "flag TRUE first, then
SetAnalyzer".  §2 step 5 said "SetAnalyzer first, THEN flip
flag".  Inconsistent.  Race window if §1 Q5 followed literally:
TX worker observes flag=true and calls `feedTxSpectrum(126)`
against analyzer still sized for 1024 — WDSP `Spectrum0` with
`bf_sz != configured bf_sz` is undefined.

**v2 LOCK — canonical keydown:**
1. Main under `channelMtx_`: `configureAnalyzerForTx()`
   (reconfigures sample-rate + bf_sz + overlap + max_w on
   kAnDisp).
2. Main: `txOwnsAnalyzer_.store(true, std::memory_order_release)`.
3. RX worker on next `feedIq` reads
   `txOwnsAnalyzer_.load(std::memory_order_acquire)` → sees
   true → skips Spectrum0.  Audio continues (txMuted handles
   mute separately).
4. TX worker on next block-pack tick reads same flag → sees
   true → calls `feedTxSpectrum(...)` against the now-
   correctly-sized analyzer.

**v2 LOCK — canonical keyup:**
1. Main under `channelMtx_`: `configureAnalyzerForRx()`
   (restores RX rate/bf_sz).
2. Main: `txOwnsAnalyzer_.store(false, std::memory_order_release)`.
3. RX worker next feedIq → sees false → resumes Spectrum0.
4. TX worker next block-pack tick (if any: `injectTxIq_`
   already false post-FSM) → sees flag false → no-op.

Release/acquire makes the SetAnalyzer side-effects happen-
before the flag observation on any reader — `channelMtx_`
provides the synchronization but the explicit fence removes
reader-side reordering ambiguity.

### A.3.  Helper lock convention — caller-holds (Agent A #1)

**Defect:** Plan said `configureAnalyzerForRx()` takes
`channelMtx_`.  But `openRx1` (`wdsp_engine.cpp:387-630`) does
NOT take the lock; caller does (`setSampleRate:771`).  If the
helper takes the lock itself, `setSampleRate`'s already-locked
path deadlocks.

**v2 LOCK:** Both `configureAnalyzerForRx()` and
`configureAnalyzerForTx()` follow the openRx1 pattern:

```cpp
// PRECONDITION: channelMtx_ held by caller.
void WdspEngine::configureAnalyzerForRx() noexcept;
void WdspEngine::configureAnalyzerForTx() noexcept;
```

Callers acquire:
* `openRx1()` — already holds lock via `setSampleRate` caller.
* `setTxOwnsAnalyzer(bool on)` — acquires lock before calling
  either helper.
* `setSampleRate()` — already holds lock; calls openRx1 which
  calls configureAnalyzerForRx (precondition satisfied).

### A.4.  Step-4 stub double-gate (Agent A #3)

**Defect:** Plan §2 step 4 stub gated only on `wdspEngine_->
txOwnsAnalyzer()`.  Block-pack site at `tx_dsp_worker.cpp:339-
359` runs whenever worker has ≥ kEp2BlockSize samples — could
fire with stale `txIqBuf_` from prior keyup that current packs
didn't refresh.

**v2 LOCK — Step-4 code:**

```cpp
// Phase 2: feed panadapter analyzer with same post-WDSP-TXA
// block.  Double-gated: txOwnsAnalyzer_ (MOX-edge swap) AND
// injectTxIq_ (per-block FSM gate that already guards EP2
// emission).  Both gates prevent stale-buffer feed during
// FSM mid-keyup transients.
//
// Forward-compat note (v2 amendment A.1): when v0.3 PS lands
// and iqc activates, this feed carries post-iqc content
// (matches wire output, diverges from reference's pre-iqc
// panadapter posture).  Deliberate; operator decision in v0.3.
if (wdspEngine_
        && wdspEngine_->txOwnsAnalyzer()
        && injectTxIq_.load(std::memory_order_acquire)) {
    // Spectrum0 takes interleaved DOUBLE IQ; txIqBuf_ is
    // complex<float>.  Convert in place into a stack-local
    // 2*kEp2BlockSize-double scratch.
    double iqd[2 * kEp2BlockSize];
    for (int i = 0; i < kEp2BlockSize; ++i) {
        iqd[2*i + 0] = static_cast<double>(txIqBuf_[i].real());
        iqd[2*i + 1] = static_cast<double>(txIqBuf_[i].imag());
    }
    wdspEngine_->feedTxSpectrum(iqd, kEp2BlockSize);
}
```

### A.5.  feedTxSpectrum lock scope + block-size flexibility (Agent A #6 + Agent B #4)

**Defect (A6):** Plan hardcoded `n != 126` check inside
`feedTxSpectrum`.  HL2/P1-specific; v0.4 ANAN/P2 different
block sizes (§6.7 #1 — "no magic constants").

**Defect (B4):** `feedTxSpectrum` under `channelMtx_` could
block tens of ms while RX worker holds the lock across an
entire `feedIq` body (`wdsp_engine.cpp:1972` — whole-method
scope).  TX worker wedge = wire-cadence hazard.

**v2 LOCK:** Introduce a NEW finer-grained mutex
`analyzerMtx_` distinct from `channelMtx_`.  Scope: protects
SetAnalyzer reconfigure vs Spectrum0 feeds.  Held briefly:
~microseconds for Spectrum0 (memcpy + internal ring inc),
larger for SetAnalyzer (drain + reconfigure, but only on MOX
edge).

```cpp
// PRECONDITION: analyzerMtx_ NOT held by caller (this is a
// hot-path call from the TX worker; channelMtx_ is too coarse).
// Returns false on a configured-block-size mismatch — the caller
// should treat that as "skip this frame" not "fatal".
bool WdspEngine::feedTxSpectrum(const double *iq, int n) noexcept;
```

Implementation:
```cpp
bool WdspEngine::feedTxSpectrum(const double *iq, int n) noexcept {
    std::lock_guard<std::mutex> lk(analyzerMtx_);
    if (!analyzerOpen_) return false;
    if (n != txAnalyzerBfSize_.load(std::memory_order_relaxed))
        return false;  // mid-reconfigure: drop this block
    api_.Spectrum0(1, kAnDisp, 0, 0, const_cast<double*>(iq));
    return true;
}
```

`configureAnalyzerForTx()` takes `analyzerMtx_` while calling
SetAnalyzer + updates `txAnalyzerBfSize_` atomic.  Same for
`configureAnalyzerForRx()`.  Spectrum0 feed and SetAnalyzer
calls serialize against each other through `analyzerMtx_`;
the (heavier) `channelMtx_` is held by RX `feedIq` whole-method
but does NOT serialize against the TX worker's feedTxSpectrum.
Both are independently safe — WDSP's `SetAnalyzerSection` /
`PB_ControlsSection` internal locks (verified per Agent B's
`analyzer.c:1213-1335` read) handle WDSP-internal serialization;
`analyzerMtx_` is Lyra-side coordination of the new code paths.

Block-size flex: `txAnalyzerBfSize_` is set at SetAnalyzer
time, checked at feed time.  Caller passes whatever size their
TX chain uses (today 126; ANAN P2 could be different — engine
doesn't care).  Removes the hardcode.

### A.6.  cfg_.inRate race in spanHz (Agent A #7)

**Defect:** `spanHz()` reads `cfg_.inRate` with no atomic /
no lock; `setSampleRate` mutates it under `channelMtx_`.  Data
race on a non-atomic int.  Pre-existing; plan inherits.

**v2 LOCK:** Atomicize `cfg_.inRate` (or introduce a parallel
`std::atomic<int> inRateAtomic_` mirror updated under lock).
One-line fix; no behaviour change.

### A.7.  Slot ordering at the new connect (Agent A #4)

**Defect:** Slot ordering at `moxActiveChanged` is load-bearing
for §15.26 PART B (audio-mute-leads-analyzer-swap).  A future
maintainer reordering the connects in `main.cpp` could un-mute
audio AFTER SetAnalyzer reconfigures = brief noise emission
during a keyup window.

**v2 LOCK:** Add explicit comment at the new connect:

```cpp
// MUST be connected AFTER the setTxMuted connect above
// (main.cpp:376) — Qt guarantees connection-order
// DirectConnection slot dispatch, and §15.26 PART B audio-
// mute-leads-analyzer-swap is load-bearing.  Reordering these
// connects can briefly emit audio between SetAnalyzer
// reconfigure and audio re-mute on keyup.
QObject::connect(stream, &lyra::ipc::HL2Stream::moxActiveChanged,
                 wdspEngine, [wdspEngine](bool on) {
    wdspEngine->setTxOwnsAnalyzer(on);
});
```

### A.8.  Bench gate addition: TX-worker block-pack period jitter (Agent A #8)

**v2 LOCK:** §4 bench step 5 item 10 extended:
* `LYRA_WIRE_DEBUG=1`: EP2 cadence + MAINSTALL distribution
  unchanged from pre-Phase-2 baseline.
* **NEW:** TX-worker block-pack period — log mean + p95
  worker-loop period at 1 Hz (gated on LYRA_TX_PERF=1 env var
  or similar).  Spectrum0 should be cheap (memcpy + ring inc)
  but verify it doesn't bloat the worker-loop period.  Target:
  no measurable shift in mean or p95 vs pre-Phase-2.

### A.9.  Citation/wording fixes (Agent B #2 + #3)

**v2 LOCK:**
* "Below kAnMaxFft=32768, safe" → "well under bsize allocated
  at XCreateAnalyzer time" (more accurate; kAnMaxFft is FFT-
  size max not max_w bound).
* Q1 cons-of-two-analyzer rationale: change "matches reference
  single-widget posture" → "matches single-renderer/single-
  cache posture".  Reference actually uses SEPARATE analyzer
  instances per channel (`Console/HPSDR/specHPSDR.cs:40-42`);
  Lyra's single-analyzer reuse is a DIVERGENCE defended by
  Lyra's single-renderer simplicity, not by reference
  precedent.  Honest.

### A.10.  Pre-existing "Thetis" cleanup (Agent A #5)

**v2 LOCK:** SEPARATE task, not gating Phase 2.  Track as a
new backlog item:

> Task #73 — Cleanup pre-existing "Thetis" mentions in shipped
> code per §15.26 no-attribution rule.  Sites:
> `hl2_stream.h:725-726`, `hl2_stream.cpp:1136,1446`,
> `tx_dsp_worker.cpp:37-39`, `tci_server.cpp:1212,1220,1257,
> 1292`, `tci_server.h:206`, `tci_mic_source.h:11`.
> `mainwindow.cpp:632` (Credits/About) is intentional (Task
> #58); leave.  Reword each comment in first-principles RF
> terms.  Pure doc-string change; zero behaviour impact.

### A.11.  Averaging-state leak on SetAnalyzer reconfigure (Agent B implicit)

**Caveat:** SetAnalyzer (`wdsp/analyzer.c:1303-1332`) resets
buffer indices + stitch state but does NOT reset averaging
accumulators (`av_buff`, `av_sum`, `avail_frames`, `av_in_idx`,
`av_out_idx`, `pre_av_out`, `t_pixels`).  With operator-default
`kAnAvgMode = 0` (averaging off), no glitch.  If averaging is
later enabled, MOX-edge swap visually leaks RX averaging
state into TX (and vice-versa).

**v2 LOCK:** Plan's §2 step 6 (optional polish) already covers
visible glitches on the swap edge.  No code change needed for
v0.2 (avgMode=0 default).  If a future operator enables
averaging + reports the leak, add a `ResetPixelBuffers(kAnDisp)`
binding + call from `configureAnalyzerForTx/Rx()`.

---

## §B.  Updated implementation steps (v2)

Numbering preserved from v1; deltas marked.

### Step 1 (v2-amended) — Helpers + caller-holds convention

**Files:** `src/wdsp_engine.cpp`, `src/wdsp_engine.h`.

* Extract SetAnalyzer body from `wdsp_engine.cpp:486-511` into
  private `WdspEngine::configureAnalyzerForRx()`.  **Document
  PRECONDITION: channelMtx_ held by caller** (matches openRx1
  convention).  Call from `openRx1()` inline body.
* Add `WdspEngine::configureAnalyzerForTx()` — same shape with
  rate=48000, bf_sz=126, overlap_tx=3296, max_w_tx=8896.  Same
  PRECONDITION docstring.
* **NEW (A.5):** Both helpers also acquire `analyzerMtx_`
  internally (NOT channelMtx_) and update `txAnalyzerBfSize_`
  atomic.

Verification: openRx1 path unchanged.

### Step 2 (v2-amended) — Atomics + spanHz race fix

**Files:** `src/wdsp_engine.h`, `src/wdsp_engine.cpp`.

* `std::atomic<bool> txOwnsAnalyzer_{false};`
* `std::atomic<int>  txSpanHz_{48000};`
* `std::atomic<int>  txAnalyzerBfSize_{0};` (A.5)
* **NEW (A.6):** atomicize `cfg_.inRate` (or add parallel
  `std::atomic<int> inRateAtomic_` mirror; pick whichever
  touches fewer call sites).
* `bool txOwnsAnalyzer() const { return txOwnsAnalyzer_.load(
   std::memory_order_acquire); }`
* `spanHz()` override (per A.2 release/acquire discipline):
  ```cpp
  int spanHz() const {
      if (txOwnsAnalyzer_.load(std::memory_order_acquire)) {
          const int z = std::max(1, static_cast<int>(
              zoom_.load(std::memory_order_relaxed)));
          return txSpanHz_.load(std::memory_order_relaxed) / z;
      }
      const double z = zoom_.load(std::memory_order_relaxed);
      const int r = inRateAtomic_.load(std::memory_order_relaxed);
      return static_cast<int>(r / (z > 1.0 ? z : 1.0));
  }
  ```

### Step 3 — Wire WdspEngine pointer into TxDspWorker

Unchanged from v1.

### Step 4 (v2-amended) — TX worker feeds Spectrum0 with double-gate

**File:** `src/tx_dsp_worker.cpp` (block-pack site near
339-359).

Use the code stub in §A.4 above (double-gate `txOwnsAnalyzer()
&& injectTxIq_.load()`).

Add `WdspEngine::feedTxSpectrum(const double *iq, int n)`
public method per §A.5 spec (returns bool, takes analyzerMtx_,
checks `n == txAnalyzerBfSize_`).

### Step 5 (v2-amended) — MOX-edge wiring with locked slot order

**File:** `src/main.cpp`.

Add the new connect AFTER the existing setTxMuted connect at
`main.cpp:376-377`.  Include the §A.7 explicit ordering
comment.

`WdspEngine::setTxOwnsAnalyzer(bool on)`:
```cpp
void WdspEngine::setTxOwnsAnalyzer(bool on) {
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (on) {
            configureAnalyzerForTx();
        } else {
            configureAnalyzerForRx();
        }
    }
    txOwnsAnalyzer_.store(on, std::memory_order_release);
    emit spanChanged();
}
```

Keydown order is reconfigure (under channelMtx_) → flag flip
(release) → readers see reconfigured analyzer (acquire).  Keyup
is symmetric.

### Step 6 — Optional polish on swap-edge glitches

Unchanged from v1 (still optional; only ship if first-bring-up
shows visible glitch).

---

## §C.  Updated risks + mitigations (v2)

| Risk | Mitigation (v2-updated) |
|---|---|
| Per-block Spectrum0 jitter on TX worker | Bench-gate addition A.8; benchmark mean + p95 worker-loop period under LYRA_TX_PERF=1. |
| SetAnalyzer race vs in-flight RX feedIq | `analyzerMtx_` (NEW, fine-grained) serializes SetAnalyzer vs Spectrum0; `channelMtx_` separately serializes lifecycle vs feedIq.  Both safe independently. |
| WDSP doesn't tolerate live SetAnalyzer | Agent B verified `analyzer.c:1213-1335` — SetAnalyzer takes its own critical section + drains threads + reconfigures + clears stop.  Designed for live reconfigure.  Fallback: two analyzer instances (kTxDisp=1, fully spec'd). |
| Flag-vs-reconfig ordering race | `memory_order_release` on store + `memory_order_acquire` on load + reconfigure-before-flag-flip discipline = guaranteed happen-before.  Documented at every store/load site. |
| GUI copySpectrum mid-call vs SetAnalyzer | WDSP's `SetAnalyzerSection` + `PB_ControlsSection` (Agent B verified) handle internal serialization.  GUI gets at-worst one torn frame at reconfigure.  Cosmetic. |
| Slot ordering: setTxMuted vs analyzer swap | §A.7 explicit comment locks order at the connect site.  §15.26 PART B integrity preserved. |
| `cfg_.inRate` mid-MOX rate change | Atomicized (A.6); `setSampleRate` reopens channel → configureAnalyzerForRx applies new rate on next keyup.  Mid-MOX rate change is rare but handled. |
| sip1 ring 2-consumer trap (Phase 2 + v0.3 PS) | Phase 2 feeds from `txIqBuf_` at producer site, NOT from sip1 ring.  v0.3 PS calcc remains the 1 sip1 consumer.  No 2-consumer problem. |
| v0.3 PS panadapter content silently changes (Agent B #1) | Deliberate divergence (option a in §A.1) — operator sees actual wire output post-PS.  v0.3 design re-evaluates a Settings toggle if operator wants pre-PS view.  Doc updated. |

---

## §D.  Operator decision points (for go-ahead)

1. **Confirm A.1 option (a)** — accept the post-iqc tap point
   (panadapter shows actual wire output, diverges from
   reference's pre-iqc when v0.3 PS lands).  Alternative is
   option (b) which adds TxChannel-level work; recommend
   deferring that to v0.3 if it matters then.
2. **Confirm overall Phase 2 scope** — 6 steps, ~250-350 LOC,
   wire-bytes byte-identical, each step individually
   revertible, MeterModel:218 MOX-swap pattern reused.
3. **Confirm the bench gate** at §4 (revised per A.8 — add
   TX-worker block-pack period jitter measurement).
4. **Confirm Task #73 cleanup** is separate, not gating.

If green-light: implement Steps 1-5 in order, build + commit
each individually, ship step 6 only if first-bench-light shows
the visible glitch.

---

## §E.  Provenance

* Plan v1: `panadapter_tx_phase2_PLAN_2026-06-02.md` (Plan-
  agent output, premise correction noted in v1 header).
* Red-team 1 (concurrency): general-purpose agent, 8
  amendments, agent id `a5e01378519becf75`.
* Red-team 2 (WDSP correctness): general-purpose agent, 4
  amendments (1 STRUCTURAL), agent id `a90ad9d29a945dcb0`.
* Reference verifications:
  * `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/analyzer.c:1213-1335` (SetAnalyzer live-reconfigure safety)
  * `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/analyzer.c:1108-1186` (ResetPixelBuffers — what SetAnalyzer does NOT reset)
  * `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/TXA.c:580-590` (sip1 vs iqc vs rsmpout chain order)
  * `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/Console/HPSDR/specHPSDR.cs:36-43, 487, 532, 585` (per-channel analyzer instances, KEEP_TIME, overlap formula, max_w formula)
* Lyra source verifications cited per amendment.

**Reconciliation outcome (v2):** plan structurally sound; 11
bounded amendments folded into v2; no BLOCKS-SHIP defects;
operator go-ahead is the next gate.

---

## §F.  v2.2 — pre-iqc tap via `TXAGetaSipF1` (PS-fidelity correction)

### Motivation

Operator concern 2026-06-02 PM: "Thetis shows the correct
sourced signal on TX on the panadapter and waterfall with PS
on or off if I recall right?"  Verified correct against
`wdsp/TXA.c:580-590`:

```
xmeter(alcmeter)              // ALC meter
xsiphon(sip1, 0)              // line 586 — display tap (pre-iqc, dsp_rate)
xiqc(iqc.p0)                  // line 587 — PureSignal correction
xresample(rsmpout)            // dsp_rate (96k) → out_rate (48k)
                              // → outbuff → fexchange0 caller
```

Reference panadapter taps sip1 — ALWAYS pre-iqc, regardless
of `iqc.run`.  Operator sees clean modulator output whether PS
is on or off.  PS-active TX: predistortion happens BETWEEN the
panadapter tap and the wire — invisible to the operator.

v2's "feed from `txIqBuf_`" tap was POST-iqc (verified Agent B
A.1).  v0.2 would be observationally identical (iqc.run=0 =
passthrough), but v0.3 PS would silently make Lyra's panadapter
show post-correction content while reference shows pre-
correction.  Real PS UX failure mode (false-alarm spurs).

### WDSP API discovery

`wdsp/siphon.c:253-282` exposes:

```c
PORT
void TXAGetaSipF (int channel, float* out, int size);    // I only
PORT
void TXAGetaSipF1 (int channel, float* out, int size);   // I + Q interleaved
```

`TXAGetaSipF1` is the right one — pulls up to `size` complex
samples (writes 2*size floats: I,Q,I,Q,…) from txa[channel].sip1.
Internally locked via `EnterCriticalSection(&a->update)`.
Calls `suck(a)` to drain the ring into `sipout`.

Sip1 ring capacity = 16384 samples (TXA.c:401-402); FFT size
= 16384 (line 402); samplerate = `ch[channel].dsp_rate` = 96 kHz
for our TXA config (`tx_channel.cpp:140-148`).  Plenty of
headroom for any reasonable read cadence.

### v2.2 data flow

Identical to v2 in every respect EXCEPT the data source:

```
TX worker block-pack tick (~380 Hz)
    │
    ├── (unchanged) accumulator + EP2 hand-off (txIqBuf_)
    ├── (unchanged) sip1Ring_ tee for v0.3 PS calcc
    │
    └── NEW: if (txOwnsAnalyzer && injectTxIq):
            call wdspEngine_->feedTxSpectrum()  →  inside:
                tx_sip_buf[2*N];  // N ≈ 256
                api_.TXAGetaSipF1(1, tx_sip_buf, N);
                for i in [0,N): convert float→double interleave
                api_.Spectrum0(1, kAnDisp, 0, 0, tx_sip_double);
```

Two WDSP calls per Lyra TX worker tick during MOX:
`TXAGetaSipF1` (drains pre-iqc samples from WDSP's sip1 ring,
WDSP-internal lock on `a->update`) + `Spectrum0` (feeds those
samples to the analyzer, WDSP-internal lock on `SetAnalyzerSection`).
Lyra-side `analyzerMtx_` (A.5) serializes our SetAnalyzer
reconfigure vs this feed.

### Read cadence sizing

* WDSP sip1 produces at dsp_rate = 96000 samples/sec.
* TX worker fires at EP2 cadence = `48000 / kEp2BlockSize(126)`
  ≈ 381 Hz.
* If we ask for `N` samples per call, total drain demand =
  `381 * N` samples/sec.  To match production:
  `N ≈ 96000 / 381 ≈ 252`.
* Pick `kTxAnalyzerBlockSamples = 256` (nice power-of-2-ish;
  drain ≈ 97k samples/sec, slightly over-pulls so the ring
  stays drained; `suck(a)` is happy to return < requested if
  short).

### SetAnalyzer params (96 kHz, not 48 kHz)

v2's params were sized for the 48 kHz wire rate.  v2.2 sizes
for the 96 kHz dsp_rate (sip1 native rate):

```
rate_tx        = 96000
bf_sz_tx       = 256                                  (per-call read size)
overlap_tx     = max(0, kAnFftSize - rate_tx/kAnFrameRate)
               = max(0, 4096 - 96000/60)
               = max(0, 4096 - 1600)
               = 2496
max_w_tx       = kAnFftSize + min(rate_tx/10, kAnFftSize*kAnFrameRate/10)
               = 4096 + min(9600, 24576)
               = 13696
```

Below `kAnMaxFft=32768`; safe.

### Span report

`txSpanHz_` becomes 96000 (was 48000 in v2):
```cpp
std::atomic<int> txSpanHz_{96000};
```

Operator sees a wider freq view during MOX vs RX (assuming RX
is at 192k, TX shows a 96k window — half the span).  Zoom
still applies per `spanHz()` getter.

### Step-4 stub (v2.2 final)

Same double-gate as v2 (A.4); only the body changes:

```cpp
// Phase 2 (v2.2): pull pre-iqc TX I/Q from WDSP sip1 ring and
// feed the panadapter analyzer.  Pre-iqc tap (TXA.c:586,
// sip1 is upstream of xiqc) = reference-faithful posture:
// the trace shows the clean modulator output regardless of
// PureSignal state.  When v0.3 PS lands and iqc activates,
// predistortion happens DOWNSTREAM of this tap so the
// panadapter content is unchanged — operator sees what they
// transmit, not the predistorted-for-PA-cancellation signal.
//
// Double-gated: txOwnsAnalyzer_ (MOX-edge swap state) AND
// injectTxIq_ (FSM gate that already guards EP2 emission).
// Both prevent stale-buffer feed during FSM mid-keyup
// transients.
if (wdspEngine_
        && wdspEngine_->txOwnsAnalyzer()
        && injectTxIq_.load(std::memory_order_acquire)) {
    wdspEngine_->feedTxSpectrumFromSip1();
}
```

`WdspEngine::feedTxSpectrumFromSip1()`:

```cpp
bool WdspEngine::feedTxSpectrumFromSip1() noexcept {
    constexpr int kN = 256;
    float  sipBuf[2 * kN];     // interleaved I,Q,I,Q,... (size from TXAGetaSipF1)
    double specBuf[2 * kN];    // interleaved doubles for Spectrum0

    // WDSP drains internally under sip1.update CRITICAL_SECTION.
    // Returns up to kN complex samples into sipBuf; pads with last-
    // good if ring is empty (suck() behaviour — safe).
    api_.TXAGetaSipF1(/*channel=*/1, sipBuf, kN);

    for (int i = 0; i < 2 * kN; ++i)
        specBuf[i] = static_cast<double>(sipBuf[i]);

    std::lock_guard<std::mutex> lk(analyzerMtx_);
    if (!analyzerOpen_) return false;
    if (kN != txAnalyzerBfSize_.load(std::memory_order_relaxed))
        return false;  // mid-reconfigure: drop this frame
    api_.Spectrum0(1, kAnDisp, 0, 0, specBuf);
    return true;
}
```

Note: drop the `(const double *iq, int n)` signature from v2's
spec — v2.2's reader pulls + feeds in one operation (the
caller doesn't supply the data), simpler API.  Same `bf_sz`
check still applies.

### Cdef additions

New `wdsp_native.h` entry:

```cpp
using fn_TXAGetaSipF1_t = void(*)(int channel, float *out, int size);
fn_TXAGetaSipF1_t TXAGetaSipF1 = nullptr;
```

Resolve at load time alongside the other TXA cdefs.

### PureSignal forward-compat (v2.2 confirmed)

When v0.3 PS lands and `iqc.run=1`, the panadapter trace
content is **unchanged** because sip1 is captured upstream of
the iqc stage.  Reference posture preserved with zero
additional Phase 2 code.  This is the right posture: operator
sees what they're modulating; the PS-corrected signal hits
the wire invisibly.

### Risks delta (v2 → v2.2)

| Risk | v2.2 mitigation |
|---|---|
| sip1 ring underrun (Lyra reads faster than WDSP fills) | `suck()` is safe with `< requested` — pads from last-good in ring; visual = brief frozen-trace pixel.  Not a fault. |
| sip1 ring overrun (Lyra reads slower than WDSP fills) | Ring drops oldest (designed behavior at 16384 capacity ≈ 170 ms at 96 kHz).  At 256 samples/call × 381 Hz = ~97k/sec drain vs 96k/sec write — slightly over-pulls; ring stays nearly empty.  No backpressure. |
| WDSP `EnterCriticalSection(&sip1.update)` blocks TX worker | sip1 internal lock held only for the `suck()` call (memcpy + index inc).  Microseconds.  Same envelope as `GetTXAMeter` calls already in the meter hot path. |
| `TXAGetaSipF1` cdef binding | Single cdef + resolve; trivial.  Verify symbol present in our `wdsp.dll` build at load time. |
| Sample rate confusion (operator may expect 48 kHz wire span on TX panadapter, gets 96 kHz dsp_rate span) | Documented in operator-facing help.  Wider span = sees more of the modulator's bandwidth, which is what they want for ESSB and IMD inspection.  Reference shows the same dsp_rate span. |

### v2.2 implementation steps (updated)

Step 1, 2, 3 unchanged from v2.

**Step 3.5 (NEW):** Add `TXAGetaSipF1` cdef + resolve in
`wdsp_native.h` / `wdsp_native.cpp`.  Trivial.  Wire-inert.

**Step 4 (v2.2-amended):** TX worker double-gated call to
`wdspEngine_->feedTxSpectrumFromSip1()` (new method name,
takes no args — pulls + feeds internally).  Use the step-4
stub above.

Step 5, 6 unchanged from v2 (MOX-edge wiring + optional
swap-edge polish).  SetAnalyzer params updated for 96 kHz
math per §F above.

### v2.2 bench gate

Identical to v2 bench step 5 (item 1-11) PLUS:

**NEW item 12:** With v0.3 PS not yet shipped, panadapter
content during MOX should look reference-equivalent on the
same TX signal (clean modulator output, post-ALC spectrum
shape).  When PS v0.3 lands: panadapter trace must remain
visually unchanged when toggling PS on/off (the whole point
of the pre-iqc tap).

### v2.2 status

LOCKED 2026-06-02 PM after operator green-light ("go").
Implementation begins.  Same 6 steps (now 7 with step 3.5
cdef add), each individually revertible, wire-bytes
byte-identical.  No additional red-team needed for the v2.2
delta — the change is purely the data source within step 4
(swap `txIqBuf_` reference for `TXAGetaSipF1` call); the
locking discipline, MOX-edge swap mechanism, helper
convention, ordering semantics, and PS forward-compat
posture are all unchanged or strictly improved.

