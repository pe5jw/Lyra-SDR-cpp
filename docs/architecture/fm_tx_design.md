# FM TX — "beat Thetis" design & findings

Status: **Fix #1 (Carson bandpass clamp) CODE COMPLETE + builds (lyra.exe links clean,
2026-06-28), awaiting operator bench. Fixes #2–#5 still to do.**
Date: 2026-06-28. Sources: a read-only code scroll of Thetis vs Lyra FM TX paths +
two deep-research passes on HF FM technical/regulatory requirements (the second one
closed the pre-emphasis time-constant gap — see §3 update).

Goal (operator): Thetis is poor on FM; make Lyra's FM **cleaner and more correct**.
Three operator recollections to verify + fix: (a) over-wide FM TX bandwidth → splatter,
(b) Thetis pre-emphasis done incorrectly, (c) US-vs-other-country FM differences.

---

## 1. Key reframe

Both Thetis and Lyra drive the **same stock WDSP `wdsp.dll`** (`xfmmod`/`xemphp` math is
byte-identical). The only difference is **which setters each app calls**. So "beating Thetis"
on FM is about driving the WDSP FM chain correctly + clamping bandwidth + adding controls
Thetis lacks — not new DSP.

WDSP TX chain order (TXA.c `xtxa`, FM-relevant stages):
`xeqp → xemphp(preemph,pos0) → leveler → cfcomp → bp0 → comp → osctrl → alc →
ammod → xemphp(preemph,pos1) → xfmmod → uslew`. Pre-emph runs at exactly ONE position
(0 or 1; create-time default = 1, just before the modulator).

---

## 2. Operator's three recollections — verdicts (file:line verified)

**(a) Over-wide FM TX bandwidth → splatter — TRUE, but it's now a LYRA bug; current Thetis fixed it.**
- Current Thetis (2.10.3.13): FM RF passband Carson-clamped `halfBw = TXFMDeviation + TXFMHighCut`
  (`console.cs:8105-8108`, tag `[2.10.3.4]MW0LGE`); a SEPARATE FM audio filter (300/3000,
  `radio.cs:4146-4149`, never set to the SSB width); plus WDSP's internal modulator self-clamp
  `bp_fc = deviation + f_high` (`fmmod.c:39`). The wide SSB/ESSB filter does NOT reach FM.
- **Lyra**: FM output bandpass `bp0` IS set from the operator SSB/ESSB width — `pushTxFilter`
  (`main.cpp:1214-1242`), FM (mode 5) does `SetTXABandpassFreqs(±txBandwidth/2)`. **No Carson clamp,
  no FM override.** A wide ESSB TX BW (up to 10 kHz) passes straight through on FM. Only WDSP's
  internal `fmmod bp_fc` clamps the *modulator*; the downstream `bp0` is wider and useless for FM.
  → **THE concrete bug. = operator's recalled splatter, live in Lyra.**

**(b) Pre-emphasis incorrect — PARTIALLY CONFIRMED.**
- WDSP slope is a correct 6 dB/oct (`-20*log10(f_high/f_low)` over 300-3000, `emph.c:50/137/155`),
  BUT there is **no time-constant** anywhere (no 50/75 µs; `ctype=0` FIR fit, not an RC network).
- Thetis Console **conflates emphasis on/off with the chain-position selector**: `radio.cs:3110`
  passes the C# `bool TXFMEmphOn` into `SetTXAFMEmphPosition` (`emph.c:107-113` writes
  `preemph.position`). Mode-set force-runs `preemph.run=1` (`TXA.c:780`), so emphasis is never
  truly disabled — "off" just moves it to position 0.
- **Lyra drives pre-emphasis NOT AT ALL** — never calls `SetTXAFMEmphPosition`/`PreEmphFreqs`
  (grep clean; `wdspcalls` binds only `SetTXAMode`/`SetTXAFMDeviation`/`SetTXACTCSSFreq`/`Run`).
  It inherits WDSP create-time defaults (pos 1, 300-3000, ctype=0). Pre-emphasis runs, but Lyra
  has zero control and inherits the same no-τ imprecision with no UI.

**(c) US vs other-country differences — CONFIRMED.**
- Thetis hardcodes two deviations (5000/2500 radio buttons, `console.cs:41282/41311`).
- **Lyra already better**: continuous 1-6 kHz deviation spin + "Wide (US)"/"Narrow (US/EU)" labels
  (`settingsdialog.cpp:4194-4227`), `setFmDeviationHz` clamp [1000,6000] (`hl2_stream.cpp:2723`).
- Regulatory difference (research): **US has no numeric limit**; **IARU R1 has a hard 12 kHz cap** —
  see §3.

---

## 3. Deep research — verified facts (cited)

- **Carson's rule** BW = 2(Δf + f_max). Wide ±5 kHz + 3 kHz audio ⇒ **16 kHz**; narrow ±2.5 kHz +
  3 kHz ⇒ **~11 kHz** (10 kHz at a 2.5 kHz audio top). [NTIA Redbook Annex J — primary;
  corroborated Wikipedia/HamRadioSchool/repeater-builder]
- **US deviation standard**: wide ±5.0 kHz, narrow ±2.5 kHz across 10/6/2 m etc. [repeater-builder]
- **US FCC Part 97.307**: NO numeric FM deviation/channel limit. (a) "no more bandwidth than
  necessary … good amateur practice"; (b) no splatter / confined to segment. ±5 kHz is a
  *band-plan convention*, not law. [47 CFR 97.307 — Cornell/eCFR primary]
- **IARU Region 1 (UK/EU)**: HARD **12 kHz max occupied bandwidth** + **12.5 kHz channelization**
  on 6/4/2 m FM segments ⇒ ±2.5 kHz narrow is the norm. [IARU-R1 VHF bandplan PDF — primary; RSGB]
  ← **the US-vs-rest difference the operator recalled.**
- **10 m band plan**: FM 29.500-29.700 MHz, 10 kHz channels, **29.600 simplex calling**, repeaters
  −100 kHz offset (in 29.520-29.590, out 29.610-29.700). [ARRL — primary]
- **Clean FM TX**: band-limit audio ~300-3000 Hz (nothing useful >4 kHz) + channel-matched TX
  bandpass (not SSB width) + audio limit/clip ahead of the modulator. [repeater-builder + 97.307(b)]

### Pre-emphasis follow-up research (2026-06-28, second deep-research pass) — RESOLVED ENOUGH TO DECIDE

A focused adversarial-verification pass on amateur HF/low-VHF NBFM pre-emphasis. What it
**confirmed (high-confidence, voted)**:
- Pre-emphasis is a **6 dB/octave** rising response (τ sets the corner, not the slope). [repeater-builder predeemp]
- Clean comms-FM TX chain order: **audio BP → pre-emphasis → IDC/clipper → splatter LPF → modulator**
  (pre-emph BEFORE the clipper, splatter LPF AFTER it). [Motorola US4,802,236]
- **CTCSS is injected flat** — bypasses limiting + pre-emphasis, below the ~300 Hz HPF, ~10–15% of
  system deviation. [confirmed]
- **75 µs = US FM *broadcast* standard** (≈2122 Hz corner). [Wikipedia/FCC]

What it could **NOT** confirm (all 0-0 abstain — *unverified*, not refuted):
- A single authoritative pre-emphasis τ specifically for **amateur NBFM** (50 vs 75 vs none).
- That a **phase modulator inherently pre-emphasizes** (the crux for WDSP's integrate-phase `xfmmod`).

### RESOLVED by code (2026-06-28) — WDSP xfmmod is TRUE FM, so the comms 6 dB/oct curve is correct

The research's one unresolvable abstain ("does a phase modulator inherently pre-emphasize?") is **moot for
Lyra** because WDSP's `xfmmod` is a **true frequency modulator**, verified in `fmmod.c:95-100`:
`dp = audio * sdelta; sphase += dp; out = cos(sphase)/sin(sphase)` — instantaneous phase is the running
**integral** of the audio ⇒ instantaneous frequency ∝ audio = textbook FM. A *phase* modulator would set
φ ∝ audio directly (no integral), which is what produces the inherent +6 dB/oct. xfmmod integrates, so it
has **NO inherent pre-emphasis**. ⇒ applying a 6 dB/oct pre-emphasis is **correct, not double-emphasis**.

And WDSP's native `emph.c` IS that curve: a **6 dB/oct rise bounded 300–3000 Hz** (`emph.c:50/137/155`),
NOT an RC time-constant — exactly the amateur/land-mobile comms curve a real Icom/Kenwood produces (they
get it free from PM physics; Lyra applies it explicitly to a true-FM core). So the "good" voice mode needs
**no custom DSP** — it's WDSP's built-in pre-emph, just exposed with a real Off (Thetis force-runs it,
`TXA.c:780`, and conflates Off with chain-position — never a true bypass).

**Engineering recommendation (REVISED, decisive — supersedes the "default Off" above):** ship a clean
selector, NOT Thetis's on/off-vs-position conflation:
- **Off / Flat** — WDSP emph `run=0`, true bypass. Warm/bass-rich; **AUTO for DIGU/DIGL** (already
  digital-gated in Lyra). Mandatory for VARA/Packet/FT8.
- **Comm (6 dB/oct, 300–3000)** — WDSP's NATIVE `emph` (`SetTXAFMEmphPosition`+run). The rig-match;
  **the voice default.** This is the "beat Thetis" sound — same curve, but Lyra lets you reason about it
  and truly turn it off.
- **50 µs / 75 µs (optional, experimental)** — a Lyra-native single-pole IIR pre-stage
  (`y[n]=x[n]−α·x[n−1]`, `α=e^(−1/(fs·τ))`, output-normalized) for ESSB/wideband experimenters, since
  WDSP's emph is not τ-parameterized. Sits **before** the FM AF brick-wall LPF (fix #2) so the HF boost
  can't splatter.

So plan item #3 = a 3-(or 4-)position selector: **Off / Comm / [50µs] / [75µs], voice default = Comm,
digital auto = Off.**

### Still-open research gaps (lower priority)
- **CTCSS** tone deviation level + exact placement below the 300 Hz HPF: no verified claims.
  (Lyra already has the 50-tone table + FM-gating; this is the level/placement refinement.)
- Narrow-FM audio top **2.5 vs 3.0 kHz** (changes Carson BW 10 vs 11 kHz; matters for 12.5 kHz fit).
- **IARU Region 3 (ACMA/JARL)** norms not pinned (treat as narrow-leaning, open).

---

## 4. The plan (priority order)

1. **Carson-clamp the FM TX bandpass (THE bug). ✅ DONE 2026-06-28 (builds; awaiting bench).**
   In `pushTxFilter` (`main.cpp:1214`), FM (mode 5) is now its OWN switch branch that does NOT
   use `txf->high` (SSB/ESSB width). Sets `halfBw = stream->fmDeviationHz() + kFmAfHighcutHz`
   (`kFmAfHighcutHz = 3000.0`; ≈ 5000+3000=8000 wide / 2500+3000=5500 narrow) →
   `SetTXABandpassFreqs(-halfBw,+halfBw)`. Mirrors Thetis `console.cs:8105`. The `.setFmDeviation`
   forwarder now also re-calls `pushTxFilter()` so the clamp tracks live deviation changes
   (no-op for non-FM modes — branch reads the live `txf->mode`). Clean callers-updated edit, NOT a
   shim. **Bench check: in FM, a wide ESSB TX BW must no longer widen the FM occupied bandwidth.**
2. **Drive the FM audio filter explicitly.** Bind + call `SetTXAFMAFFilter(ch, low, high)` (or
   `SetTXAFMPreEmphFreqs`+`SetTXAFMAFFreqs`) on FM entry (300/3000 wide; ~300/2550 narrow). Bind in
   the `wire/wdspcalls.cpp` X-macro list. Removes the current rely-on-default-by-luck.
3. **Pre-emphasis: bind + control. DECISION (research §3, code-resolved): selector Off / Comm /
   [50µs] / [75µs]; VOICE default = Comm (WDSP native 6 dB/oct 300–3000); DIGITAL auto = Off.**
   xfmmod is true FM (no inherent pre-emph), so the comms curve is correct, not double-emphasis.
   - Off / Comm = WDSP native: bind `SetTXAFMEmphPosition` + the emph run; Off = true bypass (beats
     Thetis's force-run + position-conflation, `TXA.c:780`).
   - 50/75µs = Lyra-native single-pole IIR pre-stage (`α=e^(−1/(fs·τ))`, normalize gain) BEFORE the
     fix-#2 AF LPF — optional/experimental for ESSB ops; WDSP emph isn't τ-parameterized.
   - CTCSS stays injected flat (already FM-gated in Lyra; matches the verified comms-chain rule).
4. **Region preset** {deviation, AF highcut, BW clamp}: **US** = ±5 kHz wide default + ±2.5 narrow
   (no legal cap, no-splatter); **IARU R1** = ±2.5 kHz / 12 kHz-BW cap. #1 consumes the pick.
5. **(optional) soft FM deviation limiter** on the audio into `xfmmod` (or FM-tighter ALC). Neither
   app has one; extra over-deviation insurance. Low priority.

**Net:** #1 is the single concrete bug (FM bandpass inherits ESSB width → splatter; current Thetis
already clamps, Lyra does not). #2/#3/#4 are how Lyra goes past Thetis. #3's τ is the one genuinely
unresolved decision.

---

## 5. Reference anchors
- Thetis tree: `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/`
  (`wdsp/fmmod.c`, `wdsp/emph.c`, `wdsp/TXA.c`; `Console/console.cs`, `Console/radio.cs`).
- Lyra: `src/hl2_stream.{h,cpp}`, `src/main.cpp` (`pushTxFilter`, FM mode 5), `src/wire/wdspcalls.{h,cpp}`,
  `src/settingsdialog.cpp` (FM Settings group).
