# FM TX correctness ("1-up Thetis") — grounded scope

*Reference-studied 2026-06-20 against Thetis 2.10.3.13 (WDSP `TXA.c`,
`fmmod.c`, `emph.c`; `Console/radio.cs`) + the lyra-cpp current FM path.
Study only — no code yet. The operator goal is **cleaner FM than Thetis**,
not bug-for-bug parity.*

---

## 1. How the reference (Thetis/WDSP) does FM TX

**TXA chain order** (`TXA.c` `xtxa`), amplitude stages → modulator:

mic → panel gain → **PHROT** → EQ → (downward expander) → **emph(pos 0)** →
**Leveler** → **CFC** → bp0 → **Compressor** → bp1 → **CESSB/osctrl** → bp2
→ **ALC** → AM-mod → **emph(pos 1)** → **FM modulator (`xfmmod`)** → out.

Key facts:
- The **FM modulator sits after ALC**, near the end. Everything above it
  is amplitude-domain processing of the *audio* that becomes deviation.
- **Pre-emphasis** has two positions: 0 = early (pre-Leveler), 1 = late
  (post-ALC, just before the modulator). `SetTXAMode(FM)` sets
  `fmmod.run=1` **and** `preemph.run=1` (`TXA.c:778-781`). Thetis default
  `TXFMEmphOn=true` → position 1.
- **`fmmod`** outputs constant-envelope I/Q (`0.7071·{cos,sin}(phase)`),
  `phase += audio · 2π·deviation/Fs`. CTCSS tone is mixed into the audio
  *before* the phase integrator (`fmmod.c:89-93`).
- **Deviation** default 5000 Hz; the modulator's internal FIR bandpass =
  ±(deviation + f_high) and is **recomputed on every deviation change**, so
  RF bandwidth ≈ 2·(dev + f_high) ≈ 16 kHz (Carson) automatically. FM AF
  band default 300–3000 Hz.
- **Thetis does NOT force the SSB amplitude stages off in FM** — Leveler /
  CFC / COMP / CESSB stay at whatever the operator left them. *That is
  exactly why Thetis FM often sounds bad: the SSB voice chain colors the
  deviation.* The only FM-specific DSP gate is the bp0/bp1 filter setup
  (`TXASetupBPFilters`, FM case).

## 2. lyra-cpp's current FM path

Mode = WDSP TXA mode 5; `setMode` lambda calls `SetTXAMode` (main.cpp
~1224). Bound WDSP setters (wire/wdspcalls.h): `SetTXAMode`,
`SetTXAFMDeviation`, `SetTXACTCSSFreq/Run`, `SetTXAPHROTRun`,
`SetTXALevelerSt/Decay/Top`, `SetTXAALCDecay/MaxGain`. **lyra-cpp does
NOT run WDSP Compressor / CFC / CESSB** — voice processing is its own
**native rack** (EQ #50 / Speech #88 / Combinator #51 / Plate #52),
pre-WDSP-TXA.

So the **effective FM signal path today**:

mic → **native rack (EQ/Speech/Combinator/Plate)** → **PHROT** → **Leveler**
→ bp0 → **ALC** → **pre-emphasis** → **FM modulator** → out.

Mode gating that exists:
- **Native rack** bypassed by `SetTxRackBypass(1)` **only for DIGITAL**
  (DIGU/DIGL) — `txModeIsDigital()`, main.cpp ~1336-1354. **Runs in FM.**
- **PHROT** auto-off for digital (`applyPhrotRun`, digital-gated). **On in
  FM.**
- **CTCSS** correctly FM-gated (`applyCtcssRun`, mode 5 only) — wired.
- **Deviation** wired (`Stream.fmDeviationHz` → `SetTXAFMDeviation`).
- **Pre-emphasis**: enabled implicitly by the ported `SetTXAMode(FM)`
  (`preemph.run=1`) at WDSP default position; **not** operator-exposed
  (no `SetTXAFMEmphPosition` binding).

## 3. The gap (why FM isn't "correct" yet)

In FM, lyra-cpp still runs the **native voice rack + PHROT + Leveler**
into the FM modulator — amplitude/phase processing that's meaningless or
harmful for a constant-envelope mode. It colors and over-shapes the
deviation. Same root cause as Thetis FM, plus lyra runs its own rack on
top.

What's already right: deviation, CTCSS (FM-gated), pre-emphasis on (at
default), ALC (good — it caps peak audio → caps peak deviation).

## 4. Proposed fix — "1-up Thetis"

Give FM a clean path: **audio → (mic gain) → pre-emphasis → ALC(cap) →
FM modulator**, bypassing the SSB voice/phase coloring. Mostly reuses the
existing digital-gate pattern.

**Core (high value, low risk — extends existing gates):**
1. **Bypass the native rack in FM** — widen the gate so EQ/Speech/
   Combinator/Plate are bypassed for FM as well as digital (`SetTxRackBypass`).
2. **Force PHROT off in FM** — extend `applyPhrotRun`'s gate (FM is
   constant-envelope; phase rotation is an SSB peak trick).
3. **Force Leveler off in FM** — `SetTXALevelerSt(0)` while in FM (it
   shapes deviation dynamics; cleaner without). *Operator decision — see
   below.*
4. **Keep ALC on** (peak-deviation guard) and **pre-emphasis on**.

**Verify / likely-small:**
5. **FM TX audio bandpass (bp0)** = the audio band (≈300–3000), not the
   wide ESSB TX BW — confirm the FM-mode filter push does this (the fmmod
   internal bandpass already handles RF BW = 2·(dev+f_high) on its own).
6. **Pre-emphasis**: confirm the ported `SetTXAMode(FM)` lands the late
   position (1) + the 75 µs (US) / 50 µs (EU) curve. Add a
   `SetTXAFMEmphPosition` binding only if the default is wrong or we want
   to expose it.

**Possible follow-on (separate from TX):**
7. **RX FM bandwidth** = Carson-wide (~16 kHz) when receiving FM, tracking
   deviation — an RX-filter concern, not the TX chain. Scope as its own
   item if wanted.

## 5. Operator decisions before coding

- **(A) FM auto-bypass set** — confirm: native rack ✅, PHROT ✅, Leveler
  ✅ (recommend off), ALC ✅ keep, pre-emphasis ✅ keep. Any you want left
  operator-controllable in FM instead of forced?
- **(B) Pre-emphasis** — fine to rely on the WDSP default position/curve
  for now, or expose a position + 75/50 µs control (region table already
  exists for deviation)?
- **(C) RX FM Carson bandwidth** — include in this work, or split to its
  own follow-on?

## 6. Build sequence (once decisions locked)

1. Extend the mode-bypass gates (rack + PHROT + Leveler) to FM — one
   focused change to the existing gate helpers; auto-restores on leaving
   FM. Bench: FM TX sounds clean vs an SSB-chain-colored baseline.
2. Verify/curate the FM TX bandpass + pre-emphasis position (add the emph
   binding only if needed).
3. (Optional) RX FM Carson bandwidth.

No new wire/protocol surface; all via existing WDSP setters + the bypass
pattern already shipped for digital. PureSignal-irrelevant (FM isn't a
PS mode). See [[project-lyra-cpp-tuning-ui]] / [[project-lyra-cpp-tx]].
