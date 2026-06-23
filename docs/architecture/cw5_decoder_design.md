# CW-5 — RX CW decoder + macro field design

**Status:** grounded design, awaiting operator sign-off. NO code yet.
**Scope:** #173 CW-5. A native C++23 RX CW decoder in its **OWN floating
panel + header chip** (separate from the CW keyer console), plus the
station-token macro grid on the keyer console. Faithful clean-room port of
the SDRLogger+ decoder (operator's own code; the algorithm is the value —
port it, don't shortcut to a threshold decoder). Full reference extraction
in memory [[reference-sdrlogger-cw-decoder]] (cw.html / main.py file:line).

## Locked decisions (operator 2026-06-18, REVISED 2026-06-23)
- **REVISED 2026-06-23 — separate keyer / decoder panels (two chips).**
  Operator wants the decoder as its OWN chip + floating panel, distinct
  from the CW keyer console, so a user can run **decoder-only, keyer-only,
  or both**. SUPERSEDES the 2026-06-18 "decoder pane inside the one
  `CwConsolePanel`" decision. Split:
  - **CW keyer console** (`CwConsolePanel.qml`, shipped) — sidetone/keyer
    controls + the station-token **macro grid** (§4). RX-decode placeholder
    pane is REMOVED (not used).
  - **CW decoder panel** (`CwDecoderPanel.qml`, new) — RX-only: decoded-text
    pane + WPM readout + AFC-lock indicator + decoder knobs (§3). Own
    "CW Dec" header chip alongside the existing "CW" chip.
- **REVISED 2026-06-23 — CW pitch is bidirectional and on-the-fly.** The
  decoder's AFC center is BOUND to the unified `WdspEngine::cwPitchHz`
  (the single source of truth, 200–1500 Hz, `dsp/cwPitchHz`, TCI-synced,
  shared by Tuning panel + CW tab + carrier offset + sidetone). Changing
  pitch in Lyra moves the decoder's AFC seed; the decoder's AFC lock can
  write the refined center BACK to `cwPitchHz` (so the radio re-centers to
  what's actually being copied). NO standalone decoder "Tone Hz" knob.
  AFC range/squelch/threshold/NB/DSP-filter stay decoder-local.
- **NEW 2026-06-23 — optional RX-WPM → keyer-speed coupling.** A decoder
  toggle (default OFF): when on, the decoder's adaptive RX-WPM readout
  drives the CW keyer's send speed so you answer at the other station's
  speed automatically. Off by default (don't surprise-change send speed);
  one-way decoder→keyer; debounced/rounded so it doesn't jitter the keyer.
- **Macros = station tokens only.** `{MYCALL}/{MYGRID}/{MYNAME}/{MYRIG}/
  {MYANT}/{MYPWR}/{MYSTATE}/{MYCNTY}` from Lyra Settings → Station + plain
  text. NO worked-station tokens (`{CALL}/{NAME}/{RST}/{NR}`), serial,
  Auto-Log-73, or logger integration — Lyra has no QSO log.
- **Reuse the panadapter** for tuning — NO separate CW waterfall in the
  decoder panel. The panel shows decoded text + WPM readout + AFC-lock
  indicator + knobs only.
- **No external source.** Lyra taps its own post-demod RX audio in-process
  at fixed 48 kHz (§2). NO FLdigi / Hamlib / external sound device /
  Web-Audio front-end / sample-rate negotiation (all of that in the
  SDRLogger+ original is DROPPED — Lyra has the audio natively; the
  SDRLogger+ sibling only tied in off TCI because it had no native RX).
- **CWU/CWL ONLY (correctness, not just scope — operator 2026-06-23).**
  Decode runs ONLY in CW mode. This is a fidelity requirement, not a
  convenience cap: (1) CW mode bypasses the SSB DSP chain a user may have
  configured (NR / EQ / passband shaping) that would smear the tone the
  timing classifier depends on; (2) the CW pitch + AFC-center machinery
  only has meaning in CW mode. Gating to CWU/CWL hands the decoder a clean,
  predictable signal by construction. Tap PRE-RX-EQ as well (§2) so a
  user's RX-EQ curve (which still runs in CW mode) can't distort the
  decode. Decoding in SSB/AM is explicitly NOT supported.
- **Build order revised** (§5): decoder DSP + audio tap first, then the
  separate decoder panel, then the keyer-console macro grid.
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

## 3. Decoder panel (`CwDecoderPanel.qml`, new + a CwDecoder context obj)
A SEPARATE floating panel + "CW Dec" header chip (sibling of the CW keyer
console — independent: decoder-only / keyer-only / both). Contents:
- **Decoded-text pane** (scrolling, monospace, font-size + color like
  SDRLogger+; clear button).
- **WPM readout** + **AFC-lock indicator** (— / locked Hz).
- **Decoder knobs:** decode on/off; AFC range; Squelch; Threshold;
  Noise-blanker + DSP-filter toggles. (Tuning is on the main panadapter —
  no panel waterfall.)
- **NO standalone tone knob** — AFC center is bound to the unified
  `WdspEngine::cwPitchHz` (bidirectional: pitch change seeds AFC; AFC lock
  writes refined center back to `cwPitchHz`, debounced).
- **"Match TX speed to RX WPM" toggle** (default OFF) — when on, the
  adaptive RX-WPM drives the CW keyer send speed (one-way decoder→keyer,
  rounded/debounced).
- Persist decoder state via QSettings (`cw/decoder*`) — standalone, NOT in
  the TX/RX profile (#49), mirroring the RX-EQ standalone-state pattern.

The **macro grid stays on the keyer console** (`CwConsolePanel.qml`, §4),
not in the decoder panel.

## 4. Keyer-console macro grid (`CwConsolePanel.qml`, shipped shell)
- Remove the unused "RX decoder — coming with CW-5" placeholder pane.
- **Macro grid:** 16 slots (label + text), click=send (→ `sendCw` after
  station-token expansion), right-click=edit; station-token insert buttons;
  factory-reset. Persist via QSettings (`cw/macros`).
- Station-token expansion reads Lyra's operator/station settings.

## 5. Build order (within CW-5)
- **CW-5a** — `CwDecoder` DSP class (pure, unit-tested vs synthetic CW) +
  the verified CW-audio tap. Bench: decode real off-air CW → text + WPM +
  AFC lock; mode-gated; zero RX-audio impact when off.
- **CW-5b** — separate `CwDecoderPanel.qml` + "CW Dec" chip + knobs (wire
  `CwDecoder`); CW-pitch bidirectional bind; optional RX-WPM→keyer toggle.
- **CW-5c** — the 16-slot station-token macro grid on the keyer console →
  `sendCw`.
- Bench decoder + keyer independently and together, then ship.

## Out of scope (noted)
Worked-station tokens / serial / Auto-Log-73 (no logger); a console CW
waterfall (reuse panadapter); mid-text `>`/`<` speed + `|prosign|` macro
syntax (EESDR macro-text — a CW follow-on); external Winkeyer (#171).
