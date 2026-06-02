# Task #44 Phase 2 — implementation plan (Plan-agent output, pre-red-team)

**Status:** Plan-agent output 2026-06-02 PM, AWAITING red-team
review.  Do NOT implement from this until §15.25 red-team
methodology completes (2 independent agents → reconcile loop
until converged → operator go-ahead).  Companion to
`panadapter_tx_audit_THETIS_2026-06-02.md` and
`panadapter_tx_audit_LYRA_2026-06-02.md`.

---

## ⚠ Premise correction caught by the Plan agent

The LYRA audit's claim that "panadapter continues displaying the
frozen RX trace during MOX (because RX channel is stopped per
§15.25)" is FALSE for lyra-cpp.  That's Python-lyra behaviour.

lyra-cpp's `WdspEngine::setTxMuted` (`src/wdsp_engine.cpp:2087`)
only gates AUDIO output — not `feedIq`, not `Spectrum0`.  The RX
worker thread keeps feeding raw RX I/Q into the analyzer every
block during MOX.  So today's lyra-cpp TX-state panadapter shows
**live RX spectrum** (typically blinded by §15.26 ATT-on-TX
RX-LNA cut + operator's own TX coupling — the documented
"wavey waveform" §15.26 pathology), NOT a frozen trace.

Plan below assumes this lyra-cpp reality.  The LYRA audit's
§B section should be patched on a follow-up doc commit to note
this correction.

---

## §1 Design choices (Q1–Q10 answered)

### Q1.  Single analyzer reuse vs separate TX analyzer

**Decision: REUSE `kAnDisp`; swap the input source feeding
`Spectrum0` on the MOX edge.**

Today's analyzer: `kAnDisp = 0` (`wdsp_engine.cpp:68`), created
in `openRx1()` at `wdsp_engine.cpp:480` via
`XCreateAnalyzer(kAnDisp, &success, kAnMaxFft, 1, 1, ...)` and
configured at `:494-511` with `bf_sz=cfg_.inSize` and
`sz=kAnFftSize=4096`.

Fed from ONE site: `Spectrum0(1, kAnDisp, 0, 0, dspPtr)` at
`wdsp_engine.cpp:2034-2035`, called from `feedIq()` on the RX
worker thread.  Read via `copySpectrum` at
`wdsp_engine.cpp:799-851` (GUI thread); WDSP serialises
feed-vs-read internally.

**Pros of reuse:** one renderer, one cache (`specCache_`,
`wdsp_engine.cpp:818-823`), one set of `kAnPixels` (4096), one
set of QML bindings, ZERO new code in `panadapter.cpp`.
Display-side zoom crop in `copySpectrum` (`:825-849`) keeps
working unchanged.  Matches reference single-widget posture
(Thetis audit §1.d).

**Cons of reuse:** sample-rate mismatch (Q2) — RX analyzer at
192 kHz block size, TX sip1 at 48 kHz; SetAnalyzer reconfigure
on MOX edge.

**Cons of two-analyzer:** doubles analyzer memory + state;
`copySpectrum`/`specCache_`/zoom-crop math assume one
analyzer/one frame size; pick a `kTxDisp=1` and prove WDSP's
two-instance path.

### Q2.  Sample-rate handling

**Decision: `SetAnalyzer`-reconfigure kAnDisp on MOX edge with
TX-side block size + rate; accept TX span = 48 kHz.  On keyup,
restore RX-side config.**

- TX sip1 chunks: 126 samples complex @ 48 kHz
  (`tx_dsp_worker.h:335-341 kEp2BlockSize`,
  `tx_dsp_worker.cpp:339-359`).
- SetAnalyzer's `bf_sz` is feed block size (`wdsp_engine.cpp:501`
  = `cfg_.inSize` = 1024 currently).  Overlap = `fft -
  rate/frame_rate` (`:486-487`).  Feeding 126 doubles into an
  analyzer set up for 1024 will not crash but produces mostly
  black until accumulated feeds.
- "TX span = 48 kHz regardless of RX setting" matches Thetis
  audit's "fixed TX freq span" finding (§2.b).  48 kHz spans
  more than expected TX so all sub-modes fit.
- Glitch concern: RX is not being looked at during MOX (RX
  analyzer pixels in cache; first feed-after-swap shows TX).
  On keyup swap back; cache holds last TX frame for one render
  tick until fresh RX feeds rebuild (`:818-823` retain-last-good).
  Cosmetic.

**Concrete keydown sequence:**
1. Don't tear down — `SetAnalyzer` reconfigures kAnDisp.
2. `SetAnalyzer(kAnDisp, 1, 1, 1, flp, kAnFftSize, 126,
   kAnWindow, kAnKaiserPi, overlap_tx, 0, 0.0, 0.0, kAnPixels,
   1, 0, 0.0, 0.0, max_w_tx)` with
   `overlap_tx = max(0, kAnFftSize - 48000/kAnFrameRate) =
   max(0, 4096 - 800) = 3296` and
   `max_w_tx = kAnFftSize + min(48000/10,
   kAnFftSize*kAnFrameRate/10) = 4096 + 4800 = 8896`.  Below
   `kAnMaxFft=32768`, safe.
3. Optional one-shot `Spectrum0(0, kAnDisp, …)` to flush; then
   resume `Spectrum0(1, kAnDisp, …)` from TX feed path.

First TX frame visible after `ceil(4096/126)*126/48000 ≈ 85 ms` —
acceptable settle.

### Q3.  Threading

**Decision: option (a) — extend the TX DSP worker thread to
feed Spectrum0 directly from its existing block-handoff site.**

Three options:
- **(a) TX DSP worker feeds Spectrum0 inline** at
  `tx_dsp_worker.cpp:351-358` (right after sip1Ring_ write).
  Producer thread already has the 126-sample complex block in
  `txIqBuf_` (line 339-341); runs at EP2 cadence (~380 Hz).
  One extra call per block; no thread, no queue.
- **(b) Dedicated TX-spectrum-feed thread polling sip1**:
  needs SPSC consumer (own read index — Q4), wakeup, empty-
  handling.
- **(c) Qt main thread on QTimer polling sip1**: cleanest
  separation but adds 2nd consumer + GIL/event-loop-stall
  starvation risk (CLAUDE.md §15.26 hazard class).

**Why (a):**
- `WdspEngine::feedIq` shows the pattern: RX worker is the
  Spectrum0 producer; WDSP serialises feed-vs-read against
  `GetPixels` from GUI (`wdsp_engine.cpp` comment 277-280).
  TX worker doing the same is symmetric.
- Zero new threads, zero new queues, zero new latency.
- Wire-cadence-safe: Spectrum0 is memcpy + tiny ring increment
  on WDSP side; well below EP2 headroom (§15.26 S2+S3 proved).

MOX-edge source-swap discipline (Q5) makes (a) clean: TX worker
Spectrum0 feed runs only `if injectTxIq_` (`tx_dsp_worker.h:350`)
— already gates EP2 emission.  RX worker's Spectrum0
(`wdsp_engine.cpp:2034`) runs only when NOT MOX (new
`txOwnsAnalyzer_` atomic — §2 step 4).

### Q4.  sip1 ring consumer pattern

**Decision: do NOT add a consumer index to `sip1Ring_`.**  Feed
Spectrum0 from TX worker BEFORE/AT the ring write site, using
the same `txIqBuf_` data.

TX worker already has the 126-sample chunk in `txIqBuf_` at the
moment of the ring write (`tx_dsp_worker.cpp:339-359`).
Panadapter consumer doesn't ever READ FROM the ring — gets the
same data via the same producer site.  v0.3 PS calcc stays the
v0.3-scheduled sip1 consumer; its read index is its own
(`tx_dsp_worker.h:355-366` design).

**Keeps sip1 1-producer/0-consumer in v0.2.**  Matches existing
comment (`:353-359`).  Forward-compat for v0.3 unchanged.  No
per-reader-index complexity.  Panadapter feed in exact lockstep
with EP2 wire content; no stale frames, no fell-behind-wrap.

### Q5.  MOX-edge source swap discipline

**Decision:** all on the existing `HL2Stream::moxActiveChanged`
signal (`hl2_stream.h:873`), which fires on TR-settled edges
(post-mox_delay+rf_delay keydown; post-ptt_out_delay keyup,
`main.cpp:368-369`).

Per `main.cpp:376-377`, `moxActiveChanged` already wired to
`WdspEngine::setTxMuted` — one more connect adds analyzer-source
swap to same signal.  AutoConnection: both endpoints are Qt main
thread → DirectConnection executes on main thread before WDSP
worker's next feedIq.

**Keydown (moxActiveChanged(true)):**
1. Main: `txOwnsAnalyzer_ = true` (new atomic).
2. Main under `channelMtx_` (`wdsp_engine.h:580` lifecycle vs
   feedIq mutex): `SetAnalyzer(kAnDisp, ..., bf_sz=126, ...,
   overlap=overlap_tx, ..., max_w=max_w_tx)` reconfigure.
3. RX worker's `feedIq()` (`:2034`) sees `txOwnsAnalyzer_=true`
   and skips `Spectrum0`.  RX keeps processing audio (txMuted
   gates audio output; analyzer skip is separate).
4. TX worker's block-pack site (`tx_dsp_worker.cpp:339-359`)
   sees `wdspEngine->txOwnsAnalyzer()` and calls
   `Spectrum0(1, kAnDisp, 0, 0, txIqAsDouble.data())` after
   ring write.

RX channel is **NOT** stopped — lyra-cpp's deliberate choice
(different from Python's §15.25).  Works because (a) analyzer
source swap is at Spectrum0 feed gate not channel stop; (b)
`setTxMuted` mutes RX audio so operator isn't deafened; (c)
keeping RX channel running avoids the "bristle-broom sweep"
keyup artifact (§15.26 3 reverted iterations).

**Keyup (moxActiveChanged(false)):**
1. Main under `channelMtx_`: re-`SetAnalyzer(kAnDisp, ...,
   bf_sz=cfg_.inSize, ..., overlap=overlap_rx, ...,
   max_w=max_w_rx)` to restore RX config (refactor original
   `wdsp_engine.cpp:494-511` into a helper).
2. Main: `txOwnsAnalyzer_ = false`.
3. RX worker `feedIq()` Spectrum0 call resumes on next block.
4. TX worker Spectrum0 path no-op (`injectTxIq_` already false
   by this point per FSM).

Ordering at keyup: SetAnalyzer reconfigure FIRST (any in-flight
TX feed from worker safe), then flip flag, then RX resumes.
`channelMtx_` serializes lifecycle vs feed (`:577-580`).

### Q6.  Frequency axis on TX

**Decision: report TX span (48 kHz) via existing
`WdspEngine::spanHz` property** (`wdsp_engine.h:266-269`).  QML
auto-tracks.

- `spanHz` today: `cfg_.inRate / zoom` (`:266-269`).  Don't
  mutate `cfg_.inRate` (rest of engine — resampler ratios,
  out_size, captured-profile — depends on it).
- Add `txSpanHz_` atomic; `spanHz` getter returns
  `txOwnsAnalyzer_ ? txSpanHz_/max(1,zoom) : cfg_.inRate/zoom`.
  Emit `spanChanged()` on MOX edge both directions.
- Non-SPLIT (today): TX freq == RX VFO freq, centre stays put.
  Freq labels redraw at 48 kHz span but stay centred — matches
  reference (Thetis audit §2.a).
- Zoom 1×–8× on 48 kHz: 48/24/12/6 kHz views — ESSB to narrow
  CW range.  Operator's last RX zoom retained (Thetis audit
  §2.b).

### Q7.  Mode-specific (CW)

**Decision: skip for Phase 2.  Comment-anchored hook for
v0.2.2 CW.**

Reference blanks panadapter to floor on CW (Display.cs:5028-5032,
Thetis audit §5.a).  lyra-cpp CW lands v0.2.2 (LYRA audit
:225-226).  For now all SSB/AM/FM/DSB/DIGU/DIGL show natural
TX spectrum.  CW TUN (only CW-like in v0.2.x) produces narrow
tone — fine, not blank.  v0.2.2 adds one-line guard in TX
worker Spectrum0 feed: `if (mode != CWU && mode != CWL)
Spectrum0(...);`.

### Q8.  Waterfall

**Decision: defer — lyra-cpp doesn't ship a waterfall in v0.2.x.**
(`panadapter.h` has zero waterfall surface.)  When waterfall
lands: match reference — feed from same analyzer; naturally
swaps to TX during MOX since it shares `kAnDisp`.

### Q9.  PureSignal forward-compat

**Confirmed: nothing to do.**

sip1 is BEFORE iqc in TXA chain (TXA.c:586-587, comment
`tx_dsp_worker.h:357-358`).  Feeding panadapter from EP2-output
buffer (`txIqBuf_` = post-WDSP-TXA output) means panadapter
shows post-ALC pre-iqc TX I/Q.  When v0.3 PS lands and iqc kicks
in between sip1 and EP2 wire, panadapter still shows
pre-correction TX signal — matches reference.  Forward-compat
preserved with ZERO Phase 2 code.

### Q10.  Failure modes

| Failure | Phase 2 panadapter behavior |
|---|---|
| TX worker stalls (>100 ms no blocks) | `specCache_` retain-last-good (`:818-823`) — trace freezes at last TX frame, no crash. |
| sip1 underrun | N/A — panadapter doesn't consume sip1 (Q4). |
| SetAnalyzer reconfigure races in-flight RX feedIq | `channelMtx_` (`:577-580`) serializes.  MOX-edge slot takes it.  Worst: one block dropped.  Cosmetic. |
| RX channel restart race on keyup | N/A — lyra-cpp doesn't stop RX channel.  Keyup reconfigure-then-flip-flag sequence under `channelMtx_` is race-free. |
| `injectTxIq_=false` but `txOwnsAnalyzer_=true` (FSM mid-keyup transient) | TX worker `tryConsumeTxIq` is no-op when `injectTxIq_=false`; no TX block produced → no Spectrum0 call → analyzer holds last cached frame.  Self-heals on next moxActiveChanged(false). |
| MOX-true fires twice | SetAnalyzer idempotent; flag already true; harmless. |
| MOX-false before MOX-true | RX config already current; flag already false; harmless. |

---

## §2 Implementation steps (smallest first, individually revertible)

Each step is ONE commit, leaves wire-bytes byte-identical (only
behaviour change is panadapter source during MOX).  Steps 1-3
ship wire-inert + analyzer-inert; steps 4-6 wire up.

### Step 1 — Helpers: factor SetAnalyzer config

**Files:** `src/wdsp_engine.cpp` (new private helpers),
`src/wdsp_engine.h` (declarations).

Extract SetAnalyzer call body from `wdsp_engine.cpp:486-511`
(overlap+max_w+flp+SetAnalyzer+SetDisplayDetectorMode+
SetDisplayAverageMode block) into private
`WdspEngine::configureAnalyzerForRx()`.  Call from `openRx1()`.

Add `WdspEngine::configureAnalyzerForTx()` — same shape, but
`rate = 48000` and `bf_sz = 126`.  Compute `overlap_tx =
max(0, kAnFftSize - 48000/kAnFrameRate)` and `max_w_tx =
kAnFftSize + min(48000/10, kAnFftSize*kAnFrameRate/10)`.  Same
detector + average mode.

Both take `channelMtx_`.  Idempotent against `analyzerOpen_=
false` (no-op).

**Verify:** `openRx1()` path unchanged behavior; RX smoke clean;
suite green.

**Revertible:** pure refactor + dormant helper.

### Step 2 — `txOwnsAnalyzer_` atomic + `txSpanHz_`, wire to spanHz

**Files:** `src/wdsp_engine.h` (members + getter),
`src/wdsp_engine.cpp` (init + spanHz override).

```cpp
std::atomic<bool> txOwnsAnalyzer_{false};
std::atomic<int>  txSpanHz_{48000};
bool txOwnsAnalyzer() const { return txOwnsAnalyzer_.load(); }
```

`spanHz()` becomes:
```cpp
int spanHz() const {
    if (txOwnsAnalyzer_.load(std::memory_order_relaxed))
        return txSpanHz_.load(std::memory_order_relaxed)
               / static_cast<int>(std::max(1.0, zoom_.load()));
    const double z = zoom_.load(std::memory_order_relaxed);
    return static_cast<int>(cfg_.inRate / (z > 1.0 ? z : 1.0));
}
```

Emit `spanChanged()` on MOX-edge slot (added step 5).
Wire-inert; QML auto-tracks.

**Revertible:** dormant flag + getter override; identical to
today when `txOwnsAnalyzer_=false`.

### Step 3 — Wire WdspEngine pointer into TxDspWorker

**Files:** `src/tx_dsp_worker.h` (ctor + member),
`src/tx_dsp_worker.cpp` (store), `src/main.cpp` (pass at TX
worker construction).

`lyra::dsp::WdspEngine *wdspEngine_ = nullptr;` member.  Take
via ctor param or setter (match existing dependency-injection
idiom).  In `main.cpp` at TxDspWorker construction site (find
near existing `wdspEngine` + `stream` wiring `~main.cpp:240-260`),
pass WdspEngine pointer.

TxDspWorker calls nothing on it yet; wire-inert.

**Revertible:** new member, no behaviour.

### Step 4 — TX worker feeds Spectrum0

**File:** `src/tx_dsp_worker.cpp` (inside existing `txIqBuf_`
block-pack site near lines 345-359).

After sip1Ring_ write at `:351-358`:
```cpp
// Phase 2: feed panadapter analyzer with same 126-sample
// post-WDSP-TXA pre-iqc block.  Only when analyzer is
// TX-owns mode (set on moxActiveChanged(true) edge by main
// thread).  Forward-compat: stays pre-iqc by virtue of
// feeding from txIqBuf_, same buffer that tees into sip1
// (= pre-iqc).
if (wdspEngine_ && wdspEngine_->txOwnsAnalyzer()) {
    // Spectrum0 takes interleaved DOUBLE IQ; txIqBuf_ is
    // complex<float>.  Convert into 252-double stack buffer.
    double iqd[2 * kEp2BlockSize];
    for (int i = 0; i < kEp2BlockSize; ++i) {
        iqd[2*i + 0] = static_cast<double>(txIqBuf_[i].real());
        iqd[2*i + 1] = static_cast<double>(txIqBuf_[i].imag());
    }
    wdspEngine_->feedTxSpectrum(iqd, kEp2BlockSize);
}
```

Add `WdspEngine::feedTxSpectrum(const double *iq, int n)`
public: under `channelMtx_`, check `analyzerOpen_`, call
`api.Spectrum0(1, kAnDisp, 0, 0, const_cast<double*>(iq))`.
No-op when closed or `n != 126`.

Still wire-inert: `txOwnsAnalyzer_` never set true yet
(step 5).

**Revertible:** gated on dormant atomic.

### Step 5 — MOX-edge wiring in `main.cpp`

**File:** `src/main.cpp` (near existing `moxActiveChanged →
setTxMuted` connect at `:376-377`).

```cpp
QObject::connect(stream, &lyra::ipc::HL2Stream::moxActiveChanged,
                 wdspEngine, [wdspEngine](bool on) {
    wdspEngine->setTxOwnsAnalyzer(on);
});
```

Add `WdspEngine::setTxOwnsAnalyzer(bool on)`: under `channelMtx_`,
if on: `configureAnalyzerForTx()` then `txOwnsAnalyzer_=true`.
Else: `configureAnalyzerForRx()` then `txOwnsAnalyzer_=false`.
Emit `spanChanged()` either way.

**Order at keydown:** SetAnalyzer first (sized for 126-feed),
THEN flip flag (TX worker's next block lands correctly-sized).
At keyup reverse: SetAnalyzer back to RX, THEN flip flag.
TX feed mid-reconfigure no-op because flag flips after.

**THIS IS THE PHASE 2 GO-LIVE COMMIT.**

### Step 6 — Optional polish: panadapter ringing on swap edges

Empirically test for visible glitch (sub-frame stale 192k at
48k scale).  If visible:
- Try `Spectrum0(0, kAnDisp, 0, 0, …)` (run=0) one-shot flush.
- If still ugly: 1-frame `specCache_` blank on each edge
  (`specCache_.assign(kAnPixels, -200.0f)` under lock — like
  reference PurgeBuffers, Thetis audit §4.b).

Likely not needed; SetAnalyzer reconfigure is fast, next feed
lands within ~3 ms.

---

## §3 Risks + mitigations

| Risk | Mitigation |
|---|---|
| Per-block Spectrum0 adds GIL-class jitter | Benchmark on bring-up.  Spectrum0 = memcpy + small ring inc; falls below §15.26 S2+S3 wire-cadence headroom.  If it shows: move to lock-free queued post-block hook on dedicated TX-spectrum thread (Q3 option b). |
| SetAnalyzer reconfigure races in-flight RX feedIq | `channelMtx_` (`:577-580`) serializes.  MOX-edge slot takes it. |
| WDSP doesn't tolerate live SetAnalyzer reconfigure | Test bring-up.  Fall back: two analyzer instances (Q1 option B); create `kTxDisp=1` in `openRx1()` configured 48k/126 from start; `copySpectrum` reads per `txOwnsAnalyzer_`.  Larger but fully spec'd as backup. |
| 192k RX feed continues into 48k-sized analyzer in flag-vs-reconfig window | Order enforced under mutex: SetAnalyzer FIRST, then flag.  RX worker reads flag inside `feedIq` — by the time it sees flag flipped, analyzer is correctly configured.  `memory_order_relaxed` sufficient because mutex provides synchronization. |
| MOX edge fires while `copySpectrum` mid-call (GUI thread) | WDSP serialises Spectrum0 vs GetPixels internally (`:277-280`).  SetAnalyzer is new entrant — protected by `channelMtx_` (also serialises lifecycle vs feed).  GUI's copySpectrum doesn't take channelMtx_ (hot path); accept one torn frame at reconfigure.  Cosmetic. |
| Slot ordering: setTxMuted vs analyzer swap | Both main thread + DirectConnection; execute in connection order: setTxMuted first (main.cpp:376), THEN new analyzer swap (step 5).  Audio mute leads analyzer swap by microseconds.  Operator-imperceptible. |
| `cfg_.inRate` changes mid-MOX (operator rate-change) | Keyup `configureAnalyzerForRx()` reads `cfg_.inRate` at that moment — picks up new rate.  `setSampleRate` already takes `channelMtx_` (`:577-580`).  On rate-change mid-MOX: setSampleRate reopens channel; new openRx1 calls `configureAnalyzerForRx()` but `txOwnsAnalyzer_=true` so RX feed skipped.  On keyup, configureAnalyzerForRx() reapplies new rate.  Coherent. |
| sip1Ring_ producer site = canonical TX-spectrum feed; v0.3 PS later wants its own analyzer feed | Out of scope.  v0.3 designs own consumer; sip1 ring already there (`tx_dsp_worker.h:352-366`).  Phase 2 feed at producer site doesn't consume ring; coexist. |

---

## §4 Bench gate (operator hardware)

Dummy-load-only.  No antenna.

**After step 1 (helper refactor):**
- Cold launch; RX1 audio + panadapter clean per v0.2.x baseline.
  No `lyra_wire.log` cadence regression.

**After step 2 (atomic + getter):**
- Same as step 1.  `spanHz` returns same value as today on RX
  (192000/zoom).

**After step 3 (TxDspWorker wiring):**
- Cold launch + keydown TUN dummy + Palstar ~5 W (§15.26 first-RF
  bench must match).  No regression TX cadence or Spectrum0 logs.

**After step 4 (TX feeds Spectrum0 — flag still false):**
- Same as step 3 (new branch dead code, gated on dormant flag).

**After step 5 (MOX-edge swap goes live — THE Phase 2 commit):**
1. RX-only baseline: RX1 USB 14.200 MHz, operator dB range
   (e.g. -130/-20 dBm), live RX spectrum.
2. Keydown TUN dummy: panadapter NOW shows steady tune-carrier
   line, NOT previous "wavey RX-blinded" trace.  Within ~85 ms
   of moxActiveChanged(true).
3. Audio: still muted by `setTxMuted` (existing).
4. Phase 1 (y-axis swap + red badge): work independently,
   listen to same signal.
5. Keyup: panadapter returns to live RX within ~85 ms of
   moxActiveChanged(false).  No "bristle-broom sweep" (lyra-cpp
   doesn't stop RX channel — §15.26 pathology doesn't apply).
6. Re-key during draining keyup (sub-50 ms): SetAnalyzer for TX
   fires; analyzer already in TX mode (idempotent); flag stays
   true.  Clean.
7. Keydown SSB+mic dummy: panadapter shows audio-bandwidth-shaped
   TX spectrum (~±3 kHz).  Operator sees own TX passband.
8. Operator drags zoom mid-MOX: zoom factor applies to 48 kHz
   TX span (zoom 4× = 12 kHz centred slice).
9. Stop/Start stream 5×: no hang, no dead RX (cb58bcb invariant
   holds — we don't touch RX channel lifecycle).
10. `LYRA_WIRE_DEBUG=1`: EP2 cadence + MAINSTALL distribution
    unchanged from pre-Phase-2 baseline.  Per-block Spectrum0
    must not bloat worker thread cost.
11. Phase-3-EXIT kill-test (§15.20/§15.24-C) STILL HOLDS —
    Phase 2 doesn't touch PA safety.  PA-current drops on
    `taskkill /F` mid-TUN.

**Fail-state recovery:** step 5 = single-file/single-connect
revert.  Steps 1-4 wire-inert, stay.

---

## §5 Forward-compat

### v0.3 PureSignal

- **Tap point already correct.**  Phase 2 feeds panadapter from
  `txIqBuf_` (same site as sip1 tee — both pre-iqc, TXA stage
  27, `tx_dsp_worker.h:357`).  When v0.3 iqc kicks in between
  sip1 and EP2 wire, panadapter trace continues pre-correction.
  Reference posture preserved (Thetis audit's TX panadapter
  also pre-iqc §1.c).
- **sip1 stays SPSC 1-producer/0-consumer in v0.2.**  v0.3
  calcc plugs in as designed consumer; Phase 2 doesn't add
  competing reader.  Per-reader-index trap doesn't arise.
- **frame-4/frame-11 ATT-on-TX + PS auto-attenuator** (§15.26):
  unaffected; Phase 2 doesn't touch protocol layer.
- **MOX+PS DDC routing** (§3.8/§6.7/§14.6 CR-1): on HL2 MOX+PS
  reroutes DDC0/DDC1 to TX freq via cntrl1=4.  Today RX worker
  feeds analyzer with whatever DDC0 delivers.  After Phase 2,
  during MOX TX worker feeds it (DDC0's reroute invisible to
  panadapter — operator gets TX-domain spectrum, not PA-
  feedback samples).  Matches reference.

### Phase 3 SPLIT

- `dual_rx/README.md:127-129` "per-side selectivity in SPLIT-on"
  is Phase 3 scope, NOT here.
- When Phase 3 lands with two panadapter panes (one per RX),
  source-swap logic generalises: TX-target pane subscribes to
  `moxActiveChanged → setTxOwnsAnalyzer` exactly like today's
  single pane; listening-only pane stays on its RX analyzer.
  Each pane has its own analyzer id (lyra-cpp engine extends
  to multiple `kAnDisp` ids per RX channel — RX2 work
  trigger).  Phase 2 wiring is the per-pane template; doubling
  needs no architectural change.
- TX freq axis "follows RX1 VFO even in SPLIT" (Thetis audit
  §2.a) satisfied by feeding same `kAnDisp` from TX I/Q —
  centre stays at operator's tuned VFO; TX spectrum is I/Q
  baseband around centre, lands on TX freq regardless of
  SPLIT offset (when SPLIT lands, pane swap moves analyzer
  feed; freq axis convention unchanged).

---

## Critical files for implementation

- `Y:/Claude local/SDRProject/lyra-cpp/src/wdsp_engine.cpp`
- `Y:/Claude local/SDRProject/lyra-cpp/src/wdsp_engine.h`
- `Y:/Claude local/SDRProject/lyra-cpp/src/tx_dsp_worker.cpp`
- `Y:/Claude local/SDRProject/lyra-cpp/src/tx_dsp_worker.h`
- `Y:/Claude local/SDRProject/lyra-cpp/src/main.cpp`

---

**Plan-agent output preserved verbatim above (light reformatting
for readability).  Awaiting 2-agent red-team review next.**
