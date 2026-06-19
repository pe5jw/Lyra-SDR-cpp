# RX EQ (#59) design — the RX mirror of the shipped TX EQ (#50)

**Status:** grounded design, awaiting operator sign-off. NO code yet.
**Scope:** #59. A receive-side parametric EQ, the RX twin of the shipped TX
EQ (#50, v0.4.0). Per the locked EQ plan ([[project-lyra-cpp-eq]] Stage 5):
"RX mirror = 2nd `ParamEq` instance, post-RXA." Same native RBJ-biquad engine,
same EESDR3 panel — applied to the RX audio instead of the mic.

## What's already built + reusable (no rework)
- **`src/dsp/ParamEq.{h,cpp}`** — generic RBJ-biquad cascade, 10 typed bands,
  `magnitudeDb()` (drawn curve == heard), RT-safe. Engine is side-agnostic.
- **`src/dsp/EqAnalyzer.{h,cpp}`** — pre/post FFT analyzer behind the curve.
- **`src/qml/EqPanel.qml`** — the full EESDR3 view (typed nodes, tile row,
  Spec/Acc/B-A). Bound to a context property — reusable for a 2nd instance.
- **`src/eqmodel.{h,cpp}`** — `EqModel` (the model/view bridge). The ONLY
  TX-specific piece: statics `s_txEngine`/`s_txAnalyzer`/`s_self` +
  `txProcessCb` registered to the CMaster TX rack (`SendpTxEqProcessor`).

## Insertion point (verified, Thetis-faithful)
RX audio is produced post-WDSP and handed to `WdspEngine::dispatchAudioFrame
(const double* audio, int nframes)` — stereo-interleaved doubles, mono-dup
(L==R) for a single RX. EVERY consumer tees there: HL2 jack (OutBound 0), PC
sink (`pcm16_`), TCI (`audio[2f]` → digital apps), VAC (`xvacOUT` stream 1).
RX EQ runs at the **TOP of `dispatchAudioFrame`, before all tees** — i.e.
pre-tap, exactly where Thetis's RXA `eqp` sits (it colors what you hear AND
what's recorded/bridged). Direct C++ call (NOT a CMaster fn-ptr — the RX path
is a `WdspEngine` method, unlike the TX rack's cm_main-thread hook).

## The two decisions (LOCKED 2026-06-19)
1. **Pre-tap, with BOTH a manual operator bypass (any mode) AND an auto
   DIGU/DIGL bypass.** Apply RX EQ to `audio` before the tees (Thetis-faithful
   — colors what you hear AND a recording/VAC capture). Bypass is two-layer:
   (a) **manual** — the panel ON button = `EqModel.bypass`, works in ANY mode,
   exactly like the TX EQ (operator-required 2026-06-19: "a option to bypass
   it … like the TX side, not just automatically in digital modes"); (b)
   **auto** — additionally force-bypass in DIGU/DIGL (mirror the shipped TX
   rack mode-gate `c8441a6`) so digital decoders always get flat audio.
   Effective gate: `rxEqEngine && !bypass && mode∉{DIGU,DIGL}`.
2. **RX EQ persists via QSettings `rxeq/*`, NOT in the TX Profile (#49).**
   Profiles bundle the TX chain; RX EQ is an RX listening setting. Standalone
   persisted setting (its own enable/bypass + bands), like RX bandwidth.

## Engine detail (mono-dup)
RX `audio` is mono-dup (L==R). `ParamEq::processInterleaved` filters the
"I"/L lane and skips the "Q"/R lane — that would break L==R before the tees.
Add **`ParamEq::processMonoDup(double* interleaved, int nframes)`**: filter
the mono (L) lane through the cascade, write the result to BOTH L and R.
One biquad-state instance, correct for the mono-dup RX. (When RX2/true-stereo
lands, revisit — but RX1 is mono-dup today.)

## Build order (most of the engine is done — small stages)
- **R-1** — `EqModel` gains a `Side {Tx, Rx}` ctor arg. `Tx` keeps the
  existing `s_txEngine`/`txProcessCb`/CMaster registration (untouched). `Rx`
  skips all of that; it just owns its `ParamEq`+`EqAnalyzer` and exposes
  `engine()`. Behavior-neutral for TX. (Clean parameterize, not a twin.)
- **R-2** — `WdspEngine::setRxEqEngine(ParamEq*)` (atomic ptr) +
  `ParamEq::processMonoDup`; apply at the top of `dispatchAudioFrame` gated on
  `rxEqEngine && !bypass && mode∉{DIGU,DIGL}`. **RX EQ goes live.** Bench:
  voice RX, dial a peak/notch → audio + the panel analyzer change; DIGU/DIGL
  → bypassed (FT8 decode unaffected).
- **R-3 — DONE (builds clean, exit 0).** 2nd dock "RX EQ" (`RxEqPanel.qml`
  wrapper → `EqPanel`, context property `RxEq`) +
  **chip-click pop-out** (operator 2026-06-19: same idiom as the TX DSP
  chips — dock defaults `setFloating(true)+hide()`, a chip = its
  `toggleViewAction`); persistence `rxeq/*` (enable/bypass + bands). The RX
  EqModel's analyzer taps the RX audio (animates on RX always — bonus over TX
  which needs a live mic). **Plus: the RX EQ graph x-range tracks the RX
  bandwidth** (operator 2026-06-19 — the RX mirror of the shipped TX EQ #168
  "track/scale x-range with TX bandwidth"): parameterize `EqPanel.qml` to take
  BOTH the model (`property var eq: Eq`, `Eq.`→`eq.`) AND its band-edge source
  — TX panel binds the TX BW, RX panel binds the RX bandwidth — so the curve
  spans the passband actually in use.
- **R-4 — DONE.** USER_GUIDE: added the "RX EQ — receive parametric EQ"
  subsection alongside the TX EQ (RX DSP chip, RX-bandwidth graph tracking,
  ON-bypass-any-mode + auto-DIGU/DIGL, standalone-not-in-TX-profile).

## Post-build fix (operator bench 2026-06-19)
The bulk `Eq.`→`eq.` rename missed the `Connections { target: Eq }` block
(no dot), so on the RX panel RxEq's `bandsChanged` never bumped the `rev`
tick — the node markers looked frozen (engine value moved, the drawn node
didn't). Fixed: `target: eq` (follows the bound model; TX unchanged since
eq==Eq there).

## Out of scope
RX EQ in the TX Profile (it's a standalone RX setting); true-stereo RX EQ
(RX1 is mono-dup; revisit with RX2 #96-101); a 2nd RX-side Combinator/Plate
(those are TX-chain processors — not mirrored to RX).
