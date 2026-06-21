# CTUNE (Click-Tune / center-tune lock) — design / scoping doc

**Status:** IN PROGRESS — RX1-only.
- **Step 1 DONE (2026-06-21, local/uncommitted):** WDSP RX-shift binding wired,
  INERT. `SetRXAShiftFreq`(#282)/`SetRXAShiftRun`(#281) dumpbin-verified in the
  bundled `wdsp.dll`; added to `WdspNative` (typedefs + `WdspApi` members +
  `resolveSymbols`) — the RXA home, NOT the ChannelMaster `wdspcalls` seam (the
  earlier note assumed PHROT's TXA seam; RX setters live in `WdspNative` beside
  `SetRXAMode`). `WdspEngine::setRxShiftHz(double)` added, **no caller** (openRx1
  untouched → WDSP shift stays default-off → zero behavior change). Builds +
  links clean. Fully revertable.
- **Step 2 DONE (2026-06-21, operator-bench-confirmed):** the decomposition is
  live in `HL2Stream::pushEffectiveRxFreq` (CTUNE on → DDC locked at
  `ctuneCenterHz_`, WDSP shift = dial(+RIT) − center; CTUNE off → byte-identical
  to the old single-NCO path, no shift emitted). `rxShiftHzChanged` →
  `WdspEngine::setRxShiftHz` wired in `main.cpp`. Engage via `setCtuneEnabled`
  (snaps center to current dial). Panadapter `centerHz` locks to the center under
  CTUNE (`effCenterHz`), so the dial marker slides within the frozen band using
  the existing off-center-marker machinery. **Shift SIGN verified correct on the
  bench (`kCtuneShiftSign = +1`)** — signal tunes the right direction under CTUNE.
  Temporary engage = a clickable **CTUN** button bottom-left on the panadapter
  (a QML `Shortcut` does NOT fire in this QQuickWidget-hosted panel — clicks do).
- **Step 3 IN PROGRESS:** tune-past-edge auto-re-center DONE (PanadapterPanel
  `onRx1FreqChanged`: dial beyond ±`WdspEngine.spanHz`/2 of the locked center
  re-locks center to the dial via `setCtuneCenterHz` — Thetis behavior).
  **CTUN button HOME = the header `RX DSP:` chip strip, right after `RX EQ`**
  (mainwindow.cpp) — checkable QToolButton, same boxed chip style, GREEN "on"
  accent, two-way synced to `ctuneEnabled`/`ctuneChanged`; the panadapter bench
  button was removed. Drag-pan operator-bench-OK as-is (tune-past-edge re-center
  handles it — no dedicated drag-moves-center needed for now). **Feature-complete
  for field test (2026-06-21); sent to the requesting users to judge.** REMAINING
  (deferred, field-feedback-gated): rate/zoom span-shrink + big band/memory/TCI-
  jump edge cases, optional persistence. Still LOCAL/UNCOMMITTED.

**Why:** a couple of operators asked for it. CTUNE lets you **lock the
panadapter/DDC center** and tune the VFO to an offset *within the captured IQ
bandwidth* — so you can watch a fixed slice of spectrum (a pileup, a net, an
FT8 window) and click signals across it **without the waterfall scrolling /
re-centering** on every tune.

---

## 1. What CTUNE is (confirmed against the Thetis reference)

A **locked-LO + DSP-NCO-shift** model. When CTUN is ON in Thetis
(`Console/console.cs`, verified 2026-06-21):

- The **hardware DDC center stays locked** — `CentreFrequency` → `RX1DDSFreq`
  (`console.cs:32636/32658`). The radio's capture LO does not move.
- The VFO tunes via a **WDSP demodulator oscillator offset**:
  `rx1_osc = −(VFOAFreq − CentreFrequency) * 1e6`
  (`console.cs:32135`) → `radio.GetDSPRX(0,0).RXOsc = rx1_osc`
  (`console.cs:32228`); forced to `0.0` when CTUN is off (`:32239`).
- **RIT stacks on top** of the CTUNE offset: `rx1_osc -= RIT` (`:32183-32184`).
- **Per-RX**: `_click_tune_display` (RX1) / `_click_tune_rx2_display` (RX2),
  independent, each with its own checkbox + offset calc (`:10894-10907`).
- **Toggle** `chkFWCATU` (`:44026`); **persisted** to the DB
  (`click_tune_display/…`, `:2873-2876`) and in the **band stack**: band
  change forces CTUN off then re-applies the stored state + restores the locked
  center (`SetBand`, `:5922-5938`).
- **Tune past the displayed edge** → auto re-center: `CentreFrequency = freq;
  rx1_osc = 0.0` (`:32148-32178`); large jumps (`FreqJumpThresh` 0.5 MHz, e.g.
  memory recall) always re-center.
- **Mouse**: CTUN off = click sets VFO + display pans to follow; CTUN on =
  click sets the VFO offset, display stays locked; drag on the center line pans
  the locked center, drag below tunes the offset.

---

## 2. The load-bearing finding for lyra-cpp

Lyra and Thetis tune differently at the bottom of the stack:

| | Thetis | lyra-cpp (today) |
|---|---|---|
| Normal tune | moves hardware DDC | moves hardware DDC (`HL2Stream::setRx1FreqHz`) |
| **RIT** | WDSP `RXOsc` offset | **moves the hardware DDC** — `pushEffectiveRxFreq()` writes DDC NCO = `rx1FreqHz_ + (ritEnabled ? ritOffsetHz : 0)` (`hl2_stream.h:1379-1382`) |
| WDSP RX baseband shift | yes (`RXOsc`) | **NOT wired** — no `SetRXAShiftFreq` / `RXOsc` / shift binding in `WdspEngine` |

So the one mechanism CTUNE fundamentally requires — a **WDSP RX NCO shift** that
lets the demodulator sit off the DDC center — **does not exist in Lyra yet**.
That binding (+ its sign verification) is the real cost driver. Everything else
is plumbing we already have.

---

## 3. The design that fits Lyra (low-surface)

**Keep `rx1FreqHz` as the single source of truth** for the dial — VFO LED,
TCI, band memory, logging all keep reading it unchanged. CTUNE only changes how
that dial is *decomposed* at the wire/DSP layer, all inside the existing single
RX-NCO chokepoint `pushEffectiveRxFreq()`:

- **CTUNE off** (current behaviour): DDC NCO = `rx1FreqHz(+RIT)`; WDSP shift = 0.
- **CTUNE on**: DDC NCO = locked `ctuneCenterHz_`; WDSP shift =
  `rx1FreqHz(+RIT) − ctuneCenterHz_`; panadapter `centerHz` = the locked center.
- **Tune past ±span/2** from the locked center → re-lock center to the dial,
  shift → 0 (Thetis's re-center).

Net: `setRx1FreqHz`, the LED, TCI, click/wheel/drag handlers, and band memory
are all unchanged — they keep writing the dial; only the decomposition differs.
RIT folds into the shift exactly as Thetis stacks it. XIT/TX are untouched (the
TX NCO is a separate writer).

---

## 4. What we already have vs what's genuinely new

**Already have (cheap / reuse):**
- Single RX-NCO writer chokepoint `pushEffectiveRxFreq()` (no-patching-friendly).
- Off-center marker drawing — RIT/XIT/VFO-B already render at `center + offset`
  in `PanadapterPanel.qml`.
- `freqAtX` / `xToFreq` / drag-pan (`startCenter`/`newCenter`) / wheel-tune math.
- DDS-vs-carrier offset bookkeeping precedent from the CW arc
  (`txDdsHzForTune`, `cwMarkerOffsetForMode`).
- Prefs-toggle pattern (Q_PROPERTY + QSettings key + QML binding) — e.g. the
  `bandPlanCountry` toggle added in v0.4.3.
- The WDSP X-macro call seam (`wire/wdspcalls`) — how `SetTXAPHROTRun` was added.

**Genuinely new (the work):**
1. **WDSP `SetRXAShiftFreq` + `SetRXAShiftRun` binding** into the call seam +
   `WdspEngine::setRxShiftHz()`. **Verify the shift sign** against WDSP/HL2's
   mirrored baseband (the §14.2-class sideband gotcha — bench, don't guess).
2. **Decomposition + lock logic** in `pushEffectiveRxFreq()`
   (`ctuneEnabled_` / `ctuneCenterHz_` atomics).
3. **QML**: `centerHz` reads the locked center under CTUNE; a **CTUNE button**
   (engage = snap current dial → locked center); tune-past-span re-center;
   drag-pan moves the locked center.
4. **Edge cases**: rate/zoom change that shrinks the span below the current
   offset; band / GEN / memory / TCI jumps larger than the span (re-center);
   persistence (toggle persists; center resets per band/session).
5. **RX1-only** for the first cut — extend to RX2 when #96+ lands (same as RIT
   shipped RX1-only).

---

## 5. Files touched (estimate)

- `wire/wdspcalls` X-macro seam — add `SetRXAShiftFreq` / `SetRXAShiftRun`.
- `wdsp_engine.{h,cpp}` (`WdspEngine`) — `setRxShiftHz(double)` + run gate.
- `hl2_stream.{h,cpp}` — `ctuneEnabled_` / `ctuneCenterHz_` atomics;
  decomposition + re-center inside `pushEffectiveRxFreq()`; call the WDSP shift.
- `prefs.{h,cpp}` — `ctuneEnabled` Q_PROPERTY + QSettings key.
- `src/qml/PanadapterPanel.qml` — locked-center `centerHz`, CTUNE button,
  engage-snap, tune-past-span re-center, drag-pan → center.
- (later) RX2 extension when the RX2 arc ships.

---

## 6. Risks / constraints

- Touches the **RX freq path** — but RX-only, not the TX safety FSM, so
  lower-risk than the TX arc.
- **HL2 captured-bandwidth limit**: CTUNE only tunes *within the sample rate* —
  ±24 kHz at 48k, ±96 kHz at 192k, ±192 kHz at 384k. Most useful at higher
  rates; set expectations in the UI/USER_GUIDE. This is the main value caveat
  for the go/no-go decision.
- **WDSP shift sign** + **zoom-vs-center interplay** are the two
  "verify on the bench" items.

---

## 7. Effort

Comparable to the shipped **RIT/XIT** feature — a real mode, not a one-liner,
but it lands on plumbing we already have. Realistically ~2–3 focused sessions:
(1) WDSP-shift binding + decomposition, (2) QML center-lock + button + gesture
rerouting, (3) edge cases + persistence + HL2 bench. The verify-first reference
read is already done (this doc).

---

## 8. Open decision

Green-light as a feature vs park: a couple of operators asked, it's genuinely
nice for working a fixed slice without the waterfall scrolling, and the design
keeps `rx1FreqHz` as the source of truth so it doesn't disturb LED/TCI/memory.
The honest gating question is **value vs the HL2 captured-bandwidth limit**
(§6) — usefulness scales with sample rate.

*Reference read 2026-06-21 (Thetis console.cs CTUN + lyra-cpp RX freq path).
Verify-not-guess on the WDSP shift sign before any FSM/freq-path code.*
