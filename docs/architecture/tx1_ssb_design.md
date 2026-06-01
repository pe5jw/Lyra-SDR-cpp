# TX-1 SSB modulator arc — design document

Status: **DESIGN v2.1.1 + 2026-05-30 reference-reconciliation amendments
LOCKED.  Components 1-7 shipped + operator-HL2-bench-confirmed (C7 =
first end-to-end SSB voice TX, wire path clean, 0.2 W bench result
root-caused to ALC `max_gain=0 dB` default trap + missing operator
mic gain UI).  Pre-Component-8 Thetis TX audio-path study landed
2026-05-31 EVE (`docs/architecture/tx_audio_path_reference.md`, 807
lines) — Component 8 ship order locked: 8a-0 (ALC init fix) → 8a
(mic gain UI) → 8a+ (HW mic boost) → 8b (mic source) → 8c (TX BW)
→ 8d (TX multimeter ALC/MIC/COMP values) → 8e (HW PTT).
**2026-05-31 NIGHT EOD:** Components 8a-0 / 8a / 8a-tx-mode / 8c
all SHIPPED + operator-bench-confirmed.  Plus Task #46 (TX worker
error/skip accounting + ring high-water instrument), Task #48
(GetUdpStatisticsEx EP6-drop diagnostic), Task #53 (shared RX+TX
Filter Low edge — Settings → Audio spinbox, default 100 Hz,
operator-tunable 0..500 Hz; replaces the previously-hardcoded
RX SSB-low=0 + TX SSB-low=200 with one shared knob until the TX
Profile Manager #49 lands), and Task #54 (panadapter lo-edge
drag now writes Prefs.filterLow for SSB/DIG modes and reaches
all the way to 0 Hz, was previously pinned at 0).  Post-
Component-8 advanced-chain architecture LOCKED in the section
below (EQ → Combinator → Plate → ALC → BW LPF order; operator-
supplied DSP2024P Plate presets W5UDX + N8SDR captured; pending
operator X-Air Combinator + EQ screenshots before those arcs
start).  TX Profile Manager (#49) is next major arc — schema
will absorb the Filter Low + Filter High + Mode + everything-
else into named profile bundles, per-profile (lo, hi) pairs
override the interim default on load.**
Date: 2026-05-29 (v2 lock) / 2026-05-30 (v2.1 + v2.1.1 + reference-reconciliation) / 2026-05-31 (components 5 + 6 + 7 ship + Thetis TX audio-path study + Component 8 ship order locked).
Scope: SSB-only (USB/LSB).  CW/AM/FM/digital-modulator are later slices.
Reference: **Thetis 2.10.3.13 only** — operator directive 2026-05-29.  Old
Python lyra is NOT a reference for TX-DSP / WDSP-TXA / mic-input; it
remains relevant ONLY for (a) TX panadapter visual rule (red passband
rectangle on TX-active) and (b) 2RX + SUB/SPLIT UI design ideas.

## ⚠ DESIGN PRINCIPLE (operator-mandated, ENFORCE EVERY COMMIT)

**"Do as Thetis does, Lyra-Native style."**

Three rules with no exceptions:

1. **Every WDSP API call must match the reference byte-for-byte.**
   Parameter values, parameter order, the SET of setters called,
   the lifecycle stage at which they fire — all match the
   reference exactly.  If you're considering a value the reference
   doesn't use, STOP and find what the reference uses (typically
   in `cmaster.c` / `console.cs` / `radio.cs` / the `wdsp/*.c`
   stages).  "I have a reason to pick something different" =
   you don't.  Reference has been right every verified time.

2. **"Lyra-Native style" applies to the SURROUNDING architecture
   ONLY.**  Qt + Vulkan + QML + our process model + thread model +
   facade APIs + IPC + UI shell.  This is what `Lyra-native`
   gives us latitude over.  The DSP-engine call surface (WDSP
   TXA / RXA / fexchange0 / OpenChannel / SetTXA* / SetRXA*) is
   NOT Lyra-native — it's reference-bound.

3. **Provenance lives only in code COMMENTS + design docs +
   memory.**  Never in commit messages.  Never in operator-
   visible UI strings.  This is the unchanged no-attribution rule
   from §2 of the project memory.

This principle has been the standing directive since TX work
started.  The 2026-05-30 `fexchange0` crash arc proved its
necessity in hardware: Lyra picked `in_size=126` (per-datagram
mic count) where the reference picks `getbuffsize(48000) = 64`
(constant-latency rule); the resulting non-integer-divisor
ratio against `dsp_insize=1024` corrupted WDSP's internal ring
math on the first `fexchange0` call → access violation inside
a WDSP-internal `memcpy` (1488 bytes / 93 frames vs an
overrun-short source buffer).  Reverting to the reference's
parameters + removing the 17 `SetTXA*` setters that
`create_xmtr` doesn't call removed both the crash hazard and a
class of WDSP-state-machine perturbations the reference doesn't
expose at this lifecycle stage.

---

**v2 amendment trail (2026-05-29 EOD):**
- Three independent red-team lenses (concurrency / safety / scope) ran
  with file:line evidence against Thetis source.
- §4.9 uslew open question DEFINITIVELY ANSWERED via wdsp/uslew.c +
  wdsp/TXA.c read (BLOCKS-SHIP correction folded into §4.9 + §5.6).
- C&C modulus-19 audit confirmed against networkproto1.c:948-1176.
- §3.2 mic byte extraction CORRECTED — the original "bytes 24-25"
  layout is wrong; mic is interleaved with IQ per nddc-aware formula.
- §5.4 MicSource collapsed to a single class (Thetis-faithful).
- §6 open questions all answered + locked.
- Concurrency amendments applied to §5.5/§5.7 (mutex+cv not SPSC;
  producer-paced; teardown order; block-size pinned).
- Safety amendments applied (init order, TR delays, EP2 zero-on-no-MOX,
  panel-gain comment fix, DC/IQ-cal status note).
- v0.3 PureSignal sip1 tap added (§5.8).

**v2.1 amendment trail (2026-05-30 AM):**

Operator (N8SDR) caught two Thetis digital-mode TX behaviors that
were not in v2.  A focused source-verification agent confirmed both
in `console.cs` + `setup.designer.cs`; operator visually confirmed
the Setup checkbox in screenshots.  Both are folded in:
- **§6.3 (Leveler) + §6.4 (PHROT)**: digital-mode auto-OFF behavior
  added per Thetis `dmssTurnOffSettings` (`console.cs:44094-44105`).
- **NEW §5.4.1**: future VAC1 feature spec locks the "Enable for
  Digital modes, Disable for all others" checkbox + the
  PTT/SPACE/MOX-override-VAC options per Thetis
  `chkAudioVACAutoEnable` (`setup.designer.cs:28915-28935`).
- **NEW §6.7 (v0.2.1 anchor)**: every TX-DSP block landing in v0.2.1
  (EQ / Combinator / Tube-plate / formant boost / sibilance / DX
  cut-through / de-esser / auto-AGC / CESSB) gets the same
  auto-OFF-on-digital treatment by default.
- **§9 reworked**: full Settings → TX layout cross-referenced to
  Thetis's actual Setup → Transmit tab (operator-provided
  screenshots locked the visual template).

**v2.1.1 correction trail (2026-05-30 AM):**

Operator (N8SDR) caught that v2.1 §9.5 wrongly listed "TX Meter
dropdown" as future-reserved.  Code-verified: `MeterModel::Source`
enum (`src/metermodel.h:45-56`) ships TODAY on `main` with 9
sources, MOX-edge auto-switch (`src/metermodel.cpp:170, 130, 622`),
and a click-to-cycle UI (`src/qml/MeterPanel.qml:97-119`).  Picker
exists; only the ALC/MIC/COMP source VALUES are pending (blocked
on TX DSP — wired in component 2c via `GetTXAMeter` already cdef'd
in component 1).  v2.1.1 amendments:
- **NEW §9.3.1**: documents the existing MeterModel picker
  architecture + the pending ALC/MIC/COMP value-population work
  for component 2c.
- **§9.5 "Tune" entry corrected**: removes the "TX Meter
  dropdown" line (it's not future work); the entry now covers
  only the Use Drive Slider / Use Tune Slider / Use Fixed Drive
  radio + cross-references §9.3.1.

(This is the §15.28 retrospective lesson working as intended:
operator-empirical knowledge of the existing codebase corrects
agent inference from incomplete reads.)

**2026-05-30 reference-reconciliation amendment (PM)** (folded into §5.3
+ §5.5):
- `OpenChannel` parameters corrected to reference values:
  `in_size=64` (was 126), `dsp_size=4096` (was 2048),
  `tdelayup=0.000` (was 0.010), `tslewup=0.010` (was 0.025).
- All 17 `SetTXA*` setters REMOVED from `open()` — the
  reference's `create_xmtr` calls zero such setters at this
  lifecycle stage; TXA chain stays at WDSP create-time defaults
  until operator-settings layer fires.
- `SetChannelState(1,1,0)` REMOVED from `open()` — channel
  parks at state=0; `start()` arms it from the PTT/MOX edge
  handler when keyed.  Mirrors reference `create_xmtr` posture.
- `TxChannel::process()` early-returns on `!running_` guard so
  the worker thread (component 4c) cycles harmlessly until
  `start()` flips the flag.  No `fexchange0` call possible
  until then.

**2026-05-31 — Component 5 ship trail (5a + 5b complete + bench-confirmed):**
- **5a** `5d5a9e1`: MoxEdgeFade engine.  `src/mox_edge_fade.{h,cpp}`
  + 4 TR-sequencing constants promoted to mutable instance members
  + 6 Q_PROPERTY decls + QSettings persistence + fade wired into
  the EP2 TUN-tone packing loop.  Defaults: MOX 15 / RF 50 /
  Space-MOX 13 / PTT-Out 5 / Fade-In 50 / Fade-Out 13 ms (operator's
  bench-validated working-station config; hot-switch-safe for
  typical 1 kW SS HF linears).  Unit test 21/21 PASS.  Operator
  HL2 bench: smooth cos² ramps, drive × fade composition correct,
  rapid re-key mid-fade reversal works, no clicks, no regressions.
- **5b** `6210ac9`: Settings → TX tab UI.  Six QSpinBoxes + Restore-
  Defaults button + ⚠ HOT-SWITCH PROTECTION tooltips on RF Delay
  + Fade-In Duration.  Bidirectional Q_PROPERTY binding.  Operator
  HL2 bench: tab visible (between Hardware and Bands), defaults
  loaded, tooltips show, persistence verified across restart,
  Restore-Defaults works.
- **Reconcile chore** (same day, pre-5a): cherry-picked v2.1
  + v2.1.1 docs commits off origin's stale TX-1-on-WdspEngine
  branch; applied fresh no-attribution sweep (`4cef88d`, 15 src/
  files, 67→0 leaks); force-pushed local over the superseded
  origin TX-1 architecture.  Operator HL2 bench confirmed clean.
- **Session-start discipline locked** (memory file): `git fetch
  origin` is the FIRST action every session; push after every
  component ships, not at EOD; operator verbal cue at session
  start if they solo-pushed from another machine.

**2026-05-31 PM — Component 6 ship trail (wire-inert plumbing, bench-confirmed):**
- **6** `d7b415b`: EP2 SSB I/Q hand-off — reference-faithful
  two-semaphore producer-consumer handshake (txIqDataReady_ +
  txIqConsumed_ + shared 126-sample txIqBuf_), mirroring the
  verified reference's outIQbufp + hsendIQSem + hobbuffsRun[0]
  pattern.  Producer side (TxDspWorker) accumulates 64-sample
  fexchange0 outputs → fills the buffer → releases dataReady →
  blocks on consumed.  Consumer side (HL2Stream EP2 packer)
  non-blocking try_acquire dataReady → if data: pack with cos²
  fade × reference-faithful symmetric quantize → release consumed
  → if no data: mandatory zero-fill (matching the reference's
  `!XmitBit ⇒ zero outIQbufp` posture).  TUN takes priority over
  SSB.  **Lyra deviation (documented, per design rule 2):**
  consumer uses non-blocking try_acquire instead of the
  reference's blocking WaitForMultipleObjects — required because
  Lyra's EP2 writer is on a hard 2.6 ms timer cadence (S2-locked)
  and cannot afford to block; sample-by-sample wire content
  functionally identical to reference for both XmitBit states.
- **sip1 tap ring**: 1 sec @ 48 kHz = 48000 complex samples,
  teed from every EP2 hand-off into a wraparound ring.  No
  consumer in v0.2 — allocated + filled, that's it.  v0.3
  PureSignal calcc reads it for adaptive predistortion
  calibration.  Wiring now lets v0.3 land without re-validating
  every TX sub-mode (per CLAUDE.md §6.7 "don't paint into a
  corner").
- **Wire-inert by construction**: `injectTxIq_` defaults FALSE.
  While false, producer NEVER signals dataReady → consumer
  always falls through to zero-fill → no SSB I/Q on the wire
  even with MOX keyed.  Operator HL2 bench: 17 TUN cycles +
  2 MOX-only cycles + rapid TUN stress (14 cycles in 7 sec) —
  every cycle clean, zero `inject_tx_iq ARMED` log lines,
  zero sip1 fills, mic Q6.5 + RX baseline unchanged from 5b.
- **Unit test**: 21/21 PASS — handshake mechanics, reference-
  faithful quantize endpoints + saturation, non-blocking
  try_acquire underrun, 64-→-126 accumulator math (2-sample +
  4-sample carryover).
- **Teardown safety**: `aboutToQuit` clears the source
  registration FIRST (mutex-guarded in HL2Stream) BEFORE
  deleting txWorker — prevents the EP2 writer thread from
  calling a captured-pointer callback on a deleted object.

**Component 7 (NEXT) — FSM keydown/keyup wiring + first end-to-end
SSB voice TX:** keydown after rfDelayMs → start WDSP TXA channel
(`SetChannelState(1,1,0)`) → `setInjectTxIq(true)`; keyup →
`setInjectTxIq(false)` → wait for MoxEdgeFade fade-out → stop
WDSP TXA channel (blocking flush) → wire MOX clears (per §5.7
keyup ordering invariant).  Bench: operator dummy load + low
drive + mic input → first SSB voice key-up.

**2026-05-31 EVE — Component 7 SHIPPED + Thetis TX audio-path
study landed (pre-Component-8 design):**

- **Component 7** `7119214`: FSM keydown/keyup wired end-to-end.
  Operator first-SSB bench (dummy load, drive 100%): voice TX
  worked — wire path / TR-sequencing FSM / MoxEdgeFade two-stage
  release / §5.7 keyup invariant all clean over multiple cycles.
  ONLY issue: **0.2 W peak out** at 100% drive (mic peaks
  consistently −11 to −13 dBFS in the existing Q6.5 mic bench).
  Operator flagged the missing operator-tunable mic gain UI; bench
  itself was structurally clean.

- **Pre-Component-8 study commissioned + landed:** a focused
  research agent did a full read of Thetis 2.10.3.13 (console.cs
  + audio.cs + setup.cs + radio.cs + cmaster.c + the wdsp/TXA.c
  chain). Output =
  `docs/architecture/tx_audio_path_reference.md` (807 lines, every
  claim cited file:line). The study reframed the 0.2 W result:

  - **🚨 ALC `max_gain` default = 1.0 (0 dB) at WDSP create time**
    (`wdsp/TXA.c:322`). Thetis only escapes this because Setup
    immediately calls `SetTXAALCMaxGain(channel, 3.0)` from the
    profile default on first load (`setup.cs:9296-9301`, profile
    default at `setup.designer.cs:39547`). **Lyra-cpp NEVER calls
    this setter** — the ALC pins the entire TXA output chain at a
    hard 0 dB ceiling regardless of mic level. This is the
    load-bearing cause of the 0.2 W bench result, BIGGER than the
    missing mic-gain UI. One-line fix at TX channel open. Becomes
    **Component 8a-0** (ships ahead of 8a).
  - **PanelGain1 (the Mic Gain knob) is TXA chain stage #3** — before
    phrot, mic meter, EQ, leveler, CFCOMP, bandpass, compressor,
    OSCtrl, AND ALC (`wdsp/TXA.c` chain order; `patchpanel.c:55-101`
    `xpanel()` math). It is the ONLY operator-tunable software gain
    in the chain. Thetis default `mic_gain = 0.5` (≈ −6 dB) at
    construction (`radio.cs:4127`); lyra-cpp default 1.0 (= +6 dB
    hotter than Thetis-at-construction — operator slider should
    map 0..100 % onto dB Min/Max with default ~+10 dB to match
    Thetis-Setup-loaded behavior).
  - **20 dB Mic Boost is a HARDWARE codec PGA bit, NOT software**
    (`setup.cs:7851-7855` → `console.cs:13310-13320` →
    `console.cs:41787-41800` `SetMicGain()` → `NetworkIO.SetMicBoost`
    → ChannelMaster → HL2 C&C status bytes → AK4951 PGA register).
    Same for Line In (codec mux) and Line In Boost (0..31 dB step).
    Lyra-cpp Component 8a+ MUST wire these as C&C bits, NOT a
    TXA-side software multiplier (a parallel software boost
    double-counts and saturates the chain).
  - **VAC1/VAC2 routing is a 3D runtime truth table**:
    DSPMode × VAC1Enabled × PTT/MOX/SPACE override-bypass
    checkboxes, rewritten on every mode/VAC/bypass change via
    `cmaster.cs:1070-1110` `CMSetTXAPanelGain1`. Far more state
    surface than the original Component 8b plan assumed. **Beta-1
    decision: collapse Mic Source selector to 2-position (Mic In /
    Line In, both codec-mux C&C); defer VAC1/VAC2 to v2** with a
    UI tooltip noting it. (Aligns with operator preference for
    Combinator/EQ pre-processing — the digital-mode VAC path can
    land when the speech-chain UI lands.)
  - **TX bandpass is ONE H/L pair reinterpreted per-mode** via
    `UpdateTXLowHighFilterForMode` (`console.cs:8079-8118`). No
    per-mode preset matrix exists in Thetis. Default 100/3000;
    USB pass-through, LSB negate-and-swap, AM/SAM symmetric ±,
    FM symmetric, CW special. Component 8c ships two operator spin
    boxes (High/Low) driving `SetTXABandpassFreqs` via this
    transform; no preset-matrix UI work needed.
  - **TX meter rates**: 20 Hz for analog modes, 5 Hz for digital
    (`meter_delay=50`, `meter_dig_delay=200`). Per-meter getters
    documented in §10 of the study doc (`TXA_MIC_PK`,
    `TXA_LVLR_PK / GAIN`, `TXA_COMP_PK / AV`, `TXA_ALC_PK / AV /
    GAIN`, plus the HL2 C&C status bytes for Fwd / Rev / SWR / VDD
    / ID with HL2 decode formulas).

- **No-CFC reaffirmation:** the study doc's §8 documents Thetis
  CFCOMP at length, but ONLY for chain-position reference — Lyra
  does NOT ship CFC (§6.7 deferral table line 1111). The
  Lyra-native **5-band Combinator** replaces it (per `FEATURES.md`
  §3.3, operator-locked 2026-05-20). The study's read-first
  header makes this explicit so the spec isn't misread later.

**Component 8 ship order (locked 2026-05-31 EVE):**

| Slice | What | Status |
|---|---|---|
| **8a-0** | `SetTXAALCMaxGain(channel, 3.0)` at TX channel open | ✅ shipped 2026-05-31 |
| **8a** | Mic Gain UI: HL2Stream Q_PROPERTY `micGainDb` + TxControl callback + TX panel slider (dB-to-linear via `SetTXAPanelGain1`). Range −90..+40 dB. Hot-tunable during keydown. QSettings persistence. | ✅ shipped 2026-05-31 |
| **8a-tx-mode** | USB-stuck-LSB bug fix — push TX mode at registerTxControl + on operator mode change; designated-initializer call-site discipline | ✅ shipped 2026-05-31 |
| **8a+** | 20 dB Mic Boost checkbox → HL2 C&C MicBoost bit (NOT software) | ⏸ operator confirmed not needed for N8SDR's station; ship later if a tester needs it |
| **8b** | Mic Source: Mic In / Line In codec mux via C&C. VAC1/VAC2 anchor only (placeholder tooltip "available in v2") — no inert UI | 🟡 pending |
| **8c** | **RX BW + 🔗 lock + TX BW** combo row in ModeFilterPanel (mirrors old-Lyra's MODE+FILTER pattern). Per-mode persistence. SSB preset list 1500..10000 Hz. Lock toggle ON pulls RX→TX, both directions mirror after. `TxChannel::setBandpass` (USB pass-through, LSB negate-and-swap) wires through. | ✅ shipped 2026-05-31 (commit `2a949e2`) |
| **+#53** | **Shared RX+TX Filter Low edge** — single Settings → Audio spinbox (0..500 Hz, default 100 Hz, 10 Hz step). Operator-tunable interim until TX Profile Manager #49 ships per-profile (lo, hi) pairs. WdspEngine SSB/DIG asymmetric edge + HL2Stream TX bandpass low both read this. Tooltip carries mains-coupling warning. | ✅ shipped 2026-05-31 (commit `02bb7ea`) |
| **+#54** | **Panadapter lo-edge drag to 0** — SSB/DIG lo-edge handle now writes Prefs.filterLow (was always writing rxBandwidth and pinned at carrier). Drag below 0 pins at 0; hi-edge unchanged. CW (pitch-centred) and AM/DSB/FM (symmetric) lo-edge drag still writes rxBandwidth. | ✅ shipped 2026-05-31 (commit `02bb7ea`) |
| **8d** | TX multimeter: Mic / Comp / ALC / PO / SWR / VDD / ID source selector via MeterModel (picker exists per v2.1.1 §9.3.1); fills the ALC / MIC / COMP values pending from C7 | 🟡 pending |
| **8e** | Hardware PTT input (foot switch / hand mic) — opt-in default OFF per ff5f128 regression class | 🟡 pending |
| **+#49** | **TX Profile Manager** — named profile bundle (Mode + RX/TX BW + lock + filterLow + mic gain + ALC + Leveler + PHROT + future EQ/Combinator/Plate state). Save/Load/Delete/Set-as-Default. Manual select only — NO auto-detect by call. QSettings JSON. Ships W5UDX (Greg) + N8SDR personal Plate presets when Plate arc lands. Forward-compat schema absorbs the EQ/Combinator/Plate fields as those arcs land — no inert UI; only fields with backing setters appear. | 🟡 next major arc |
| **+#55** | Profile Manager quick-preset chip panel (operator-named profile chips for one-click recall, e.g. "4K" / "8K" / "Narrow DX" / "Wide ESSB"). Bundled with #49. | 🟡 pending, bundles with #49 |

---

## ⚓ POST-COMPONENT-8 TX ADVANCED-CHAIN ARCHITECTURE (LOCKED 2026-05-31 NIGHT)

Operator-locked locks captured in a research conversation 2026-05-31
(operator did the homework, brought concrete decisions back, asked me
to record them for circle-back at implementation time).  This block
is **architectural locks only — NOT design-blocking 8c**.  When the
EQ / Combinator / Plate implementation arcs begin, each will get its
own grounded-design → red-team → bench cycle per the locked
methodology; this block is the operator-locked starting posture, not
the full spec.

### Locked chain order (supersedes CLAUDE.md §15.19 ordering)

```
Mic → [WDSP EQ]──→ [5-band Combinator]──→ [Plate Reverb]──→ ALC/Limiter → TX BW Brickwall LPF → I/Q → EP2 → (PureSignal later)
       toggle        toggle                  toggle              ALWAYS ON      ALWAYS ON
```

Three operator-toggleable stages (EQ / Combinator / Plate); the
final ALC + brickwall LPF always run regardless to enforce 0 dBFS
strict + the operator's TX BW selection.

Rationale (operator-research-locked):
- **EQ before Combinator** — industry-standard; prevents EQ boost
  from clipping the limiter; lets the Combinator compress on the
  final-shaped tonal balance.
- **Plate INSIDE the chain, BEFORE the final limiter** — a plate
  AFTER the limiter lets reverb-tail peaks re-clip the DAC and
  confuses PureSignal's adaptive predistortion.  Combinator tames
  voice dynamics, Plate adds the ESSB "air," limiter catches any
  combined peak.
- **WDSP EQ, NOT biquads** — WDSP TXA already uses frequency-
  domain continuous-gain EQ via FFT bin interpolation (`eqp.c`,
  spline curve across bins, zero band-edge phase artifacts).
  A 5-band parametric UI maps to `SetTXAEQRun` + WDSP setters,
  NOT a separate IIR cascade.  Matches Thetis's posture; keeps
  the chain Thetis-faithful at the EQ stage.
- **Linkwitz-Riley IIR 24 dB/oct crossovers for the Combinator,
  NOT linear-phase FIR** — what X-Air uses; few-sample group
  delay, perfect summing.  PureSignal does NOT require linear-
  phase audio (Thetis itself ships non-linear-phase IIR
  `compress.c` / `cfcomp.c` / `wcpagc.c` and PS works there;
  PS cares about steady commanded-TX-IQ ↔ PA-feedback delay,
  not zero audio group delay).  Linear-phase FIR adds ms-scale
  latency for marginal benefit — skip it.
- **Reuse WDSP ALC (xwcpagc mode 5, the always-on splatter
  protection from §4.4) as the always-on final limiter** —
  1 ms attack / 10 ms decay / 3 dB max gain by default.
  Combined with the int16 saturation at EP2 packing, this is
  adequate 0 dBFS strict for PureSignal.  A separate look-
  ahead limiter is a polish item ONLY if bench testing shows
  ALC overshoots into the DAC; not load-bearing.
- **Default-bypass posture**: every toggleable stage starts
  OFF on first run (so a fresh-install operator hears their
  raw mic through ALC + BW LPF only, exactly as TX-1 ships
  today).  Operator opts in per-stage via Settings → TX
  and the Profile Manager.

### DSP2024P Plate Reverb — operator-supplied presets (LOCKED)

Behringer DSP2024P "PLAT" Schroeder-Moorer plate-reverb
algorithm.  Built native in C++23, NOT external hardware.
Two presets ship at first-light:

| Param | W5UDX preset (Greg) | N8SDR personal | Maps to |
|---|---|---|---|
| **PRE.D** (Pre-Delay) | 0.010 s | 0.010 s | Circular delay line before reverb |
| **DECA** (Decay time) | 2.358 s | 1.542 s | Comb-filter feedback coefficients |
| **DAMP** (HF Damping) | 10 | 15 | LPF inside the feedback loop |
| **SIZE** (Room Size) | 33 | 10 | Scales internal delay lengths |
| **SHV.D** (Shelf/Density) | 32 | 20 | All-pass coefficients |
| **DIFF** (Diffusion) | 20 | 20 | All-pass diffusion |
| **BASS** (wet low shelf) | −16 | −16 | Wet-tail low shelf cut |
| **TREB** (wet high shelf) | +16 | +16 | Wet-tail high shelf boost |

UI slider ranges to mirror the DSP2024P front panel:
- SIZE 1..100, DECA 0.1..5.0 s, DAMP 1..100, DIFF 1..100,
  PRE.D 0..0.100 s (1 ms steps), BASS / TREB ±18 dB.

Reverb on SSB transmit is unusual but is exactly what ESSB
operators do for the "broadcast-air" quality.  Bandplan-
constrained operators (DX contest etc.) just bypass it —
that's the toggle.

### Parked-awaiting-operator-screenshots (don't design yet)

When the EQ + Combinator arcs reach the implementation stage,
operator will provide:
- **Behringer X-Air XR12 Combinator** — screenshots + settings
  values (crossover frequencies, per-band threshold / ratio /
  attack / release / makeup, top-band exciter "polish" param,
  any preset names operator uses).  We'll mirror the param
  surface in the 5-band Combinator UI rather than guess.
- **X-Air parametric EQ** — screenshots + settings (band freqs,
  Q values, gain ranges).  We'll mirror the param surface for
  the WDSP-EQ-driving 5-band UI rather than invent.

Until those screenshots land, do NOT pick crossover defaults /
EQ band defaults by guesswork — the operator-curated values are
the spec.

### Open questions parked for the implementation arcs

These are NOT blocking 8c; they're flagged so a future Claude
session reading this block knows to RAISE them at the start of
each advanced-chain implementation arc instead of deciding
unilaterally:

1. **Combinator crossover frequencies fixed-default vs operator-
   tunable?**  Hardware compressors typically lock crossover
   freqs; X-Air does too.  Operator screenshots will answer
   when they arrive.
2. **EQ band count — 3 or 5?**  Lean to 5 to match Combinator
   visual count; operator confirms when EQ UI arc starts.
3. **Plate preset bank scope** — ship just W5UDX + N8SDR, or
   add factory "short room / long hall / dense ring" presets
   for testers?  Operator picks when Plate arc starts.
4. **TX Profile Manager schema** — full chain bundle (Mode +
   BW + lock + mic gain + mic source + ALC + Leveler + PHROT
   + EQ enable + EQ bands + Combinator enable + Combinator
   bands + Plate enable + Plate params + mute-on-TX policy).
   Manual select only (NO auto-detect by call, per §15.19
   operator standing rule).  Save / Load / Delete / Set-as-
   Default.  QSettings JSON serialization.
5. **What other AI advice to ignore** — the operator-research
   conversation suggested AVX-512 SIMD, `std::jthread` for
   the audio thread, and Vulkan-compute for DSP.  All
   over-engineering for a 48 kHz mono 5-band workload.  Stay
   with the existing TxDspWorker (`std::thread` + MMCSS Pro
   Audio).  Vulkan stays panadapter-only.

### Sub-release ordering (LOCKED — implementation arcs come AFTER 8c–8e)

1. **8c — TX Bandwidth row** (lock-button + RX/TX combo, mirrors
   old-Lyra `ModeFilterPanel` pattern; DSP wiring already done
   via `TxChannel::setBandpass()`).  **NEXT**, ~2-3 hr.
2. **TX Profile Manager** (schema lock + Save/Load/Delete/
   Set-as-Default + QSettings JSON).  Lands AFTER 8c so the
   schema's first non-mic field (TX BW) is real.  Subsequent
   advanced-chain features add fields to the profile schema as
   they land — no inert UI; only fields with backing setters
   appear in the profile.  ~1 day.
3. **5-band parametric EQ UI** driving WDSP `SetTXAEQRun` +
   band setters.  Pending operator X-Air EQ screenshots.
   ~1 day after screenshots.
4. **5-band Combinator** — brand-new C++ class.  Linkwitz-
   Riley IIR 24 dB/oct crossovers + 5 parallel compressor
   blocks + summation + WDSP ALC at output (reused as the
   always-on limiter).  Pending operator X-Air Combinator
   screenshots.  ~1 week after screenshots.
5. **Plate Reverb (DSP2024P-faithful)** — Schroeder-Moorer
   class with the 8 mapped params + W5UDX + N8SDR presets
   shipped.  Settings already operator-supplied; no
   screenshot wait.  ~4-5 days.
6. **8b Mic Source / 8e HW PTT / 8d meter fillers / #44
   panadapter rescale / #47 cosmetic polish** — small
   slices, can interleave with the advanced-chain arcs as
   the operator wants.

### Capture-it-for-next-session

If a future Claude session reads this block before the
operator does another in-session brief, treat the locks here
as **operator-curated standing decisions** that supersede
older §15.19-era plans.  Raise the parked questions at the
start of each implementation arc rather than guessing.  The
operator-empirical authority outranks plan inference, every
time (§3.9 / §15.28 lessons codified throughout CLAUDE.md).

---

## 1. Operator-visible acceptance metric (defined BEFORE design)

Per §15.28 lesson #4: a change without a falsifiable operator-visible
behaviour metric is furniture, not a step.

**Tier-A unit bench** (synthetic, automatable):
- Inject a 1 kHz mic tone at -10 dBFS into `TxChannel::process()` in
  USB mode with operator passband (+200..+3100 Hz)
- FFT the resulting EP2 TX I/Q output, assert:
  - Wanted sideband at the correct baseband sign, ≥-20 dBFS
  - Unwanted sideband ≤-60 dBFS below wanted
  - Carrier (DC) ≤-60 dBFS
  - Mirror-symmetric in LSB mode at -1 kHz baseband sign

**Hardware bench** (N8SDR HL2+, dummy load, NO external amp):
- Mic into HL2+ codec, USB mode, freq 28.495 MHz, drive 50%, PA enabled
- Speak continuously for 5 seconds
- A/B against Thetis-on-same-hardware: voice intelligible at same freq
- External SDR / RSPdx confirms ≥60 dB sideband + carrier suppression
- Palstar reads stable power consistent with voice envelope (not zero,
  not pegged)
- Zero clicks at keydown / keyup (analyzer trace clean)
- 20× rapid Stop / Start cycles with mic active: no hang, no dead RX,
  no stuck carrier
- Mode swap LSB ↔ USB mid-TX: clean transition, no transient

**Phase-3-EXIT kill-test** (bundled here; otherwise pending):
- `taskkill /F` Lyra mid-mic-TX
- PA-current banner drops to idle within HL2 gateware watchdog window
  (~13 s per `hl2_stream.cpp:518`)
- Re-launch: comes up RX (cb58bcb / come-up-not-keyed invariant holds)

**FAIL → revert.**  Operator-empirical outranks any agent verdict.

---

## 2. Lyra-cpp current TX state (load-bearing context)

From `src/hl2_stream.{h,cpp}` (verified by direct grep 2026-05-29):

- **TUN carrier today** = constant DC injection inline in EP2 packer at
  `hl2_stream.cpp:1622-1638` (`kTuneCarrierI ≈ 0.95 × 32767`, Q=0).
  NOT WDSP TXA.  Zero CPU cost.  Mode-independent (correct on a DC
  zero-beat at the dial freq).  **Keep this for TUN** — it ships, it
  works at 5 W into dummy, no §15.23-class risk.  TX-1 ONLY builds
  the WDSP TXA chain for SSB / digital voice modes.
- **EP2 writer pacing** = producer-paced with silence-keepalive
  timeout at `hl2_stream.cpp:1652-1685`.  Audio buffer drained at
  126-stereo-frame chunks.  TX I/Q produced inline per-sample in the
  same packing loop.
- **MOX bit** = snapshotted once per datagram at
  `hl2_stream.cpp:1574`, used for both C0 and TX I/Q gating
  (`hl2_stream.cpp:1614`).
- **No mic input** — bytes 24-25 of each EP6 26-byte slot are
  explicitly documented "ignored, future TX work" at
  `hl2_stream.cpp:727`.
- **No WDSP TXA bindings** in `src/wdsp_native.{h,cpp}`
  (`SetTXA*` symbols not loaded).
- **No TxChannel** in `src/wdsp_engine.{h,cpp}` (RxChannel only).
- **No MoxEdgeFade** — TUN flips on/off purely via `tuneEnabled_` AND
  `moxBit` snapshot per datagram.  No envelope shaping.
- **PTT FSM** lives inside `HL2Stream` (NOT a separate `ptt.cpp`);
  keydown/keyup hooks `fsmKeydownSettled` / similar at
  `hl2_stream.h:521+`.
- **TX-0c register surface** = drive level (`set_tx_drive_level`),
  PA enable (`set_pa_enabled`), tune carrier (`setTuneEnabled`),
  step attenuator with `31-x` HL2 encoding (`set_tx_step_attn_db`),
  TX safety timeout, ATT-on-TX policy — all shipped, hardware-
  validated 2026-05-28.  Frame composers
  (`_compose_frame_0/4/10/11/etc.`) are byte-correct for HL2+
  ak4951v4 gateware.
- **CMakeLists.txt**: no `wdsp_tx_engine.cpp` / `tx_dsp_worker.cpp` /
  `mic_source.cpp` / `mox_edge_fade.cpp` referenced.

---

## 3. Thetis reference dossier A — wire surface

(Grounded by Explore agent 2026-05-29; cited Thetis source paths under
`D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\`.)

### 3.1 EP2 TX I/Q packing
- `networkproto1.c:1194-1195` + `:1241-1259`: main packing loop inside
  `sendProtocol1Samples()`.
- EP2 frame = 1032 bytes total = 8-byte Metis header + 2 × 512-byte USB
  frames.  Per USB frame: 3-byte sync (`0x7f 0x7f 0x7f`) at offset 0–2,
  5-byte C0..C4 at offset 3-7, then 504 bytes = 63 LRIQ tuples × 8
  bytes/tuple.  Total 63 × 2 = 126 LRIQ tuples per EP2 datagram.
- Per-tuple offsets: L (2B BE), R (2B BE), TX-I (2B BE), TX-Q (2B BE).
  Same layout Lyra-cpp already implements at `hl2_stream.cpp:1628-1637`.
- **Sample scaling**: Thetis stores TX I/Q as `double` in
  `prn->outIQbufp`, scales by `× 32767.0` then casts to `int16` BE.
  Lyra-cpp's TUN injection uses the same convention
  (`kTuneCarrierI ≈ 0.95 × 32767`).
- **CW mode bit-pack override** at `networkproto1.c:1248-1252`: for HL2
  in CW mode, the Q-slot of the IQ stream is bit-packed with CWX state
  (cwx_ptt bit 3, dot bit 2, dash bit 1, cwx bit 0).  CW = later slice;
  noted for forward compat (TX-1 must NOT clobber this — but TX-1 only
  touches USB/LSB so no current risk).

### 3.2 HL2 mic → TX path

**REWRITTEN 2026-05-29** — earlier "bytes 24-25" framing was wrong;
mic samples are interleaved with IQ per an nddc-aware formula.
Verified against `networkproto1.c:562-577` (`MetisReadThreadMainLoop
_HL2`).

- **Per-sample offset formula:**
  `k = 8 + nddc*6 + isamp * (2 + nddc*6)`
- **Per-frame mic-sample count:** `spr = 504 / (6*nddc + 2)`
- For HL2's typical nddc=1: stride=8 bytes, mic samples at offsets
  **14, 22, 30, …, 510** — 63 mic samples per 512-byte USB frame.
- For nddc=2: stride=14, 36 samples per frame.
- For nddc=4 (HL2 Lyra default per CLAUDE.md §3.1): stride=26, 19
  mic samples per frame.
- **Mic format:** 16-bit big-endian signed PCM
  (`bptr[k]<<8 | bptr[k+1]`); sign-extended via `<<24 | <<16` then
  divided by 2³¹ to normalize to [-1, +1).  L-channel only, R forced
  to 0.0.
- `networkproto1.c:562-577` extract sequence + scaling.
- `networkproto1.c:579`: `Inbound(inid(1,0), mic_sample_count,
  prn->TxReadBufp)` hands the (I=mic_scaled, Q=0) complex pairs to
  ChannelMaster.
- `cmbuffs.c:89-121`: `Inbound()` copies into ring `r1_baseptr`,
  signals `Sem_BuffReady`.
- `cmaster.c:112-253`: ChannelMaster TX DSP thread pumps the ring →
  routes through TX DSP → `prn->outIQbufp` for EP2 packing.

**Operator bench result (N8SDR HL2+, 2026-05-29):** reading bytes 24-25
of each "slot" produced SILENCE on his AK4951 even with a mic plugged
in.  That's because the slot framing is nddc-dependent — bytes 24-25
for nddc=1 land mid-IQ-sample (not a mic word at all).  Implementing
session MUST use the formula above + bench-gate the parser before
wiring TX (per §1).

### 3.3 MOX bit gating
- `networkproto1.c:615` + `:896`: `C0 = (unsigned char)XmitBit` —
  MOX written to C0 bit 0.  Lyra-cpp already does the same at
  `hl2_stream.cpp:1574` (per-datagram snapshot).
- `networkproto1.c:1227`: `if (!XmitBit) memset(prn->outIQbufp, 0,
  ...)` — software belt+suspenders: TX I/Q ZEROED when RX, even though
  gateware would also gate.  Worth mirroring in Lyra-cpp for defence
  in depth.

### 3.4 TX-NCO freq writes
- `networkproto1.c:974-980`: case 1 in `WriteMainLoop_HL2()`,
  "TX VFO 0x01".  Pattern: `C0 |= 2; C1..C4 = (tx[0].frequency >>
  24,16,8,0) & 0xff` — big-endian word split.
- HL2 gateware caches the last write; same-cycle persistence implicit.
  Thetis updates on dial change.
- Lyra-cpp already implements this via `_compose_frame_*` slot 2 of
  the 18-case round-robin.

### 3.5 Drive level + PA enable
- `networkproto1.c:1076-1089`: case 10, "Drive/Filter/PA 0x09" (C&C
  address 0x09 = Lyra-cpp frame 10).
- `C1 = tx[0].drive_level` (0-255).
- `C3 bit 7 = tx[0].pa`.  **Note**: on the operator's HL2+/ak4951v4
  gateware this is the LEGACY path and is ignored — the real PA
  enable is C2 bit 3 (Apollo Tuner pin, per the §15.26 RTL findings
  already implemented in Lyra-cpp task #29).  Lyra-cpp already sets
  C2 bit 3 correctly.  Do NOT regress this in TX-1.
- `C2 bit 7 = VNA` forced to 0 (`networkproto1.c:750`, `& 0x7f`) —
  Lyra-cpp already does this.

### 3.6 C&C round-robin scheduling
- HL2 has **19 cases (0..18 inclusive)** per `WriteMainLoop_HL2`
  (`networkproto1.c:948-1176`).  Wheel advances via
  `if (out_control_idx < 18) out_control_idx++; else out_control_idx
   = 0;` at `networkproto1.c:1180-1183`.
- Each USB frame consumes one slot; ~760 USB frames/s; each register
  revisits at ~40 Hz.
- Lyra-cpp `hl2_stream.cpp:1585` `ccIdx_ = (ccIdx_ + 1) % 19` is
  **byte-exact** vs the Thetis HL2 wheel.  CONFIRMED 2026-05-29
  red-team audit.

### 3.7 SendHighPriority on MOX edge
- `console.cs:30073`: `NetworkIO.SendHighPriority(1)` called on MOX
  keydown — emits immediate C&C frame outside round-robin, reduces
  MOX-keydown latency.
- Lyra-cpp does NOT have this today.  Optional flag for TX-1: emit
  next-available frame immediately on MOX/PTT edge so the gateware
  sees the MOX bit + step-att + drive_level coherently at the
  earliest possible moment.

### 3.8 Sample rate
- HL2 TX I/Q is fixed 48 kHz (AK4951 codec hard-locked, per
  CLAUDE.md §3.5).  No resampling on the wire.
- 126 LRIQ tuples per datagram / 2.625 ms per datagram → 48 kHz exact.

---

## 4. Thetis reference dossier B — WDSP TXA DSP chain (SSB only)

(Grounded by Explore agent 2026-05-29.)

### 4.1 TXA channel scaffolding
- `wdsp/TXA.c` `create_txa()` builds the chain.  Block instantiation
  order:
  - rsmpin (line 40) — input resampler, run=0
  - gen0 (line 51) — input signal gen, run=0
  - **panel** (line 59) — `inselect=2` (I=mic), copy=0, run=1
  - **phrot** (line 71) — run=0, fc=338 Hz, nstages=8
  - micmeter (line 80) — run=1
  - amsq (line 95) — run=0
  - leveler (line 158) — run=0, τa=1ms, τd=500ms, max_gain=+5dB
  - cfcomp (line 202) — run=0
  - **bp0** (line 239) — **run=1 always-on**; the SSB sideband selector
  - compressor (line 253) — run=0
  - **bp1** (line 260) — **run=0**; default frozen at f_low=-5000,
    f_high=-100; compressor-only aux; **NEVER touch via
    SetTXABandpassRun** (§15.23 trap)
  - osctrl (line 274) — run=0
  - bp2 (line 282) — run=0
  - **alc** (line 311) — **run=1 always-on**; τa=1ms, τd=10ms,
    max_gain=1.0
  - gen1 (line 361) — run=0; output-side gen for TUN / two-tone (NOT
    used by Lyra-cpp's TUN — Lyra-cpp uses DC inline instead)
  - uslew (line 369) — upslew_time=5ms
  - rsmpout (line 451) — run=0
- Runtime exec order in `xtxa()` line 557-592: input → gen0 → panel →
  phrot → meters → leveler → CFC → **bp0** → compressor → bp1 → osctrl
  → bp2 → **alc** → AM/FM mods → gen1 → uslew → rsmpout → output.
- **SSB hot path**: mic → panel(I=mic) → phrot (run=0 default — operator
  opt-in) → bp0 (sign-aware) → alc → uslew → out.

### 4.2 SetTXAMode + SetTXABandpassFreqs
- `wdsp/TXA.c:753-789`: `SetTXAMode` sets mode + flags ammod/fmmod off
  for SSB, then calls `TXASetupBPFilters(channel)`.
- `wdsp/TXA.c:792-800`: `SetTXABandpassFreqs` sets f_low / f_high then
  calls `TXASetupBPFilters`.
- `wdsp/TXA.c:827-901`: `TXASetupBPFilters`: SSB modes (USB/LSB/CWL/
  CWU/DIGL/DIGU/SPEC/DRM) set `bp0.run=1`, `bp1.run=0`, `bp2.run=0`,
  call `CalcBandpassFilter(bp0, f_low, f_high, 2.0)`.  Only if
  compressor.run does bp1.run get enabled (with same edges).
- **The trap (§15.23)**: bp1 created with f_low=-5000, f_high=-100
  (line 268-269).  Directly calling `SetTXABandpassRun(ch, 1)` toggles
  bp1.run without recomputing its kernel → cascades stale negative-
  baseband-only filter after correct bp0 → kills USB.
- **Verified**: zero call sites to `SetTXABandpassRun` in Thetis tree.

### 4.3 Per-mode SSB sign convention
- `Console/console.cs:8079-8118` `UpdateTXLowHighFilterForMode`.
- Operator passes positive `low` / `high` (e.g., 200, 3100).  Engine
  transforms per mode:
  - **USB / CWU / DIGU**: l=+low, h=+high (positive baseband)
  - **LSB / CWL / DIGL**: l=-high, h=-low (negative baseband)
  - **DSB / AM / SAM / FM**: l=-high, h=+high (symmetric)
  - **DRM**: fixed l=+7000, h=+17000
- TX-1 implements USB and LSB only.  Other modes structurally compat
  for later slices.

### 4.4 ALC defaults (always-on, splatter protection)
- Created at `TXA.c:311-334`: tau_attack=1 ms, tau_decay=10 ms,
  max_gain=1.0 (0 dB), out_target=1.0, hang_enable=0, **run=1**.
- Setters (`wcpAGC.c:578-610`): `SetTXAALCAttack/Decay/Hang/MaxGain/St`.
- **No `SetTXAALCThresh` exists.**  MaxGain alone governs ALC ceiling.
- **No operator opt-out** in TX-1 design — ALC is mandatory splatter
  protection.  Operator can tune MaxGain via Settings (range proposal:
  -3 to +3 dB around the WDSP default 0 dB).

### 4.5 Leveler defaults (operator opt-in)
- Created at `TXA.c:158-181`: tau_attack=1 ms, tau_decay=500 ms,
  max_gain=1.778 (+5 dB), out_target=1.05, hang_enable=0, **run=0**.
- Setters (`wcpAGC.c:613-654`): `SetTXALevelerAttack/Decay/Hang/Top/St`.
- TX-1 ships leveler **wired but OFF by default**, operator opt-in
  toggle in Settings → TX.  Defer cooler-pre-EQ / compressor / CFC to
  later slices.

### 4.6 PHROT (Phase Rotator)
- `wdsp/iir.c:665-703`.  Setters use **UPPERCASE PHROT**:
  - `SetTXAPHROTRun(ch, run)` (line 665)
  - `SetTXAPHROTCorner(ch, corner)` (line 675) — NOT `SetTXAPHROTFreq`
  - `SetTXAPHROTNstages(ch, nstages)` (line 686)
- Defaults: fc=338 Hz, nstages=8, run=0.
- Purpose: PEP-to-PAR reduction (~3-4 dB) by flattening group delay in
  passband.
- TX-1 ships PHROT **ON by default** (Thetis-faithful) — quietly
  improves on-air PEP-to-PAR without operator visible knobs.  Future
  operator opt-out if anyone wants the WDSP-default off.

### 4.7 Panel input selection
- `wdsp/patchpanel.c:55-101` `xpanel`: with inselect=2,
  `I = in[2i+0] * (inselect>>1) = in[2i+0] * 1 = mic`;
  `Q = in[2i+1] * (inselect & 1) = 0`.
- Default `inselect=2` at create.  **Zero call sites to
  `SetTXAPanelSelect` in Thetis** — defaults are correct, do NOT call
  it (would set copy=3 for I-Q swap balance-test mode).

### 4.8 Rate setters
- `wdsp/channel.c:197` `SetInputSamplerate`, `:227`
  `SetOutputSamplerate`, `:211` `SetDSPSamplerate` — **shared RX/TX,
  generic channel-level**.
- **No `SetTXAInRate` / `SetTXAOutRate`** (these don't exist).

### 4.9 Channel state lifecycle
- `wdsp/channel.c:259-297` `SetChannelState(ch, state, dmode)`:
  - **state=1 (keydown)**: sets `slew.upflag`, `ch_upslew`, clears
    `exec_bypass`, triggers DSP thread.  dmode ignored.
  - **state=0, dmode=1 (keyup blocking)**: sets `slew.downflag`,
    `flushflag`, waits up to 100 ms for flush.  Forcibly clears on
    timeout.
- `uslew.c` envelope: cos²-shaped 5 ms ramp gated by ch_upslew on
  keydown — **gated to TUN / tone-gen paths ONLY, NOT SSB mic.**

**RESOLVED (red-team verification, 2026-05-29):** `wdsp/uslew.c` +
`wdsp/TXA.c` read end-to-end.  uslew **does NOT envelope-shape the
SSB mic stream**.  Proof:
- `wdsp/slew.c:90-92` (xuslew, first line):
  `if (!a->runmode && TXAUslewCheck(a->channel)) a->runmode = 1;` —
  uslew only arms when `TXAUslewCheck` returns true.
- `wdsp/TXA.c:819-825` (`TXAUslewCheck`): returns true **only if**
  `ammod.run || fmmod.run || gen0.run || gen1.run`.  SSB (USB/LSB)
  sets NONE of those (`SetTXAMode` switch at `TXA.c:762-785` leaves
  ammod/fmmod off for SSB modes; gen0/gen1 default off and are only
  enabled for input-signal-gen / TUN / two-tone).
- `wdsp/slew.c:153-154` (fall-through, !runmode): `memcpy(out, in,
  size * sizeof(complex))` — mic passes through untouched.

**Consequence for MoxEdgeFade (component 5):** MoxEdgeFade carries
**the FULL keydown AND keyup envelope responsibility for SSB voice TX**.
Fade-IN is **load-bearing**, not optional — without it every keydown
is a hard step at the EP2 packer = audible key click + adjacent-channel
splatter.  The "worst case both apply, longer wins" framing in earlier
drafts was wrong: WDSP's 5 ms cos² envelope does not run at all on
SSB, so MoxEdgeFade is the sole envelope.

---

## 5. Lyra-cpp TX-1 architecture (design v1)

### 5.1 Component list (6 components, all new for SSB voice)

| # | Component | New file | Lines (est.) |
|---|---|---|---|
| 1 | WDSP TXA cdefs + loader | extend `src/wdsp_native.{h,cpp}` | ~150 |
| 2 | TxChannel class | extend `src/wdsp_engine.{h,cpp}` | ~350 |
| 3 | Hl2Ep6MicSource (single class, no abstraction) | new `src/mic_source.{h,cpp}` | ~150 |
| 4 | TX DSP worker (dedicated thread) | new `src/tx_dsp_worker.{h,cpp}` | ~250 |
| 5 | MoxEdgeFade (cos² 50 ms) | new `src/mox_edge_fade.{h,cpp}` | ~120 |
| 6 | EP2 packer extension + sip1 tap | edit `src/hl2_stream.cpp:1622-1638` | ~70 |

Plus: Settings UI (TX panel: PHROT toggle, Leveler toggle, bandpass
sliders, mic boost / bias / mic-vs-line / line-in gain, HW-PTT toggle)
~150 lines in `settingsdialog.cpp`.

**Total estimate**: ~1240 lines (down from v1's ~1450 — MicSource
abstraction dropped per the §5.4 simplification).

**Per-component bench gates (§15.28 methodology, MANDATORY before
end-to-end Tier-A):**
1. WDSP loader: cdef dlsym audit — assert all symbols resolve from
   bundled libwdsp before any test runs.
2. TxChannel: `open()` setter-return-code audit; dump post-init bp0
   passband edges to log.
3. Hl2Ep6MicSource: loopback test piping a synthetic 1 kHz mic block
   through the consumer pattern with EP6 telemetry probe confirming
   bytes parse correctly via the §3.2 nddc-aware formula.  **This is
   also the Q6.5 mic-route bench** — verify AK4951 voice comes through
   with `line_in=0` + `mic_bias` as appropriate.
4. TX DSP worker: queue throughput at 48 kHz cadence with no
   consumer (just measures producer→worker→consumer plumbing).
5. MoxEdgeFade: cos² envelope unit test (fade-in monotonic, fade-out
   reaches exactly zero, abort-continuity on rapid keydown).
6. EP2 packer: TUN-vs-SSB-vs-silence dispatch unit test.

### 5.2 WDSP TXA cdefs (component 1)

Symbols to add to `wdsp_native.{h,cpp}` (mirror existing RxChannel
loader pattern):

```
OpenChannel(int channel, int in_size, int dsp_size, int input_samplerate,
            int dsp_rate, int output_samplerate, int type, int state,
            double tdelayup, double tslewup, double tdelaydown,
            double tslewdown, int block);
CloseChannel(int channel);
SetChannelState(int channel, int state, int dmode);
SetInputSamplerate(int channel, int in_rate);
SetOutputSamplerate(int channel, int out_rate);
SetDSPSamplerate(int channel, int dsp_rate);
fexchange0(int channel, double* in, double* out, int* error);

SetTXAMode(int channel, int mode);
SetTXABandpassFreqs(int channel, double low, double high);
SetTXAPHROTRun(int channel, int run);
SetTXAPHROTCorner(int channel, double corner);
SetTXAPHROTNstages(int channel, int nstages);
SetTXAALCAttack(int channel, int attack_ms);
SetTXAALCDecay(int channel, int decay_ms);
SetTXAALCHang(int channel, int hang_ms);
SetTXAALCMaxGain(int channel, double max_gain);
SetTXAALCSt(int channel, int run);
SetTXALevelerAttack(int channel, int attack_ms);
SetTXALevelerDecay(int channel, int decay_ms);
SetTXALevelerHang(int channel, int hang_ms);
SetTXALevelerTop(int channel, double max_gain);
SetTXALevelerSt(int channel, int run);
SetTXAPanelGain1(int channel, double gain);  // operator mic gain
GetTXAMeter(int channel, int meter_type);    // for meter readouts
```

**Explicitly NOT cdef'd** (traps): `SetTXABandpassRun` (§15.23),
`SetTXAPanelSelect` (no call sites + default correct),
`SetTXAALCThresh` (doesn't exist).  Comment-anchor "DELIBERATELY
OMITTED" in the cdef list with §15.23 ref so future audits don't
add them.

### 5.3 TxChannel class (component 2)

```cpp
class TxChannel {
public:
    enum Mode { USB, LSB };  // others later

    TxChannel(int channelId);
    ~TxChannel();

    void open(int micRate, int dspRate, int outRate);
    void close();
    void start();   // SetChannelState(ch, 1, 0)
    void stop();    // SetChannelState(ch, 0, 1) - blocking

    // Operator-positive Hz.  Engine signs internally per mode.
    void setMode(Mode);
    void setBandpass(double opLowHz, double opHighHz);

    void setMicGainDb(double db);     // SetTXAPanelGain1
    void setAlcMaxGainDb(double db);  // SetTXAALCMaxGain
    void setLevelerOn(bool, double topDb = 5.0);  // SetTXALevelerSt
    void setPhrotOn(bool);            // SetTXAPHROTRun

    // Hot path.  mic_block is `n` mono samples (mic in I slot,
    // Q=0 — engine routes via panel inselect=2 default).  Output
    // is `n` complex<float> TX I/Q.  Returns WDSP error code.
    int process(const float* mic_block, int n,
                std::complex<float>* iq_out);

private:
    int channel_;
    Mode mode_ = USB;
    double opLow_ = 200.0, opHigh_ = 3100.0;

    // Centralized sign convention per §15.23 lesson.
    void pushBandpassLocked();
    std::pair<double, double> signedEdges(Mode, double low, double high);
};
```

**🔒 AMENDED 2026-05-30 (operator-mandated "do as Thetis does,
Lyra-Native style"):** the v2 init sequence below was WRONG.  It
was caught only after `TxChannel::open()` shipped with these v2
parameters + setters, was wired into the worker thread, and
crashed on the FIRST `fexchange0` call inside a WDSP-internal
memcpy (1488 B / 93 complex<double> frames, source buffer
overrun).  Root cause: **Lyra picked parameters that diverge from
the reference, breaking integer-divisor assumptions inside WDSP's
ring math.**  Specifically:

| v2 (WRONG) | Reference (`cmaster.c::create_xmtr`, line 177-190) | Why it matters |
|---|---|---|
| `in_size=126` (per-datagram mic count) | `in_size = getbuffsize(in_rate) = 64 * rate / 48000` = **64** at 48 k | Reference's universal "constant-latency" rule.  64 divides `dsp_insize=2048` cleanly (×32); 126 does not (1024/126 = 8.127), and the residuals corrupt WDSP's ring-position arithmetic on first call. |
| `dsp_size=2048` | **4096** | Reference value; `max(2048, dsp_size)` clamp in TXA.c sizes downstream FFT-coef buffers |
| `tdelayup=0.010` | **0.000** | We added a 480-sample delay the reference doesn't |
| `tslewup=0.025` | **0.010** | Different envelope shape |
| 17× `SetTXA*` setters at open() (mode, bandpass, PHROT, ALC, Leveler) | **ZERO** `SetTXA*` calls in `create_xmtr` | Reference leaves TXA chain at WDSP create-time defaults until the operator-settings layer (radio.cs / console.cs) flows through.  We were exercising WDSP code paths the reference doesn't touch at this lifecycle stage. |
| `SetChannelState(ch, 1, 0)` inlined at end of open() | NOT called in create_xmtr (channel armed later by `chkMOX_CheckedChanged` only when keyed) | We fused "open" with "start" — making TX go live with zero operator intent the moment the channel was constructed. |

**The Lyra-Native style is the SURROUNDING architecture** (Qt
+ Vulkan + our process/thread model + our facade APIs).  **The
WDSP calls themselves must match the reference byte-for-byte** —
every parameter value, every setter, every lifecycle stage.  If
Lyra picks a value the reference doesn't pick, we're saying
"we know better than the reference" — and the reference has been
right every single time it's been verified this project.

Init sequence (in `open()`), reference-faithful (2026-05-30):

1. `OpenChannel(ch, in_size=64, dsp_size=4096, in=48k, dsp=96k,
   out=48k, type=1, state=0, tdelayup=0.000, tslewup=0.010,
   tdelaydown=0.000, tslewdown=0.010, block=1)` — byte-for-byte
   matches `cmaster.c::create_xmtr` line 177-190 for HL2 family
   TX at 48 kHz mic.  In_size derived from
   `cmsetup.c::getbuffsize` rule.

2. **No `SetTXA*` calls.**  The TXA chain stays at WDSP
   create-time defaults (`wdsp/TXA.c::create_txa`).  Operator-
   tunable state (mode, bandpass, PHROT, ALC, Leveler, panel
   gain) is configured LATER by the operator-settings layer
   when the operator interacts with the TX panel.  The
   per-setter API methods (`setMode`, `setBandpass`,
   `setMicGainDb`, `setPhrotOn`, `setLevelerOn`,
   `setAlcMaxGainDb`) remain on `TxChannel` for that flow.

3. **Channel STAYS at state=0 (parked).**  Do NOT call
   `SetChannelState(ch, 1, 0)` here.  The reference opens the
   channel parked in `create_xmtr` and only arms it later when
   the operator actually keys (`chkMOX_CheckedChanged` →
   `SetChannelState(id(1,0), 1, 0)` in console.cs).  Our
   `start()` method serves this role; the PTT/MOX edge handler
   in component 5/6 will call it when ready.

### 5.4 Mic source (component 3) — single class, no abstraction

**REWRITTEN 2026-05-29** — collapsed from the v1 3-concrete
abstraction to a single class, per Thetis-faithful pattern verified
2026-05-29:

> Thetis HAS NO PC-soundcard mic input.  Operator mic is **always**
> the radio's onboard codec via EP6 at fixed 48 kHz
> (`networkproto1.c:562-577`).  The "PC audio for TX" case is
> **only** VAC1: PortAudio opens at user-selected `vac_rate` and
> WDSP `rmatchV` resamples to/from 48 kHz (`ChannelMaster/ivac.c:38-44,
> 343-352`).  Operator selects mic vs line input + boost via standard
> C&C bytes (cases 10 & 11), not via separate "mic source" objects.

Lyra-cpp therefore ships **one mic source class**:

```cpp
class Hl2Ep6MicSource {
public:
    // Consumer registers with HL2Stream's EP6 tap. The producer
    // (HL2Stream RX loop) extracts mic samples per the §3.2
    // nddc-aware formula and pushes to the consumer.
    using Consumer = std::function<void(const float* samples, int n)>;

    explicit Hl2Ep6MicSource(HL2Stream& stream);
    ~Hl2Ep6MicSource();

    void setConsumer(Consumer);
    int sampleRate() const { return 48000; }
};
```

No abstraction layer.  Only ONE concrete implementation justifies one
class (per locked rule "three similar lines beats premature
abstraction" — one concrete doesn't justify an ABC).

**Future PC-audio-for-TX (NOT in TX-1):** when wanted (e.g., bridge
WSJT-X via Virtual Audio Cable), ship a single VAC1-style feature
following the Thetis pattern: operator-toggled loopback ring buffer
between a PC audio device and the TX chain, with a real
rational-ratio resampler (WDSP RMATCH equivalent) between the device
rate and the radio's fixed 48 kHz TX input.  This is a future
milestone (likely v0.2.3 or v0.3 along with digital-mode workflows),
NOT another mic source class.  Q6.1 (auto-switch on DIGU/DIGL) is
deleted from TX-1 scope — Thetis doesn't auto-switch and we follow
suit.

**EP2 C&C cases 10 & 11 (mic routing — already in the round-robin
wheel):** Settings → TX panel exposes operator controls that compose
the bytes per `networkproto1.c:1076-1103`:

- Case 10 (C&C addr 0x09) C2 carries `mic_boost | (line_in<<1) | …`
- Case 11 (C&C addr 0x0a) C1 carries `mic_trs | mic_bias | mic_ptt`
  bits; C2 carries `line_in_gain`.

Settings exposes `mic_boost` (toggle), `line_in` (mic vs line radio
button, default mic = 0), `mic_bias` (electret toggle, default off),
`line_in_gain` (slider 0..31).  HL2 gateware reads these and routes
the AK4951 input accordingly — no AK4951 I²C side-channel needed
(Thetis-verified 2026-05-29: zero I²C transactions to the codec
anywhere in the Thetis tree; HL2 gateware initializes the AK4951
autonomously at bitstream init).

### 5.4.1 Future VAC1 TX-input bridge (v2.1 spec lock — NOT in TX-1)

Per §5.4 v2: PC audio for TX is a future milestone (v0.2.3 or v0.3
alongside digital-mode workflow).  This subsection LOCKS the spec
now so the implementing session (when it lands) has zero design
work to do.  Pattern verified against Thetis Setup → Audio → VAC1
tab (operator-provided screenshots 2026-05-30 + agent source audit
of `setup.designer.cs:28901-28935`, `console.cs:13464-13530`,
`ChannelMaster/ivac.c:1-200`).

**Architecture (Thetis-faithful):**

- ONE feature, ONE toggle (Settings → Audio → VAC1 tab):
  `enable_vac1` checkbox — when ON, the VAC1 ring-buffer audio
  feeds the TX chain INSTEAD of the AK4951 mic input.  Operator-
  driven, mode-independent.
- WDSP `rmatchV` (cffi) bridges PC sample rate ↔ 48 kHz wire
  rate, both directions (mic-INPUT VAC→TX, RX-OUTPUT TX→VAC).
- Used for digital-mode software (WSJT-X / FLDigi / DM780 /
  fldigi / JS8 / etc.) AND for standard-HL2 operators who lack
  a codec mic and route a PC USB mic through VAC.  ONE feature,
  TWO use cases.

**Settings → Audio → VAC1 panel layout** (mirrors Thetis exactly,
operator-screenshot-template):

| Control | Type | Default | Maps to |
|---------|------|---------|---------|
| **Enable VAC 1** | Toggle | OFF | Master enable for the VAC bridge |
| **Driver** | Dropdown (MME / WASAPI / WDM-KS / DirectSound / ASIO) | system default | Audio backend select |
| **Input** | Dropdown (Line 1, Line 2, …) | system default | PC audio device the digital app *outputs* to |
| **Output** | Dropdown | system default | PC audio device that *receives* RX audio from Lyra |
| **Buffer Size** | Dropdown (256 / 512 / 1024 / 2048 / 4096) | 2048 | rmatchV ring depth |
| **Sample Rate** | Dropdown (44100 / 48000 / 96000 / 192000) | 48000 | PC-side rate; rmatchV resamples |
| **Mono/Stereo** | Dropdown (Mono / Stereo) | Stereo | PC-side channel count |
| **Gain RX (dB)** | Spinbox −60..+20 | 0 | Pre-VAC RX-audio scale (TX→VAC path) |
| **Gain TX (dB)** | Spinbox −60..+20 | 0 | Post-VAC TX-audio scale (VAC→TX path) |
| **Direct I/Q → Output to VAC** | Toggle | OFF | Send raw IQ instead of demodulated audio (advanced, for IQ recording) |
| **Buffer Latency RingBuffer In/Out (ms)** | Spinbox 1..200 | 20/20 | rmatchV PI loop targets |
| **Buffer Latency PortAudio In/Out (ms)** | Spinbox 1..500 | 120/120 | Host-API buffer targets |

**Auto-enable group** (the operator-flagged checkbox + the locked
v2.1 anchor):

| Control | Type | Default | Behavior |
|---------|------|---------|----------|
| **Enable for Digital modes, Disable for all others** | Toggle | **OFF** (operator opt-in) | On entry to DIGU/DIGL/DRM: if this is checked AND VAC1 is currently DISABLED, auto-enable VAC1 + remember it was auto-enabled.  On exit to a non-digital mode: if VAC1 was auto-enabled by this code path, auto-disable it.  Operator's manual ON/OFF of VAC1 is preserved across the round-trip (never overwritten if they manually flipped it during the digital mode).  Maps to Thetis `chkAudioVACAutoEnable` (`setup.designer.cs:28915-28935`, `console.cs:34930-34979`). |

**VAC-override group** (PTT/SPACE/MOX bypass for phone use over an
active VAC — Thetis-faithful per the operator's screenshot):

| Control | Type | Default | Behavior |
|---------|------|---------|----------|
| **Allow PTT to override/bypass VAC for Phone** | Toggle | ON (Thetis default) | When VAC is ON and operator keys via hardware PTT, route MIC IN (AK4951 codec) instead of VAC1 input for the duration of the keydown.  Mic comes back to VAC on keyup.  Maps to Thetis `chkVACAllowBypass` (`console.cs:26034-26045`, `Audio.cs:540-554`, `ivac.c:682-686`). |
| **Allow SPACE to override/bypass VAC for Phone** | Toggle | OFF | Same but for spacebar keying (UI MOX equivalent). |
| **Allow MOX to override/bypass VAC for Phone** | Toggle | OFF | Same but for Radio.set_mox() programmatic keying (CAT/TCI/automation). |
| **VOX uses MIC instead of VAC** | Toggle | OFF | VOX (voice-activated TX) keys off the AK4951 mic level, not the VAC input.  v0.2.3 alongside the VOX feature itself. |
| **Mute will mute VAC** | Toggle | OFF | Operator's main-output mute also mutes the VAC1 RX→VAC output path. |

**Implementation notes for the future commit:**
- WDSP `rmatchV` cdefs land in the cdef loader (mirror of the
  component-1 §5.2 work for the TX chain).
- One new Lyra-cpp file: `vac1_bridge.{h,cpp}` — owns the
  rmatchV instances + the PortAudio in/out streams + the
  override-VAC FSM.
- Settings UI: new tab `Settings → Audio → VAC1` per the layout
  table above.  All controls wired-and-live (no inert UI per
  CLAUDE.md §15.13/14/15).
- Mode-change hook: extend the `isDigitalMode(mode)` helper
  from §6.7 — on the entry/exit edge, the VAC1 auto-enable
  checkbox is the trigger.

**Out of scope for this v2.1 spec** (capture later as future
work):
- VAC2 second-channel bridge (operator may want it for
  routing audio to a logger separately from a digital-mode
  app; landing alongside VAC1 is reasonable).
- TCI audio_stream as a separate TX input source — this is
  actually a different feature class: TCI's audio_stream is
  a network protocol surface, not a PortAudio bridge.  It
  lands as a SEPARATE setting, parallel to but distinct from
  VAC1.  Same auto-enable-for-digital pattern likely
  appropriate.

### 5.5 TX DSP worker (component 4) — AMENDED for concurrency

Dedicated thread (NOT folded into the RX DSP worker, NOT inlined into
the EP2 writer thread).  Rationale: wire cadence (48 kHz) and RX
cadence (192 kHz blocks) don't match; inlining loads a sensitive
critical path.

The TX DSP worker:
- Owns the `TxChannel` instance.
- Receives mic samples from `Hl2Ep6MicSource` via consumer callback
  on the HL2Stream RX thread.  Pushes them into a `std::deque<float>`
  protected by a `std::mutex` + `std::condition_variable` (the
  **same** primitive class the existing EP2 writer audio queue uses
  at `hl2_stream.cpp:1654-1687` — do NOT introduce a second
  sync-primitive type on the hot path).
- **Producer-paced clock (Lyra-native, matches the RX1 pattern at
  `hl2_stream.cpp:926-928`):** the worker wakes from the
  condition_variable when mic samples arrive (rather than from a
  separate EP2-writer semaphore).  This eliminates a round-trip,
  matches the existing RX worker model, and works for HL2+ AK4951
  where mic content is codec audio.
- **TXA channel block size pinned (CORRECTED 2026-05-30):**
  `OpenChannel` with `in_size=64` (from the reference's
  `cmsetup.c::getbuffsize(48000) = 64 * 48000 / 48000`),
  `dsp_size=4096`.  Earlier this doc said `in_size=126` (per-
  datagram mic count) and `dsp_size=2048` — both diverged from
  the reference and the resulting non-integer-divisor ratio
  caused a WDSP-internal ring-math overrun on first fexchange0
  call (see §5.3 amendment).  The mic-source's per-datagram
  rate (~9-10 samples) doesn't have to match `in_size` directly
  — the ring buffers between producer and `fexchange0` chunks,
  draining in clean 64-sample blocks aligned with the
  reference's universal constant-latency rule.
- On the MOX-off→on edge, the worker applies the MoxEdgeFade
  envelope to the I/Q output (fade-IN over 50 ms — MANDATORY per
  §4.9/§5.6).
- On the MOX-on→off edge, the worker applies fade-OUT, then signals
  completion via a `std::condition_variable` (NOT a Qt-main-thread
  poll on `MoxEdgeFade.is_off()`).  FSM keyup hook waits on the
  cv before clearing the wire MOX bit.  Per CLAUDE.md §15.25 ground
  truth, the wire MOX bit must clear ONLY after the TX I/Q down-ramp
  fully completes (else key click + splatter on every keyup).
- **TUN priority interlock:** when TUN is armed, the worker skips
  `TxChannel.process()` entirely (saves CPU; the EP2 packer §5.7
  injects the TUN DC inline regardless).  No garbage I/Q queued.

**Teardown order (pinned, mirrors §15.21 discipline):**
1. `Hl2Ep6MicSource::stop()` (stop the mic-callback producer)
2. `TxDspWorker::request_stop() + join` (drain the consumer)
3. `TxChannel::stop()` — blocking `SetChannelState(ch, 0, 1)` per
   `wdsp/channel.c:259-297`
4. `TxChannel::close()` / `CloseChannel(ch)`

Skipping this order risks half-drained EP2 buffers + dangling WDSP
callbacks (the same bug class §15.21 fought through on the Python
side).  Documented here so the implementing session pins it on day
one.

**TR sequencing (Thetis-faithful defaults, per `console.cs:30350-30384,
19772, 19807`):**

| Delay | Default | Purpose |
|-------|---------|---------|
| `mox_delay`     | **10 ms** | After TX-channel-off (blocking flush) before clearing the wire MOX bit — lets in-flight TX I/Q drain past the gateware |
| `ptt_out_delay` | **20 ms** | After clearing MOX bit before restarting RX — hardware T/R settle |
| `rf_delay`      | **50 ms** | After MOX bit set, before enabling RF/TX-DSP-on — amp hot-switch protection (operator-tunable for non-amp use; Lyra v2 makes this configurable 1..75 ms with a default of 50) |

These defaults are non-zero by design.  The FSM keyup path
implements: `MoxEdgeFade.fade_out` → wait on cv for is_off →
`SetChannelState(ch, 0, 1)` (blocking) → `mox_delay` (10 ms) →
clear wire MOX bit → `ptt_out_delay` (20 ms) → restart RX DSP.
Keydown: set wire MOX bit → `rf_delay` → start TX DSP / un-mute path.
All delays implemented via Qt single-shot timers — NEVER
`std::this_thread::sleep_for` on the Qt main thread.

### 5.6 MoxEdgeFade (component 5) — LOAD-BEARING

50 ms cos² envelope, applied to TX I/Q at the EP2-writer side just
before packing (NOT inside TxChannel — keeps WDSP TXA chain pure
mic→IQ).  Shared between TUN and SSB paths.

**Per the §4.9 resolution:** WDSP uslew DOES NOT envelope-shape the
SSB mic stream (verified against wdsp/uslew.c + wdsp/TXA.c).
MoxEdgeFade is therefore the **SOLE** envelope for SSB voice TX, and
the fade-in is **MANDATORY**, not optional.  Skipping it ships a hard
step at the EP2 packer on every keydown = audible key click +
adjacent-channel splatter.  The §1 bench acceptance metric "Zero
clicks at keydown/keyup" depends on this fade-in being wired.

Both keydown (fade-in) and keyup (fade-out) responsibilities lie here.
TUN path also uses the same envelope (TUN's inline DC injection is
multiplied by the same cos² envelope coefficient).

### 5.7 EP2 packer extension (component 6) — AMENDED

Edit `hl2_stream.cpp:1622-1638`.  Today:

```cpp
const qint16 txI = emitTone ? kTuneCarrierI : qint16{0};
const qint16 txQ = 0;
```

Becomes (sketch):

```cpp
if (emitTone) {
    txI = kTuneCarrierI;  // TUN keeps the inline DC — unchanged
    txQ = 0;
} else if (txIq_have_block) {
    txI = txIqBlock[i].real * 32767;
    txQ = txIqBlock[i].imag * 32767;
} else if (moxBit) {
    txI = 0;  // No block ready while keyed — silence
    txQ = 0;
} else {
    txI = 0;  // MANDATORY zero-on-no-MOX (mirrors networkproto1.c:1227)
    txQ = 0;
}
```

`txIqBlock` is drained from the **same mutex+cv-protected queue**
used for audio (or a parallel queue with the same primitive — do NOT
introduce a different sync-primitive class on the hot path).  TUN
takes priority over SSB.

**EP2 zero-on-no-MOX is MANDATORY**, not "defence in depth".  Per
`networkproto1.c:1227`: `if (!XmitBit) memset(prn->outIQbufp, 0, …)`.
On HL2 community gateware this is benign (PA bias drops on MOX=0),
but on ANAN-class hardware (v0.4 scope) emitting TX I/Q with no MOX
could glitch an external linear.  Mandatory now to avoid a v0.4
regression hunt.

**Keyup ordering invariant (per §5.5 + CLAUDE.md §15.25):**
1. MoxEdgeFade fade-out reaches zero
2. Clear `inject_tx_iq` (the flag that gates this packer branch)
3. Clear the wire MOX bit

Implementing session: pin this order in the FSM keyup callback.

### 5.8 sip1 TX I/Q tap (v0.3 PureSignal forward-compat)

Per CLAUDE.md §7 (v0.2.0 line item: "§8.2 sip1 TX I/Q tap mandatory
in v0.2"): every TX I/Q sample written to the EP2 packer is also
written to a ring buffer (the `sip1` tap).  In v0.2.0 the ring has
no consumer — it's allocated and filled, that's it.  In v0.3
PureSignal's `calcc` thread reads from it for adaptive predistortion
calibration.

Wiring it now (~20 LOC) lets v0.3 land without re-validating every TX
sub-mode (USB/LSB/CW/AM/FM/digital).  Skipping it forces v0.3 to
introduce the tap retroactively across modes — explicit CLAUDE.md
§6.7 violation ("don't paint into a corner").

Ring size: 1 second @ 48 kHz @ 8 bytes/sample = 384 KB.  Lock-free
SPSC or mutex+cv — pick the same primitive as the audio queue (do
not proliferate primitives).

### 5.8 PTT/MOX FSM extension (HW-PTT, task #42)

Per task #42, fold the foot-switch HW-PTT forwarder into TX-1's FSM
work.  EP6 control bytes already decoded for telemetry; add a
`ptt_in` edge detector + parallel PttSource entry alongside the UI
MOX button.  **MANDATORY opt-in default-OFF gate** (per §15.26 RESOLVED-
CORRECTION lesson — N8SDR's HL2+ ptt_in reads NON-ZERO at RX rest):
Settings checkbox "Use HL2 hardware PTT input" (default OFF).  Edge-
detect not level-driven.  10 ms debounce in the forwarder.  Bench-
verify on operator's specific unit before any production wiring.

---

## 6. Operator decisions (LOCKED 2026-05-29)

| # | Q | LOCKED |
|---|---|--------|
| **6.1** | Mic auto-switch on DIGU/DIGL → TCI? | **NONE — DELETED from TX-1 scope.**  Thetis-faithful: mic is always-on, mode change never auto-switches audio routing (`console.cs:35370-35398` SetRX1Mode → `SetDigiMode(1, dmssTurnOffSettings)` touches DEXP/TXEQ/Leveler/etc.; zero VAC/routing touches).  Future PC-audio-for-TX is a VAC1-style feature, not a mic source — operator-toggled, mode-independent. |
| **6.2** | Bandpass edges operator-tunable? | **YES, default 0–10000 Hz** (operator-curated, supports ESSB).  Settings → TX exposes low/high sliders.  Tooltip notes "set ≥50 Hz to suppress 50/60 Hz mains coupling" but default stays 0 (operator's call).  Persisted per-mode. |
| **6.3** | Leveler ship in TX-1? | **YES, wired with a Settings UI on/off toggle, default OFF.**  Plumb the cdefs + setter; UI toggle in Settings → TX panel.  Testers can flip it; the toggle keeps it from being inert UI.  **v2.1: digital-mode auto-OFF** per §6.7 — operator's chosen ON/OFF is snapshotted on entry to DIGU/DIGL/DRM, forced OFF for the duration, restored on exit to a non-digital mode. |
| **6.4** | PHROT default ON? | **YES, default ON, with a Settings UI on/off toggle.**  Thetis-faithful adoption (~3-4 dB PEP/PAR win flattens speech peaks); operator can disable via Settings → TX.  **v2.1: digital-mode auto-OFF** per §6.7 — operator's chosen ON/OFF is snapshotted on entry to DIGU/DIGL/DRM, forced OFF for the duration, restored on exit. |
| **6.5** | AK4951 I²C mic-route side-channel needed? | **NO.**  Thetis does ZERO AK4951-specific I²C — the HL2 gateware initializes the codec autonomously at FPGA bitstream init.  Operator confirms his HL2+ has worked with Thetis + AK4951 mic for years.  Mic routing is via standard EP2 C&C cases 10 & 11 (`mic_boost`, `line_in`, `mic_bias`, `mic_ptt`, `line_in_gain`).  The earlier "no audio on bytes 24-25" bench result was a Lyra-cpp parser bug — bytes 24-25 are not the mic-sample offset at any nddc value (it's the §3.2 nddc-aware formula).  Implementing session fixes the parser + verifies the C&C bytes per §5.4. |
| **6.6** | HW-PTT foot-switch fold-in? | **YES, default OFF, operator opt-in toggle in Settings → TX.**  Edge-detect + 10 ms debounce in the forwarder.  Operator uses a foot switch on his station and will enable it; default-OFF protects everyone else from the `ff5f128` regression class (HW-PTT-in mis-read at RX rest → phantom-TX).  Bench-verify operator's HL2+ ptt_in rest-state before production wiring (still applies). |

**No remaining open questions for the implementing session.  Design
v2.1 is fully locked.**

### 6.7 Digital-mode TX-DSP auto-bypass (v2.1, locked 2026-05-30)

Operator-flagged via Setup-screenshot evidence; Thetis source-
verified at `console.cs:44094-44105` (`dmssTurnOffSettings` switch
case) + `console.cs:35370-35398` (`SetRX1DSPMode` digital-mode
entry/exit).

**The Thetis behavior (mirror this exactly):** when the operator
selects a digital mode (DIGU/DIGL/DRM) on RX1, Thetis automatically
forces OFF every speech-shaping TX-DSP block — because digital
signal protocols (WSJT-X, FT8, FT4, PSK, RTTY, etc.) need clean,
linear audio passing through the modulator chain without speech-
optimised processing destroying signal integrity.  The auto-OFF is
**hard-wired** (no operator toggle to disable the auto-bypass —
that's how Thetis ships).  The operator's pre-digital ON/OFF for
each block is snapshotted on entry (`dmssStore`,
`console.cs:44106-44117`) and restored on exit to a non-digital
mode (`dmssRecall`, `console.cs:44118-44136`).

**Blocks Thetis auto-OFFs on digital entry:**
- DEXP (Noise Gate)
- TX EQ
- TX Leveler
- CPDR (Compander)
- RX EQ
- ANF (auto notch)
- NR (all four flavors)
- CESSB (Controlled Envelope SSB)
- CFC (Continuous Frequency Compression — 5-band speech processor)
- Phase Rotator (PHROT)

**Blocks NOT in Thetis's bypass list (stay as-operator-set):**
- ALC (always-on splatter protection)
- bp0 (always-on SSB sideband selector)

**Lyra-cpp implementation rule (LOCKED):** every TX-DSP block with
an operator-facing ON/OFF that affects voice-modulator linearity
gets the same `dmssTurnOffSettings`-equivalent treatment:

1. **Snapshot** the operator's chosen ON/OFF state on entering
   DIGU/DIGL/DRM.
2. **Force OFF** for the duration of the digital mode.
3. **Restore** the snapshot on exit to a non-digital mode.
4. **Hard-wired** — no Settings toggle to disable the auto-bypass.
   The operator's per-block ON/OFF is preserved across the digital
   mode round-trip; they don't lose their pre-digital configuration.
5. The auto-OFF runs ONCE per mode-change edge (entry / exit), not
   per-block on every keydown — the Settings ON/OFF UI reads the
   operator's stored preference at all times; only the live
   `SetTXA*(ch, 0)` calls flip on the mode edge.

**Lyra-cpp blocks subject to this rule** (current + future):

| Block | Lands in | Setter |
|-------|----------|--------|
| **PHROT** | TX-1 (this doc, §6.4) | `SetTXAPHROTRun` |
| **Leveler** | TX-1 (this doc, §6.3) | `SetTXALevelerSt` |
| **TX EQ** (3 or 5-band parametric per §15.19) | v0.2.1 | TBD (Lyra-native pre-processor) |
| **Combinator** (multiband compressor) | v0.2.1 | TBD (Lyra-native pre-processor) |
| **Tube Plating** | v0.2.1 | TBD (Lyra-native pre-processor) |
| **Formant boost** | v0.2.1 | TBD (Lyra-native pre-processor) |
| **Sibilance / consonant emphasis** | v0.2.1 | TBD (Lyra-native pre-processor) |
| **DX cut-through** | v0.2.1 | TBD (Lyra-native pre-processor) |
| **De-esser** | v0.2.1 | TBD (Lyra-native pre-processor) |
| **Auto-AGC on mic** | v0.2.1 | TBD (Lyra-native pre-processor) |
| **CESSB Overshoot Control** | v0.2.1 (CLAUDE.md §4.1) | `SetTXAosctrlRun` |
| **CFC** (5-band speech processor) | NOT planned (per §15.19 operator preference — Combinator replaces) | n/a |
| **Compander / CPDR** | NOT planned (per §15.19 operator preference) | n/a |
| **DEXP / Noise Gate** | v0.2.2 (§15.19) | `SetTXAamsq*` exposure |
| **Phase Rotator (PHROT)** | TX-1 (this doc, §6.4) | `SetTXAPHROTRun` |
| **ALC** | TX-1 (always-on per §4.4 + Thetis list) | NOT bypassed — splatter protection |
| **bp0** (SSB sideband selector) | TX-1 (always-on per §4.2 + Thetis) | NOT bypassed — required for SSB modulation |

**Mode-change hook (architectural):** the existing mode setter (in
`Radio` / `WdspEngine`) gains a `bool isDigitalMode(mode)` helper
+ on the entry edge to digital iterates every block in the table
above calling its setter with the OFF value, snapshotting the
prior; on the exit edge restores the snapshot.  The
`PrevDigitalMode` snapshot dataclass mirrors Thetis's
`DigiMode rx1dm` struct (`console.cs:54723-54733`).

**v2.1 implementing-session note:** this is a SEPARATE commit
ladder from the core TX-1 component work (2a → 2b → 2c → 2d).
It lands as the LAST step of TX-1 (after component 6 EP2 packer +
the §9 Settings UI), because it needs every TX-DSP block's setter
already wired so the snapshot loop has something to iterate.  Pin
this in the implementing-session plan in §8.

---

## 7. Red-team round — completed 2026-05-29 (3 lenses, converged)

Three independent senior agents reviewed this design + dossiers with
file:line evidence against Thetis source.  All three converged
CONFIRM-WITH-AMENDMENTS in one round.  One BLOCKS-SHIP correctness
issue (§4.9 uslew dossier — now fixed); all other findings are
pin-the-mechanism amendments, all folded into v2 above.

**Verification asks both decisively answered:**
- §4.9 — uslew does NOT envelope SSB mic (only TUN/gen1).  Proof
  cited in §4.9.  MoxEdgeFade is the sole envelope, fade-in
  load-bearing.
- C&C round-robin modulus — confirmed 19 cases (0..18) at
  `networkproto1.c:948-1176`.  Lyra-cpp `% 19` is byte-exact.

**Agent A (concurrency)** — CONFIRM-WITH-AMENDMENTS: drop SPSC ring
in favour of the existing mutex+cv (single primitive class on the hot
path); producer-paced worker from mic-source callback (not
EP2-semaphore round-trip); pin `fexchange0` block size to `in_size
=126`; PTT teardown via condition_variable not main-thread poll; pin
explicit teardown order.

**Agent B (safety / §15.23-trap / wire-faithfulness)** —
CONFIRM-WITH-AMENDMENTS + 1 BLOCKS-SHIP: §4.9 uslew dossier inverted
(now corrected); init order push bandpass edges before
`SetTXAMode`; add `mox_delay=10ms` + `ptt_out_delay=20ms`; EP2
zero-on-no-MOX mandatory; fix the `SetTXAPanelGain1` default comment
(WDSP default is 1.0, not 4.0); add explicit "no DC/IQ calibration in
v0.2.0" status note.

**Agent C (scope / methodology / forward-compat)** —
CONFIRM-WITH-AMENDMENTS: drop `TciAudioMicSource` and
`PcSoundcardMicSource` from TX-1 (Thetis-faithful: mic is HL2 codec
only); add per-component bench gates (6); add the sip1 TX I/Q tap
for v0.3 PureSignal forward-compat; defer leveler to v0.2.1 was
operator-overridden (ship wired with UI toggle per §6.3).

---

## 8. Status

**DESIGN v2.1.1 LOCKED 2026-05-30.  Ready for the implementing session.**

3-lens red-team round complete (§7) for v2.  v2.1 adds the Thetis
digital-mode TX-DSP auto-bypass (§6.7) + future VAC1 spec lock
(§5.4.1) + §9 reworked against the operator's Setup screenshots.
v2.1.1 corrects the TX Meter picker entry (operator caught it —
MeterModel already ships the picker; only the ALC/MIC/COMP source
values are pending TX DSP per §9.3.1).  All §6 operator decisions
answered.  No remaining design questions.

**Explicit status notes for the implementing session:**
- **No DC/IQ calibration in v0.2.0.**  Carrier suppression on the bench
  (§1 metric "Carrier (DC) ≤-60 dBFS") is hardware-limited by the HL2
  AD9866 DC trim, not software.  WDSP TXA's ALC + bp0 carrier
  suppression at create-time are ≥60 dB by design.  v0.3 ships
  `iqc.c`/`calcc.c` cffi for PureSignal-grade numbers.
- **HL2+ AK4951 mic = `Hl2Ep6MicSource` only** (single class).  Future
  PC-audio-for-TX is a VAC1-style feature, NOT another mic source.
- **Phase-3-EXIT kill-test (CLAUDE.md §15.20/§15.24-C class) BLOCKS
  any real-antenna keying.**  Bench: `taskkill /F` mid-TX into a
  dummy load, scope/confirm HL2 PA bias drops within the gateware
  watchdog window.  Until that bench passes, treat the watchdog as
  assumption-pending, NOT established fact.

**Implementing-session plan:**
1. Fix the EP6 mic byte parser per §3.2 (nddc-aware formula).
2. Verify EP2 C&C cases 10 & 11 byte composition per §5.4 (`line_in=0`
   default, expose mic_boost / mic_bias / line_in_gain to Settings).
3. **First bench gate**: with the parser fix + C&C correction, verify
   operator's AK4951 mic comes through EP6 (per-component bench gate
   #3 / §5.1).  This is the Q6.5 verification.
4. Implement components 1-6 per §5 with their per-component bench
   gates (§5.1).
5. Wire Settings → TX panel per §9 below.
6. **Wire the §6.7 digital-mode auto-bypass** (new in v2.1) — once
   every TX-DSP block's setter is alive (PHROT + Leveler today; EQ
   / Combinator / etc. in v0.2.1), implement the `isDigitalMode()`
   helper + snapshot/restore on mode-change edges per §6.7.  LAST
   step of TX-1 because it depends on all the block setters
   existing.
7. End-to-end Tier-A unit bench (§1).
8. Hardware bench (§1).
9. Phase-3-EXIT kill-test (§1).
10. FAIL anywhere → revert that step, diagnose with captured data,
   no guess-fix.

---

## 9. Settings → TX panel UI spec

**v2.1: layout template = Thetis Setup → Transmit tab** (operator-
provided screenshots 2026-05-30).  Lyra-cpp's Settings → TX panel
mirrors the Thetis visual organisation so operators bringing prior
Thetis muscle memory find the controls where they expect.

All wired-and-live (no-inert-UI per CLAUDE.md §15.13/14/15
discipline).  Each control plumbs to a real setter that takes effect
on apply.

### 9.1 Mic section (mirrors Thetis Setup → Transmit → Mic)

| Control | Type | Default | Persists | Setter target |
|---------|------|---------|----------|---------------|
| **Source** | Radio (Mic In / Line In) | Mic In (`line_in=0`) | yes | EP2 C&C case 10 C2 bit 1 |
| **Gain Max (dB)** | Spinbox −90..+40 | +40 (Thetis default) | yes | Operator-facing scale ceiling for mic-gain slider |
| **Gain Min (dB)** | Spinbox −90..+40 | −90 (Thetis default) | yes | Operator-facing scale floor for mic-gain slider |
| **20 dB Mic Boost** | Toggle | OFF | yes | EP2 C&C case 10 C2 bit 0 (`mic_boost`) |
| **Mic Bias** | Toggle (electret) | OFF | yes | EP2 C&C case 11 C1 bit (per `networkproto1.c:1091-1103`) |
| **Line-in gain** | Slider 0..31 | 0 | yes | EP2 C&C case 11 C2 |

### 9.2 Transmit Filter (bandpass) section

| Control | Type | Default | Persists | Setter target |
|---------|------|---------|----------|---------------|
| **Low (Hz)** | Slider/Spinbox 0..10000 | 0 | per-mode | `TxChannel::setBandpass(low, high)` |
| **High (Hz)** | Slider/Spinbox 0..10000 | 10000 | per-mode | `TxChannel::setBandpass(low, high)` |

Bandpass tooltip: "Set lower edge ≥50 Hz to suppress 50/60 Hz mains
coupling.  Higher edges above 4000 Hz enable ESSB audio width — verify
your TX BW conforms to your bandplan."

### 9.3 TX-DSP toggles section

| Control | Type | Default | Persists | Setter target | Digital-mode (§6.7) |
|---------|------|---------|----------|---------------|---------------------|
| **PHROT** | Toggle (on/off) | ON | per-mode? — TBD by impl | `SetTXAPHROTRun(ch, 0/1)` | **Auto-OFF** on DIGU/DIGL/DRM |
| **Leveler** | Toggle (on/off) | OFF | yes | `SetTXALevelerSt(ch, 0/1)` | **Auto-OFF** on DIGU/DIGL/DRM |
| **ALC** | (read-only indicator; always ON) | ON | hard-wired | `SetTXAALCSt(ch, 1)` at init | **NOT bypassed** — splatter protection |

§6.7 auto-bypass behavior: per the locked v2.1 rule, when the
operator selects DIGU/DIGL/DRM, both PHROT and Leveler are auto-OFF
for the duration of the digital mode; their pre-digital ON/OFF
state is snapshotted on entry and restored on exit.  Operator-
visible: the Settings → TX toggle for each retains the operator's
stored preference at all times (UI doesn't flip); only the live
WDSP run-state is overridden.

### 9.3.1 TX/RX Meter source picker (EXISTS today, NOT new work)

**v2.1.1 correction:** the v2.1 §9.5 entry "TX Meter dropdown
— v0.2.x" was wrong.  The MeterModel infrastructure already
ships on the lyra-cpp `main` branch: `MeterModel::Source` enum
(`src/metermodel.h:45-56`) with 9 sources — `RX_SMETER`, `PWR`,
`SWR`, `PA_CURRENT`, `PA_VOLTS`, `TEMP`, `ALC`, `MIC`, `COMP` —
plus separate per-state preferences (`rxSource_` default
`RX_SMETER`, `txSource_` default `PWR`), MOX-edge automatic
source-swap via the `HL2Stream::moxActiveChanged` signal
(`src/metermodel.cpp:170, 130, 622`), and a click-to-cycle UI
on the meter face (`src/qml/MeterPanel.qml:97-119`).
Secondary digital readouts (`txSecondary` + `txSecondary2`,
`metermodel.h:109,118`) round out the §15.25-style 3-line
TX-meter layout.

What's PENDING for TX-1:
- The click-cycle list in `MeterPanel.qml:106` is currently
  `[0, 1, 2]` (RX_SMETER, PWR, SWR).  `PA_CURRENT` / `PA_VOLTS`
  / `TEMP` ship in the enum but aren't in the cycle list yet
  (likely because the PA-current banner shipped first and the
  picker shipping order didn't expand).  Expanding the list
  is a one-line change once the operator confirms which order
  they want.
- `ALC` / `MIC` / `COMP` are wired in the enum but blocked on
  TX DSP existing.  Component 2c (operator setters) will wire
  the `GetTXAMeter(channel, meter_type)` cffi call (already
  cdef'd in component 1, `wdsp_native.h:283`) to populate
  these.  Once 2c lands the click-cycle list extends to
  include them.

So the TX-Meter picker is NOT TX-1 work — it's already there.
What TX-1 adds is the values flowing into the existing 3 TX-DSP
sources (ALC, MIC, COMP) via `GetTXAMeter`.

### 9.4 PTT section

| Control | Type | Default | Persists | Setter target |
|---------|------|---------|----------|---------------|
| **HW-PTT input** | Toggle (on/off) | OFF (operator opt-in per §6.6) | yes | gate the EP6 `ptt_in` consumer |

HW-PTT tooltip: "Forwards the HL2 EP6 foot-switch PTT input as a Lyra
PTT source.  Default OFF.  Some HL2 units (incl. the AK4951 variant
in some firmware revs) read non-zero at RX rest — enabling
unconditionally on those units causes spurious TX.  Bench-verify your
unit's ptt_in rest behavior before enabling."

### 9.5 Future sections (reserved, NOT in TX-1 scope)

The Thetis Setup → Transmit tab also has these sections that Lyra-
cpp will need eventually.  Reserved here so the implementing session
knows the layout slot they belong in:

- **Profiles** (top-left in Thetis): TX profile picker per §15.19
  (CLAUDE.md) — v0.2.3.
- **Tune** (Use Drive Slider / Use Tune Slider / Use Fixed Drive
  radio + the optional fixed-drive value): the tune-power
  selection mode (operator-curated whether TUN uses the live
  drive slider, a separate tune-only slider, or a fixed value) —
  v0.2.x.  (Note: the **TX Meter dropdown** from Thetis's Tune
  section is NOT here — it already exists in lyra-cpp; see §9.3.1.)
- **Monitor** (TX AF + "Ignore Master AF Change"): hot-mic monitor
  level + AF-tracking option — v0.2.3 per §15.19.
- **AM Carrier Level**: AM modulator carrier injection level —
  v0.2.2 (CW + AM + FM).
- **External TX Inhibit** ("Update with TX Inhibit state" /
  "Reversed logic"): operator-facing controls for an external
  Inhibit input that gates TX (relay-protect on external SDR/scope
  hardware) — v0.2.x.
- **Speech Processor → CESSB Overshoot Control**: TX CESSB toggle
  — v0.2.1 (`SetTXAosctrlRun`).
- **Pulsed Tune** (Pulse Freq / Duty / Ramp / Window readout):
  pulsed TX-tune mode for amp tuning — v0.2.x.
- **Profile-restore checkboxes** ("Restore VAC1/VAC2 device details
  from TX Profile", "Restore PA profile from TX Profile",
  "Auto Save TX Profile…", "Highlight TX Profile Save Items"):
  TX profile interaction policy — v0.2.3 alongside the profile
  picker itself.

The §5.4.1 VAC1 panel ships under Settings → Audio → VAC1 (separate
tab from Settings → TX), per Thetis layout convention.


## §10. v0.2.2 cycle — Meter / Help / Credits polish (2026-06-01)

Today's session shipped five distinct items.  Architectural pieces
worth recording for future readers below.

### §10.1 Task #36 — Hardware PTT input forwarder (default-OFF)

**Locked posture:** operator opt-in, default OFF.  Per the §10
Q#1 finding from the Python predecessor (`ff5f128` lineage), EP6
status C0 bit 0 (`ptt_in`) is NOT a clean 0 at RX rest on every
HL2 gateware revision — N8SDR's HL2+/AK4951 unit empirically
carries a non-zero level at rest, so an always-on forwarder
mis-reads it as a foot-switch press and produces a phantom-TX
surge the moment Lyra opens.

Implementation surfaces:
- `Prefs.hwPttEnabled` (`tx/hw_ptt_enabled`, default false).
- `HL2Stream::hwPttEnabled_` atomic mirror + `lastPttIn_`
  edge-detect memory (RX-worker-thread owned).
- `rxWorkerLoop` decodes `ptt_in = u[11] & 0x01` per datagram
  (frame-0 C0 bit 0 — gateware-pure regardless of address
  rotation / I²C-readback bit).  Pure-decode when gated off
  (still updates `lastPttIn_` so a future enable doesn't fire
  on stale state); edge-driven `QueuedConnection` dispatch to
  `requestMox()` when gated on.
- Settings → Hardware → Transmit checkbox with the
  bench-verify-first safety tooltip.

Operator-validated working on N8SDR's bench foot switch.

### §10.2 Task #35 — TX Multimeter fillers + RX/TX picker split

**Three new meter sources** wired off the EP6-decoded HL2
telemetry slots (no new wire surface, no new DSP):

- **ID — PA Current** — 0..3 A, 1.8 A verified-reference
  full-tune anchor tick, 2.5 A danger.
- **VDD — PA Volts** — 0..16 V, 12 V nominal HL2-supply tick,
  14 V danger.
- **TEMP** — 0..80 °C, 25 °C idle tick, 60 °C danger.

Slow IIR ballistic (`kTelSmooth=0.30`) — physical-quantity
readings shouldn't chase sensor noise.

**Per-MOX-state source-type enforcement (the picker-split
correction):** the RX dropdown is RX-signal-only (`RX_SMETER`
for now; PA telemetry available via the HL2 banner chip + the
Vertical Ladder style).  The TX dropdown is the TX-chain /
forward-power / HL2-telemetry set.  Enforced at the model
layer in `setRxSource()` / `setTxSource()` with explicit
switch statements — no future code path can assign a TX-chain
source to the RX-state slot.  Stale legacy QSettings values
normalize via ctor autoload clamps.

ALC / MIC / COMP show in the TX picker but are disabled with
"coming v0.2.1" caveats — they unlock when the WDSP TXA-meter
readout wires in alongside the compressor (Tasks #50 / #51).

### §10.3 Task #57 — PWR meter ballistic picker (PEP / Peak / Avg)

Three named ballistics, operator picks per use-case:

| Mode | Mechanism | Default hold | Use case |
|---|---|---|---|
| **PEP** | Sliding-window MAX, fixed 10 samples × 50 ms | 500 ms | Contest peak-watching; each speech burst kicks the bar up and drops between syllables |
| **Peak** *(default)* | Sliding-window MAX, operator-tunable hold | 3000 ms (60 samples) | General-purpose; peaks park long enough to read off the digital face |
| **Avg** | IIR smoother | ~200 ms τ | Sustained-tone gain-structure work / ragchew; calm needle that doesn't chase peaks |

**Architectural decision — why Peak default is 3000 ms vs the
verified reference's 500 ms:** the reference's analog-style
needle widget has rendered spring + inertia damping, so a
500 ms hold + the needle's settling animation reads as
continuous flowing motion.  Lyra's digital bar / Plasma arc
renderers have no inherent damping — a digital bar held at
4.6 W for 500 ms then dropping reads as a *flash*
(operator bench 2026-05-31 PM).  The 3000 ms default matches
the typical Bird/Daiwa PEAK ballistic where a peak parks for
several seconds so the operator can read it at leisure.
Same underlying MAX-detector math, same sub-50-ms attack —
only the post-attack hold is tuned for the digital renderer's
visual character.  Operators who want the reference's tighter
feel can pick PEP (fixed 500 ms) or dial *Peak* down via the
spin box.

The outer peak pip + max-hold marker remain driven off
`level_` in ALL modes — Avg still surfaces recent peaks via
those auxiliary indicators while the inner needle tracks
the running average.

### §10.4 Help dialog 2-column redesign

`QSplitter(QTreeWidget TOC, QTextBrowser content)` replacing
the prior single-pane layout.  TOC tree built at load time
from parsed markdown headings (26 H2 + 38 H3 in the current
doc).  Click → `QTextBrowser::find(<heading-text>)` — the
proven mechanism (`showTopic` for per-panel `?` badges already
used it).  Bypasses Qt's `setMarkdown()` → `QTextDocument`
anchor-name path which doesn't generate slug-named anchors
from headings (operator-reported defect after the first
`<a name>` injection attempt was silently swallowed by the
conversion).

In-document body cross-reference links resolve via a
`slugToHeading_` map populated alongside the tree.  Window
geometry + splitter state persisted to QSettings.
Min/max/close in the title bar (`Qt::WindowMinMaxButtonsHint`
— `QDialog` defaults to fixed-frame on Windows, this was the
missing piece).  TOC items styled as hyperlinks (Lyra-cyan
text, hover brighten, pointing-hand cursor) so the
click-affordance is obvious.

### §10.5 Task #58 — Credits and References

`Help → About Lyra…` dialog gains an "Inspiration and
references" + "Licensed components" block listing every
project consulted during the Lyra build (Thetis SDR, openHPSDR
Protocol 1 spec, PowerSDR, HermesLite 2 wiki + ak4951v4
gateware RTL, pihpsdr, Quisk, linHPSDR, EESDR V3, Behringer
X-Air mixer series, SparkSDR) plus WDSP / TCI / FFTW
licensed-components attributions.  Full long-form version
lands in `USER_GUIDE.md` as a new `## Credits and References`
section.

**No-attribution rule re-affirmed:** shipped code, comments,
commits, and operator-visible UI strings keep reference names
out.  This Credits section is the single operator-facing place
that names them, openly and once.  Provenance details (file,
line, decision) continue to live in `docs/architecture/` and
the project memory.

### §10.6 Closed for v0.2.2

Tasks #35, #36, #57, #58 + the helpdialog 2-column refactor.
Pending v0.2.x: #33 (Mic Source selector), #44 (Panadapter
TX-state rescale), #45 (PWR per-band cal), #47 (QML null-safety
on shutdown), #49 (TX Profile Manager), #52 (Plate Reverb —
ready, W5UDX + N8SDR presets locked).  Blocked on operator
screenshots: #50 (5-band parametric EQ), #51 (5-band
Combinator).
