# Dual-RX / SPLIT UX — locked design reference

Operator + working group (Rick N8SDR, Brent, Timmy) decided 2026-05-11
on the dual-RX UI direction for Lyra.  This decision is upstream of any
RX2 / SPLIT / TX-indicator work in lyra-cpp — when that work eventually
lands (it is NOT in the current TX-focused v0.2.x roadmap, but is
inevitable as the project arc reaches multi-RX), this is the design
spec.

## Locked decision: Option A (Hybrid focused-RX) + EESDR active-VFO enhancements

Not a Thetis clone, not pure EESDR — deliberately different, optimised
for the HL2/HL2+ reality that "RX2" structurally maps to EESDR's
SubRX (shared ADC + shared BPF passband) rather than to EESDR's true
RX2 (independent main receiver).

### Layout (single focused-RX control panel)

```
RX1: 14.205.000 MHz [TX gray] ← focused (orange border)   RX2: 14.210.000 MHz [▲ TX RED]

MODE + FILTER    Mode: USB  RX BW: 2.7k    [ SUB ] [ SPLIT ✓ ] [ A→B ] [ B→A ] [ SWAP ]

DSP + AUDIO      AGC NR NF LMS SQ APF Vol Bal AF Mute    (operates on focused RX)
```

### Always-visible elements
* Both VFO LEDs (never lose track of either freq).
* Two TX indicators, one per VFO LED.  **RED = will TX here, gray =
  inactive.**  SPLIT toggles which is red.  Click gray to manually
  swap.
* Orange focus border on whichever VFO the wheel / keyboard currently
  controls.

### Single DSP+AUDIO panel (focused RX only)
* NR / NF / LMS / SQ / APF / AGC / Mode / Filter all follow focus.
* SUB / SPLIT / A→B / B→A / SWAP live in the MODE+FILTER strip.

### Per-RX controls visible when SUB is on
* **Vol-A / Vol-B** — two independent volume sliders (the one control
  that genuinely benefits from per-RX independence: DX whisper on one
  ear, hot SWL on the other).
* **Mute-A / Mute-B** — per-RX mute (silence RX2 between pile-up
  calls without tearing down the dual-RX setup).
* **Balance stays single** — it's an overall L/R lean on the combined
  output, not per-RX.  (Per-RX pan defaults to hard-L/hard-R via WDSP
  `SetRXAPanelPan` and is NOT operator-exposed in Phase 3.)
* **AF Gain stays single** — pre-AGC system reference.

### EESDR-borrowed tuning patterns
* **Click a VFO LED** → focus moves there.  Wheel/keyboard now tune
  that VFO.
* **Middle-click on the panadapter** → swaps active VFO + brief
  `TUNE A` / `TUNE B` tooltip near the cursor for unambiguous
  feedback.  Verified unbound on Lyra's panadapter today — safe to
  claim.
* **Ctrl+1 / Ctrl+2** hotkeys for keyboard-only users.
* **Right-click on the SPLIT button** → per-mode shift-offset menu
  (e.g. set "up 5 kHz" default for USB; offset saved per mode — AM /
  LSB / USB / CW).  On the BUTTON, NOT the panadapter — panadapter's
  right-click is reserved for notch / dB-scale menus.

### Safety property (the load-bearing rule)
In non-SPLIT mode, the RED TX indicator stays glued to VFO A.  An
operator cannot accidentally transmit on RX2's freq because the
visible indicator tells them which freq TX will go to.  SPLIT-on
moves the RED to VFO B explicitly; the operator sees it move and
keys with intent.

### Architecture note — what "RX2" means on HL2/HL2+

EESDR architecture: 2 software RXs + SubRX for each → 4 slices total.
EESDR's RX1 / RX2 are *independent main receivers* that can tune to
different bands.  EESDR's SubRX is a second filter slice on the *same
band* as its parent — shared ADC + shared BPF.

**HL2/HL2+ reality:** what Lyra calls "RX2" maps to EESDR's
**SubRX**, not their RX2.  Both DDCs share one ADC and the one active
band-filter passband.  v0.4 ANAN-class multi-radio work will
introduce true independent RX1 / RX2 on different bands.

This shapes Phase 3: a fully-duplicated panel (Thetis-style) would
waste space showing RX2 settings that *typically mirror* RX1's
because they're constrained by the shared front end.  Option A keeps
the panel tight; ANAN operators with true per-RX flexibility benefit
from Option A's clarity without the duplicate-panel cost.

## Walkthrough 1 — RX2 as listening monitor (the common case)
1. Click SUB.  RX2 panel + LED light up at persisted last freq.
2. Click VFO B's LED → focus shifts to RX2 (orange border moves).
3. Type / wheel RX2 to target freq.
4. (Optional) change RX2's mode — operates on focused RX.
5. RX1 in left ear, RX2 in right ear (Phase 2 stereo split).
6. Ctrl+1 (or click VFO A's LED) to refocus RX1.  RED TX indicator
   stayed on VFO A the whole time.

## Walkthrough 2 — SPLIT pile-up (the heavy case)
1. Tune RX1 to DX freq (click-tune on panadapter, or LED edit).
2. Click SUB → RX2 enables.
3. Middle-click on the panadapter at the pile-up freq → VFO B
   follows the click; `TUNE B` tooltip flashes near cursor.
4. Click SPLIT (or right-click for "up 5" preset).  VFO B's TX
   indicator turns RED, VFO A's goes gray.
5. Balance ears with Vol-A / Vol-B (pile-up usually louder than DX).
6. Hunt clean call spot via repeated middle-clicks.  Time call → PTT
   → TX goes to VFO B (the call freq), NOT VFO A (the DX freq).
7. DX comes back on VFO A (RX1 / left ear); exchange reports.
8. Click SPLIT to disengage (TX returns to VFO A) or click SUB to
   turn off RX2 entirely.

## Phase 3 scope (when this lands)
* Not in lyra-cpp's current v0.2.x TX-focused roadmap.
* Will land in a future "Phase 3 / RX2 enable" cycle — current
  lyra-cpp engine has RX1 + TX; RX2 wire dispatch + WDSP RXA-second-
  channel + stereo audio combiner are the prerequisites.
* When implementation kicks off: read this README + the locked PDF
  (`Lyra-v0.1-Phase3-UI-Candidates.pdf`) first.  Both are
  operator-locked.

## Crossover with TX-state visual treatment

The RED / gray TX indicator design here is the SAME visual language
that drives the panadapter TX-state behavior (Task #44 area):

* TX-state badge on the panadapter pane (top-right corner) =
  RED accent same as the VFO-LED TX indicator.
* Per-side selectivity: in SPLIT-on, the panadapter pane that's
  showing the TX-target VFO gets the badge; the listening-only pane
  doesn't.
* Operator-set TX `SpectrumGridMin/Max` swaps in on MOX edge
  (Thetis-faithful), independently of which VFO is the TX-target.

When Task #44 lands (the operator is currently looking for the
confirmation PDF that fixes its scope), this badge styling reuses
the indicator visuals locked here.

## Provenance

`Lyra-v0.1-Phase3-UI-Candidates.pdf` was generated 2026-05-11 against
Lyra v0.1 Phase 2 commit `cacff0f` (the old Python lyra tree).  PDF
contents (Options A / B / C analysis, EESDR pattern review, SPLIT
workflow walkthrough, the working-group lean) are operator-locked at
that date.  Per the project's no-attribution rule, EESDR / Thetis /
PowerSDR / SunSDR names appear only in this docs/refs/ tree, never in
shipped code, comments, commits, or operator-visible UI.
