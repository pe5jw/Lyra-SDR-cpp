# CW-5 — RX CW decoder + macro field design

**Status:** grounded design, awaiting operator sign-off. NO code yet.
**Scope:** #173 CW-5. A native C++23 RX CW decoder + a station-token macro
grid, both landing in the existing `CwConsolePanel` (CW-3b shipped the
shell + reserved decoder pane). Faithful clean-room port of the SDRLogger+
decoder (operator's own code; the algorithm is the value — port it, don't
shortcut to a threshold decoder). Full reference extraction in memory
[[reference-sdrlogger-cw-decoder]] (cw.html / main.py file:line).

## Locked decisions (operator 2026-06-18)
- **Macros = station tokens only.** `{MYCALL}/{MYGRID}/{MYNAME}/{MYRIG}/
  {MYANT}/{MYPWR}/{MYSTATE}/{MYCNTY}` from Lyra Settings → Station + plain
  text. NO worked-station tokens (`{CALL}/{NAME}/{RST}/{NR}`), serial,
  Auto-Log-73, or logger integration — Lyra has no QSO log.
- **Reuse the panadapter** for tuning — NO separate CW waterfall in the
  console. The decoder pane shows decoded text + WPM readout + AFC-lock
  indicator only.
- **Build decoder + macros together** (one CW-5 arc), then bench.
- Macros route to the shipped CW-3 `HL2Stream::sendCw()` (Lyra IS the
  keyer — not a TCI/Winkeyer client; that's #171, separate).

## 1. Decoder — native DSP class (`src/dsp/CwDecoder.{h,cpp}`, new)

Pure-ish C++23 class; feed it mono audio blocks + sample rate, it emits
decoded text. Faithfully ports the SDRLogger+ chain (constants in
[[reference-sdrlogger-cw-decoder]]):

1. **IQ downconvert** at the tone (complex NCO, ~0.7 ms integ window,
   coherent phase across blocks) → magnitude.
2. **AFC** — ±range (50/100/150/200 Hz) Goertzel scan, 20 Hz step, lock
   hysteresis + clarity gate + EMA; exposes lock freq + locked flag.
3. **Asymmetric envelope** (ATK 0.30 / DKY 0.09) → **matched filter**
   (ring-buffer running avg, win scales with WPM, ×0.55 if DSP-filter on).
4. **Noise blanker** (toggle) → **adaptive noise floor** (3-mode w/ the
   post-mark holdoff) + **peak** tracker (QSB snap).
5. **SNR gate** (squelch ×1.1–3.0) → normalize → **proportional
   hysteresis** (threshold 15–90 %).
6. **Edge timing** (sample-based monotonic clock) + **one-edge lookback
   merge** (kills noise blips).
7. **Bayesian classifier** — dit/dah (1.565× bootstrap → Gaussian logL),
   element/char/word (0.85× inter-element, independent Farnsworth char/word
   models), online EMA fist-learning (α 0.15 up / 0.075 down, outlier
   reject, variance floor), WPM readout = 1200/μ_dit.
8. **Morse table** → char assembly → emit text (signal `textDecoded`,
   `wpmChanged`, `afcLockChanged`).

Unit-testable: feed synthesized CW audio (known text/WPM/noise) → assert
decode. No Qt in the core math (a thin QObject wrapper carries signals).

## 2. Audio tap (the one CW-5 verify-first step)
The decoder needs the **demodulated CW RX audio** (the tone at ~CW pitch).
Lyra produces this post-WDSP (the audio that feeds the AK4951/PC sink + the
TCI RX-out packetiser). CW-5a's first step: identify the existing
post-demod RX-audio tap point (same source as the TCI RX-out / audio sink),
add a CW-mode-gated tap that feeds `CwDecoder`. Mono, 48 kHz. Only active
in CWU/CWL + when the console decoder is on (no cost otherwise). VERIFY the
exact hook before wiring (don't guess).

## 3. Console UI additions (`CwConsolePanel.qml` + a CwDecoder context obj)
Replace the "RX decoder — coming with CW-5" placeholder with:
- **Decoded-text pane** (scrolling, monospace, font-size + color like
  SDRLogger+; clear button).
- **WPM readout** + **AFC-lock indicator** (— / locked Hz).
- **Decoder knobs:** decode on/off; tone (defaults to + follows the unified
  CW pitch); AFC range; Squelch; Threshold; Noise-blanker + DSP-filter
  toggles. (Tuning is on the main panadapter — no console waterfall.)
- **Macro grid:** 16 slots (label + text), click=send (→ `sendCw` after
  station-token expansion), right-click=edit; station-token insert buttons;
  factory-reset. Persist via QSettings (`cw/macros`, `cw/decoder*`).
- Station-token expansion reads Lyra's operator/station settings.

## 4. Build order (within CW-5)
- **CW-5a** — `CwDecoder` DSP class (pure, unit-tested vs synthetic CW) +
  the verified CW-audio tap. Bench: decode real off-air CW → text + WPM +
  AFC lock; mode-gated; zero RX-audio impact when off.
- **CW-5b** — console decoder pane + knobs (wire `CwDecoder`).
- **CW-5c** — the 16-slot station-token macro grid → `sendCw`.
- Bench the whole console, then ship.

## Out of scope (noted)
Worked-station tokens / serial / Auto-Log-73 (no logger); a console CW
waterfall (reuse panadapter); mid-text `>`/`<` speed + `|prosign|` macro
syntax (EESDR macro-text — a CW follow-on); external Winkeyer (#171).
