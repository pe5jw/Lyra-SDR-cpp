Four senior reviews are in, and I cross-checked their claims against the actual code myself (and caught one algebra slip — see the footnote). Here's the consolidated picture.

# CW Decoder — improvement opportunities

**Verdict first:** this is a genuinely strong decoder, not a threshold toy. The coherent-IQ detector, adaptive floor/peak with QSB snap, Farnsworth-aware space model, one-edge despeckle, and online "fist" learning are all real engineering. So the wins below are about raising a ceiling, not fixing something broken — and the hand-tuned constants are an asset to protect, not churn.

**The one structural insight everything traces back to:** the decoder is **single-pass, causal, greedy** (it commits every dot/dash and every character the instant it sees them, no lookahead), and it **hard-codes the machine-CW ratios** — dah = 3×dit, spaces = 1:3:7 — into a decoder whose stated mission is *hand-sent and Farnsworth* copy. Almost every accuracy finding is a facet of decoupling the learned model from those fixed ratios, or of deferring a decision just long enough to use later evidence.

---

## Tier A — high value, low risk (safe to do without disturbing the tuning)

These are additive or self-contained; none perturb the ~35 field-tuned constants, and the first two **cannot corrupt a correct decode** (the cardinal rule).

| # | Improvement | What it fixes | Effort | Why it's safe |
|---|---|---|---|---|
| A1 | **Nearest-code rescue for `?`** — on a table miss, accept the *unique* Morse code at edit-distance 1; otherwise stay `?` | Single dropped/inserted element (the dominant `?` cause on weak signals) | S | Runs **only** on table-miss; a correct character never reaches it. Reject ties/distance≥2 so honest unknowns stay `?` |
| A2 | **AFC center-bias** — weight the Goertzel pick toward the operator's tuned pitch instead of "loudest peak in ±range" | AFC wandering off your signal onto a louder adjacent station (crowded 40m) | S | A peak must be *substantially* stronger than the centered one to win — stops the "I tuned him in and it drifted away" complaint |
| A3 | **Per-character confidence output** — accumulate the Bayesian log-likelihood margins (already computed, currently discarded) + decode-time SNR; expose `onChar(ch, confidence)` | Operator can't tell "sure" from "guessing" — the #1 perceptual gap vs Skimmer/fldigi | S (DSP) + S/M (UI dims low-confidence chars) | Purely additive output; doesn't touch the decode path |
| A4 | **AFC sub-bin accuracy** — generalized (non-integer-`k`) Goertzel + 3-point parabolic interpolation on the peak | Coarser-than-it-looks AFC (20 Hz step is below the 23 Hz bin); chirp/drift tracking; the pitch write-back | S | Only refines `bestHz`; touches no slicer/timing constant. Also a prerequisite if A-tier ever leads to narrowing the detector |
| A5 | **Crash-proof peak snap** — require the elevated level to persist a few windows before `peakPower_` snaps up on a long-space | A single static crash / key-click hijacks the AGC reference, then the real signal reads low-SNR for a while (a real latent bug, line 374-381) | S | Tightens an existing path; doesn't change steady-state behavior |
| A6 | **SNR-gate hysteresis** — open at `1.5×` floor, stay open until `~1.2×` | Squelch chatter chopping a character mid-fade (the slicer has hysteresis; the SNR gate ahead of it doesn't) | S | Small, targeted QSB win; just verify it doesn't fight the existing `qsbFadeFlag_` snap |
| A7 | **Unify the two word-gap emitters** — `bayesClassifySpace`→Word and the `dotEstMs_*7.5` timeout can both fire on one physical gap and **double-train `bWordSpaceMu_`** | Spurious/duplicated spaces + a slowly-biased word-space model | S–M | Bookkeeping only; the `5.0/7.5` constants stay |
| A8 | **Morse table additions** — non-colliding punctuation (`_`, `$`) + a *deliberate decision* on prosigns | Prosigns/punctuation currently emit `?` | S | Additive entries can't corrupt. **Caveat:** AR/KN/SK alias `+`/`(`/etc., so true prosign rendering needs the char-level context from Tier B — flag, don't silently alias |

---

## Tier B — high value, but it touches the hand-tuned timing model (design + a regression pass before shipping)

This is where the real accuracy headroom for *hand-sent* CW lives — and exactly where the field-tuned constants are. Each needs the synthetic-CW unit tests (the design doc promises decode-vs-known-text assertions) **plus an on-air A/B** before it ships.

- **B1 — Learn the dah length independently** instead of `dahMu = 3×dit`. Today `bayesLearnMark` folds every dah into the dit estimate (`ms/3.0`) and the classifier compares against a fixed 3× scaling. Hand keys and bugs routinely run 2.5:1–4:1; light dahs at ~2.5:1 sit fragilely close to the boundary and flip to dits (→ `N`→`I`, `D`→`U`, `K`→`R`). Add a `bDahMu_` EMA (clamped ~`[2.2, 4.2]×dit`), restore the full Gaussian log-likelihood (the `−½ln(var)` term is currently dropped), and add a mild dit-favoring class prior. **Real win for hand CW.** *Risk: medium — gate learning behind the existing warm-up and clamp hard so a burst of mis-reads can't collapse the dah model into the dit cluster.*

  > Concrete correctness bug I verified here: the warm-up split is `MARK_BOUNDARY = 1.565×dit`, but once the Gaussian model engages the boundary jumps to **√3 ≈ 1.732×dit**. A mark at ~1.65×dit is classified *differently* before vs after the `BAYES_MIN` transition — a real discontinuity. B1 resolves it.

- **B2 — Decouple char/word spacing from the dit clock (Farnsworth).** `charMu`/`wordMu` are anchored to `bDitMu_` and clamped to `[2,8]×dit` / `[4,20]×dit`. Under Farnsworth the char gap can exceed `8×dit` → it gets crushed, and a true word gap collides into the same region → word boundaries are lost. Track a `farnsworthRatio = charSpace/elemSpace` and set the word boundary relative to the *learned char gap*, not the dit. **Real win; the cheap version (B2a: widen the clamps + ratio-anchor) is low-risk.** Char-vs-word confusion is *the* canonical CW-decoder error.

- **B3 — Per-character deferred dit/dah re-fit.** Buffer a character's raw element *durations* and pick the dit/dah split that best fits *this* character's population (Morse's strong within-char 3:1 prior makes this well-conditioned on 2–6 elements), rather than one-shot-binning each element against the global model. Recovers boundary-straddle garbles during speed drift / uneven fists. **Real win for hand CW** — but the guard is mandatory: only override the global call when the population is *clearly bimodal*, never re-split all-same-element characters (H, 5, 0), or it corrupts correct output.

---

## Tier C — big levers, strategic (worth knowing about; don't reach for casually)

- **C1 — Minimum-statistics noise floor** (track the windowed minimum, Martin-style — same family the project's NR already uses) instead of the asymmetric EMA. Best weak-signal/dense-code/QSB win, but it changes the *absolute* floor level → reshuffles where `threshold_` lands in normalized space → needs the slicer constants re-validated. *Risk: medium-high.*
- **C2 — WPM-tied coherent integration.** The detector's `iqWin` is fixed at ~0.7 ms regardless of speed, capping coherent processing gain (square-law loss at low SNR, worst at slow speeds). Tying it to dit length is the **single biggest raw-sensitivity lever** — and the **highest regression risk**, because `iqWin`→`msPerWin` is the master clock for the entire timing model. Only with a full re-tune. *Pair with A4 first.*
- **C3 — Pre-AGC tap (routing, not this class).** The decoder normalizes beautifully — but only what WDSP AGC hands it; on weak signals AGC compresses the floor toward the peak and caps SNR. Feeding it from a pre-AGC CW-mode tap (the same move the captured-profile rebuild made) lifts that ceiling. Medium effort in `WdspEngine`, changes the decoder's input contract.
- **C4 — Whole-passband / multi-signal decode** (Skimmer/RBN-style). The defining capability gap, but a **product-scope decision**, not a tweak — and arguably against this decoder's "copy the one I tuned" role. The pragmatic middle ground: N instances at operator-chosen pitches, or a Goertzel comb finding the 2-3 strongest CW peaks.

---

## Tier D — park or skip (honest down-ranking)

- **Dictionary / callsign / n-gram post-pass** — *highest corruption risk of anything here* (it will "correct" a correct-but-unusual callsign into a common word). Only ever as an optional, off-by-default stage gated strictly to decoder-flagged low-confidence characters. Build last, or not at all.
- **Deeper (depth-2) despeckle, `reset()` speed pre-scan, floor-rise holdoff retune, `?` rate-limit, matched-filter window-shape** — all marginal / RF-dependent / cosmetic, or redundant with higher-tier items.

---

## Two cross-cutting rules the panel and I agree on

1. **Never corrupt a correct decode.** A1 (miss-only) and A8 (additive) honor this structurally; B3, B2, A8-prosigns, and all of Tier D can violate it if done loosely — each needs a hard guard.
2. **Any Tier B/C item must pass the synthetic-CW regression suite *and* an on-air A/B before shipping**, because it perturbs the field-tuned constants that "are the value."

**If you want a do-first slice:** **A1 + A2 + A3 + A5** is a high-impact, near-zero-risk afternoon — fewer `?`s, an AFC that stops fighting you, confidence shading, and a real latent QRN bug closed — none of it touching the tuning. Then **B1 (+ the boundary-discontinuity fix)** as the first real accuracy push for hand-sent copy.

Want me to spin any of these into an actual implementation plan (or just knock out the Tier A safe-slice on a branch for you to bench)? Happy to — your call.

*Footnote on the cross-check: the timing reviewer pegged the learned dit/dah boundary at 2.10×dit; the algebra actually gives √3 ≈ 1.732×dit, which is what makes it inconsistent with the 1.565 warm-up constant. Everything else in the four reviews checked out against the code.*