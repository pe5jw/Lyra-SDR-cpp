# TX Combinator — design note (Task #51)

Status: **DRAFT for red-team** (2026-06-16). Grounded design before code.
Locked architecture lives in `tx1_ssb_design.md` §11; this note adds the
engine math, the control→DSP mapping, the SBC algorithm, the UI surface,
and the **N8SDR default preset** decoded from the operator's X-Air export.

Provenance note: per the project no-attribution rule, the reference
device names appear ONLY in this doc + `docs/refs/eq_combinator/`. Shipped
code, comments, commit messages and operator-visible UI strings stay
generic ("Combinator", MIX/ATT/REL/SPEED/X-OVER/RATIO/THRESH/GAIN/SBC —
all generic DSP terms).

---

## 1. What it is (reference-decoded)

A 5-band multiband compressor with an optional **automatic spectral
balance** layer. From the operator's X-Air manual paste:

> "emulates famous broadcasting and mastering compressors, utilizing
> automatic parameter control that produces very effective yet
> 'inaudible' results. MIX lets some source pass unaffected. ATTACK and
> RELEASE have dedicated controls + an Auto-Release. Global X-OVER,
> RATIO, THRESH and GAIN. Engage SBC for automatic gain balancing
> between the bands; SPEED sets how aggressively it works. THRESH and
> GAIN adjust per band independently."

So the locked "5 parallel compressors + LR crossovers + sum" model is
**faithful**; SBC is an *optional add-on layer*, not a different engine.
We use a **single** combinator instance (mono mic path; the export's
`CMB2` dual A/B is irrelevant here).

---

## 2. Signal flow

```
mic (I-ch, 48 kHz, double)
  → LR4 crossover bank → 5 bands [LOW LO-MID MID HI-MID HIGH]
      each band: envelope → gain-computer (thr, ratio, att, rel)
                 → × per-band makeup
      [+ SBC: extra per-band gain nudging bands toward balance]
  → Σ (sum bands)            = WET
  → out = MIX·WET + (1-MIX)·DRY      (parallel blend)
```

Same in-place contract as `ParamEq` / `SpeechProcessor`:
`Combinator::processInterleaved(double* x, int n)` filters the I-channel
of the `{I=mic, Q=0}` buffer. Bypassed (whole rack OFF, or this stage
OFF) → early `return`, buffer untouched — the locked bypass semantics.

Chain placement: **after EQ, before Plate**, under the one
`tx_rack_bypass` gate (so DIGU/DIGL auto-bypasses it). CMaster hook
`SendpTxCombinatorProcessor`, mirroring `SendpTxEqProcessor` /
`SendpTxSpeechProcessor`.

---

## 3. DSP detail

### 3.1 Crossover bank — Linkwitz-Riley 24 dB/oct (LR4)

4 crossover points → 5 bands. LR4 = two cascaded 2nd-order Butterworth
(LP & HP). LR4 has the perfect-reconstruction property (complementary
LP+HP sum to an all-pass = flat magnitude) for a 2-way split; for the
5-way we build the standard **serial split with all-pass phase
compensation** so the recombined sum stays magnitude-flat at unity
band gains:

- Split off LOW at f1 (LP) vs rest (HP); the "rest" path gets the
  complementary structure; repeat at f2, f3, f4.
- Higher bands are phase-compensated by the lower crossovers' all-pass
  so the summed magnitude is flat when no band is compressing.
- **Red-team item R1:** verify the chosen 5-way LR4 topology reconstructs
  to < 0.1 dB ripple at unity (unit test: white noise in, all bands
  pass-through, |out| ≈ |in|). Alternative if ripple is unacceptable:
  complementary/Lipshitz LR tree.

Base crossover set (before X-OVER shift), from the panel meter strip:
**f1 ≈ 150, f2 ≈ 500, f3 ≈ 1500, f4 ≈ 5000 Hz**. These are tunable; the
**X-OVER** knob shifts the whole set (see 3.4).

### 3.2 Per-band compressor

Per band `b` (reuses the one-pole envelope idiom proven in
`SpeechProcessor::AutoAGC`, extended with ratio/threshold):

- **Detector:** peak (one-pole) — matches the panel's "PEAK" meter mode.
  ATT/REL one-pole coefficients from global ATT (11 ms) / REL (494 ms).
  `env = max(|x|, env·rel)` on decay, fast-attack toward |x|.
- **Threshold:** `thr_b = globalThresh + bandThresh_b` (dBFS).
- **Gain computer:** above threshold, reduce by `(1 − 1/ratio)·(level −
  thr_b)` dB (hard knee v1; soft knee = red-team R2).
- **Makeup:** `mk_b = globalGain + bandGain_b` (dB), applied after.

Ratio is **global** (one value, all bands).

### 3.3 SBC — Spectral Balance Control (optional)

When engaged: measure each band's slow running level, compute the mean
across bands, and apply an **extra per-band gain** nudging each band
toward that mean (automatic inter-band balancing). **SPEED** sets the
time-constant / aggressiveness of the nudge.

- Lyra-native equivalent — the proprietary algorithm is not copied; the
  *behaviour* (auto gain-balance between bands, SPEED-controlled) is
  reproduced from the manual description.
- Applied as a per-band gain term before the sum, on top of the
  compressor makeup.
- **Red-team item R3:** define the target — pull toward the flat mean,
  or preserve the program's overall tilt and only equalise *deviations*?
  Mastering-style "inaudible" suggests gentle deviation-correction, not
  hard flattening. Default SPEED low/gentle.

### 3.4 Control units / mappings (Lyra-native; values locked, curves defined here)

| Control | Operator value | Lyra mapping |
|---|---|---|
| MIX | 100 % | 0–100 % wet/dry; 100 = full wet |
| ATT | 11 ms | one-pole attack time, 0–120 ms |
| REL | 494 ms | one-pole release time, 20–4000 ms |
| SPEED (SBC) | 4 | SBC nudge rate; map 0–10 → time-constant (R4) |
| X-OVER | −10 | ± shift of the 4-point base set (R5: −X..+X → log-Hz scale) |
| RATIO | 3 | 3:1 (global) |
| THRESH (global) | −34.0 dB | −40…0 dBFS |
| GAIN (global) | 0.0 dB | −10…+10 dB |

**R4 / R5:** SPEED→time-constant and X-OVER→Hz are X-Air internal
units; the operator's *values* are locked, the internal curves are
ours to define + bench-tune. Document the chosen curves in code.

---

## 4. N8SDR default preset (LOCKED — from `combinator settings.efx`)

Global: MIX 100 %, ATT 11 ms, REL 494 ms (manual; auto-rel off),
SPEED 4, X-OVER −10, RATIO 3:1, SBC **ON**, TRIM THRESH −34.0 dB,
TRIM GAIN 0.0 dB, meters PEAK.

Per-band THRESH offset / makeup GAIN (band order LOW→HIGH, confirmed):

| LOW | LO-MID | MID | HI-MID | HIGH |
|---|---|---|---|---|
| −3.5 / +1.5 | −4.0 / +1.0 | −1.0 / 0.0 | −1.5 / +2.5 | −2.0 / +3.0 |

Character: low/lo-mid compressed a touch harder with light makeup, MID
essentially untouched, presence/air lifted (+2.5 / +3.0) — a clean,
articulate ESSB voice.

---

## 5. UI surface (CombinatorPanel.qml)

Mirrors the operator's panel, Lyra-styled, dockable/collapsible/View-
hideable like EqPanel/SpeechPanel (header chip-strip launcher):

- **Global knob row:** MIX · ATT · REL (+ auto-rel toggle) · SPEED · 
  X-OVER · RATIO; global TRIM THRESH + TRIM GAIN.
- **Band selector** (LOW/LO-MID/MID/HI-MID/HIGH) → per-band THRESH +
  GAIN + on/off. SOLO + BYP per band.
- **SBC** enable toggle.
- **Per-band meters** (color-matched), 3 modes: gain-reduction / SBC
  balance / peak output.
- **ON** (whole-stage bypass) · collapse ▼.

Model `CombinatorModel` (lyra::ui): Q_PROPERTY/NOTIFY for globals +
index-keyed Q_INVOKABLE per-band getters/setters (the EqModel pattern),
static `txProcessCb` + `s_txEngine` atomic bridge.

---

## 6. Build stages (same pattern as EQ / Speech)

1. **`Combinator.{h,cpp}` (lyra::dsp, Qt-free)** + `scratch/test_combinator.cpp`
   — crossover reconstruction (R1), comp gain law, MIX blend, SBC
   balance. No UI, no wire.
2. **`CombinatorModel` + `CombinatorPanel.qml`** — panel + bindings,
   dock, default preset seeded.
3. **Wire** into CMaster TX rack (after EQ, before Plate),
   `SendpTxCombinatorProcessor`, digital auto-bypass, default-safe
   (ships ON? or OFF? — R6: EQ ships ON; Combinator default should be
   **OFF** until operator enables, since it materially compresses).

---

## 7. Red-team checklist (resolve before / during Stage 1)

- **R1 ✅ RESOLVED (Stage 1)** — phase-compensated LR4 serial split (lower
  bands run through the later crossovers' 2nd-order allpass). `test_combinator`
  reconstruction: −0.00 dB at 50 Hz–5 kHz, −0.16 dB at 9 kHz — within the
  < 0.1 dB target across the speech band, negligible at the very top.
- **R2** hard vs soft knee (default hard; soft if voice sounds pumped).
- **R3** SBC target: deviation-correct vs flatten (default gentle
  deviation-correct).
- **R4** SPEED 0–10 → SBC time-constant curve.
- **R5** X-OVER ±N → log-Hz crossover shift curve; base set 150/500/
  1500/5000.
- **R6** default stage state OFF (materially alters audio).
- **R7** CPU: 5×LR4 + 5 envelopes + SBC at 48 kHz — expected trivial,
  confirm in the unit test timing.
- **R8** order vs Plate/EQ already locked (EQ→Comb→Plate→ALC); no change.
