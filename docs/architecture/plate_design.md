# TX Plate Reverb — design note (Task #52)

Status: **DRAFT for red-team** (2026-06-16). Grounded design before code.
Locked spec + presets live in `tx1_ssb_design.md` §11 ("DSP2024P Plate
Reverb — operator-supplied presets (LOCKED)"). This note adds the
Schroeder-Moorer topology, the param→DSP mapping, the UI surface, and the
build stages.

Provenance: the reference unit name appears ONLY in this doc + the design
ledger. Shipped code, comments, commits and UI strings stay generic
("Plate", PRE-D / DECAY / DAMP / SIZE / DIFF / DENSITY / BASS / TREB / MIX —
all generic reverb terms).

---

## 1. What it is

A native C++23 **Schroeder-Moorer plate reverb** — the last operator-
toggleable native rack stage. Chain: **EQ → Combinator → Plate → ALC**.
Plate sits BEFORE the always-on ALC limiter so reverb-tail peaks are caught
by the limiter (and don't confuse a future PureSignal predistorter). Reverb
on SSB TX is the ESSB "broadcast air" trick; bandplan-constrained ops just
leave it off (the toggle).

Signal: mono mic on the real (I) channel of `{I=mic, Q=0}` doubles — same
`processInterleaved(double*, int)` contract as ParamEq / SpeechProcessor /
Combinator. Bypassed (stage OFF, or whole rack bypassed in DIGU/DIGL) →
early return, untouched.

```
x ──┬───────────────────────────────────────────────► (1-MIX)·dry
    │                                                        +
    └─ pre-delay ─► [ N parallel Moorer combs ] ─► [ M series allpass ]
                      (each: delay + LPF-damped fb)   (diffusion)
                    ─► wet tone (BASS low-shelf + TREB high-shelf) ─► MIX·wet
```

---

## 2. Topology (Schroeder-Moorer)

- **Pre-delay** (PRE-D) — a circular delay line on the input, 0..100 ms.
- **Parallel comb bank** — **6 Moorer combs** (Schroeder's 4 + Moorer's
  HF-damped feedback). Each comb = a delay line with feedback through a
  **one-pole low-pass** (the DAMP control — damps the tail's highs each
  pass, the "plate" character). Base delay lengths are mutually-prime
  (~50–80 ms class) scaled by SIZE; feedback gain derived from DECAY.
- **Series allpass diffusers** — **4 allpass** stages after the comb sum
  for density/diffusion (DIFF + DENSITY controls). Short delays (~1–6 ms).
- **Wet tone** — BASS low-shelf + TREB high-shelf on the wet tail only.
- **MIX** — wet/dry blend at the output (the reverb amount).

R8 note: reverb feedback loops are denormal-prone on long/quiet tails —
add a tiny DC bias or flush-to-zero in the comb feedback to avoid denormal
CPU spikes.

---

## 3. Param → DSP mapping

| Control | Range (DSP2024P front panel) | Maps to |
|---|---|---|
| **PRE-D** (pre-delay) | 0..0.100 s, 1 ms steps | input circular delay length |
| **DECAY** | 0.1..5.0 s | comb feedback gain `g = 10^(-3·D/DECAY)` per comb delay D (RT60) |
| **DAMP** (HF damping) | 1..100 | one-pole LPF cutoff in each comb feedback (higher = more damping = lower cutoff) |
| **SIZE** (room size) | 1..100 | scales all comb + allpass delay lengths |
| **DENSITY** (SHV.D) | 1..100 | allpass coefficient (echo density) |
| **DIFF** (diffusion) | 1..100 | allpass diffusion amount |
| **BASS** (wet low shelf) | ±18 dB | wet-tail low-shelf gain |
| **TREB** (wet high shelf) | ±18 dB | wet-tail high-shelf gain |
| **MIX** (wet/dry) | 0..100 % | output wet blend |

Mappings DECAY→g, DAMP→Hz, SIZE→scale, DENSITY/DIFF→allpass coeffs are
Lyra-native curves (documented in code, bench-tunable) — the operator
*values* are locked, the internal curves are ours.

---

## 4. Presets (LOCKED — `tx1_ssb_design.md` §11)

| Param | W5UDX (Greg) | N8SDR |
|---|---|---|
| PRE-D | 0.010 s | 0.010 s |
| DECAY | 2.358 s | 1.542 s |
| DAMP | 10 | 15 |
| SIZE | 33 | 10 |
| DENSITY (SHV.D) | 32 | 20 |
| DIFF | 20 | 20 |
| BASS | −16 dB | −16 dB |
| TREB | +16 dB | +16 dB |

**OPEN — MIX value not in the captured presets (R4).** A reverb needs a
wet/dry amount and the DSP2024P captures didn't include the effect
mix/level. ESSB "air" is subtle — I'll default **MIX ~15 %** and flag it
for the operator to set their actual wet level (or dial on the bench).
Both presets otherwise complete.

---

## 5. UI surface (PlatePanel.qml)

Dockable / collapsible / View-hideable like the EQ/Speech/Combinator
panels; a fourth chip in the "TX DSP:" strip. **Default stage OFF.**
- **ON** (bypass) · collapse.
- Sliders: PRE-D, DECAY, DAMP, SIZE, DENSITY, DIFF, BASS, TREB, MIX
  (CtlRow idiom, the Combinator panel's component).
- A **preset picker** (W5UDX / N8SDR) that loads the locked values
  (red-team R-presets: scope = just these two, or add factory rooms? —
  operator decides; design ships the two locked).

PlateModel (lyra::ui): Q_PROPERTY per control + static txProcessCb +
s_txEngine bridge. Default OFF, seeded N8SDR preset.

---

## 6. Build stages (same pattern as EQ / Speech / Combinator)

1. **`Plate.{h,cpp}` (lyra::dsp, Qt-free)** + `scratch/test_plate.cpp` —
   combs + allpass + pre-delay + wet shelves + MIX; denormal guard.
   Tests: impulse tail decays to ~RT60≈DECAY, MIX 0 = dry passthrough,
   bypass untouched, finite over a big block, DAMP shortens HF tail.
2. **`PlateModel` + `PlatePanel.qml`** — panel + preset picker, dock,
   default preset seeded.
3. **Wire** into CMaster TX rack AFTER the Combinator, before ALC, under
   `tx_rack_bypass`; `SendpTxPlateProcessor`; register in main.cpp;
   default stage OFF.

---

## 7. Red-team checklist (resolve before / during Stage 1)

- **R1** comb/allpass base-delay tuning — mutually-prime, no metallic
  flutter; impulse tail smooth. Unit-test the decay length ≈ DECAY.
- **R2** DECAY→feedback-gain per-comb (`g = 10^(-3·D/RT60)`); clamp g<1.
- **R3** DAMP 1..100 → one-pole cutoff Hz curve.
- **R4** MIX default + range (operator wet-level — not in captured presets).
- **R5** DENSITY vs DIFF distinction (both allpass — coeff vs count/depth).
- **R6** default stage OFF.
- **R7** CPU (6 combs + 4 allpass + 2 shelves at 48 kHz — trivial; confirm).
- **R8** denormal flush in feedback loops (reverb tails) — mandatory.
- **R9** SIZE→delay-scale range so min SIZE doesn't collapse combs to
  comb-filter "boing"; max SIZE stays < the allocated delay buffers.
