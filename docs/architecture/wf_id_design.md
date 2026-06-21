# TX Waterfall Callsign ID — Auto-ID (#175) design / scoping

**Status:** SCOPED 2026-06-21, operator-spec LOCKED. Build not started.
Method-agnostic Auto-ID subsystem; **waterfall-image** method now, **CW**
method later (lands with #173 CW-5). RX1/TX, HL2/HL2+.

---

## 1. What it is
Render the operator's callsign as a **readable raster image in the SSB
passband** — a dedicated, self-keyed ID burst so receiving stations can read
the call on their spectrum/waterfall. ESSB-community "waterfall ID."

## 2. Locked behavior (operator spec)
- **STANDALONE, self-keyed** — NOT mixed with voice. When it fires it keys TX,
  transmits ONLY the raster (mic/voice muted), then unkeys. (Operator: mixing
  with voice is unreadable on the waterfall + invites intermod/ALC issues.)
- **Flat TX path during the ID** — bypass speech DSP (TX EQ / compressor /
  combinator / PHROT / leveler) exactly as DIGU/DIGL do; keep **ALC** as the
  safety limiter. Precise multitone raster must not be distorted. "Do as
  digital does."
- Routes **through the PTT/TX keying path** so SWR-cut, TX-timeout, ATT-on-TX,
  and the keydown/keyup ramp all apply (no TX-safety back-door).
- **Cadence:** toggle ON → fire one ID now + start counter. Interval spin
  **0–20 min**: `0` = once on engage (no repeat); `1–20` = auto every N min
  while armed.
- **Defer-while-keyed:** if an ID comes due during the operator's own voice
  over, WAIT for unkey, then fire the standalone burst (never interrupt/overlap).
- **Modes:** SSB / AM / DSB / SAM only. Skipped for CW / FM / digital.
- **Re-arm every session + on band change (safety):** `wfIdEnabled` is **never
  persisted** — OFF on every app start; also forced OFF on stream **Stop**, app
  close, **AND any band change**. The operator must consciously re-enable each
  session / after any Stop / after every band change. Rationale for band-change
  disarm: after switching bands you may not be tuned up / SWR-checked yet — it
  must not auto-key on the new band before you're ready. No forgotten,
  unready, or wrong-band unattended ID.
- **11M / CB lockout:** if TX freq ∈ **26.965–27.405 MHz**, the ID **never
  keys**, and the Settings toggle is **greyed/locked** while tuned there. A ham
  callsign must never go out on CB.
- **Anti-abuse:** the ID text is ONLY ever `Prefs::callsign` (Settings →
  Hardware). No free-text field anywhere, no external trigger path. Blank call
  → feature inert.
- **Color:** none — the color is the *receiver's* waterfall palette, not ours
  to set. Render crisp **solid single-intensity** lettering; strength
  (brightness/contrast) is the bench-tuned level.

## 3. Architecture — method-agnostic Auto-ID
Shared (built now, reused by CW later): the **arm/counter**, **defer-while-keyed**,
**11M lockout**, **mode gate**, **non-persist/auto-clear**, and the
**self-keyed-flat-TX orchestration**. The **method** plugs in:
- `image` (now): the raster generator below.
- `cw` (later, #173): sends the call as Morse on the same keyed-burst plumbing.
Prefs: `wfIdEnabled` (non-persist) + `wfIdIntervalMin` (0–20) + `wfIdMethod`.

## 4. Build increments
1. **Raster generator (standalone, inert).** A small bitmap font (A–Z, 0–9,
   `/`, `-`, space) + `WaterfallId` that renders `callsign` → a mono float
   audio buffer at a given sample rate: per time-column, sum sines at the row
   frequencies of "on" pixels; sweep columns L→R. Band auto-fits the top of the
   current TX passband. Pure C++, unit-testable, NO wiring. (CTUNE-step-1-style
   safe first cut.)
2. **Auto-ID orchestrator + keyed-burst.** Self-keyed standalone TX: mute mic /
   feed the generator as the sole TX audio, force flat TX (digital-equivalent),
   key → paint → unkey, through the existing keying + safety path. Arm/counter/
   defer-while-keyed/mode-gate.
3. **Settings UI + lockout.** Settings → TX "Waterfall ID" group: enable toggle
   + interval spin (0–20) + read-only Hardware-call display; greyed on 11M and
   when call blank. Prefs (non-persist enable).
4. **Bench-tune.** Level + band placement for RX-waterfall readability
   (the one bench-gated unknown — sensible default, operator confirms on-air).

## 5. Integration points (to confirm at build — agent scoping choked on
## context size; map by hand)
- **TX audio input / mic-source swap seam:** mic path is
  Hl2Ep6MicSource/TciMicSource → TxDspWorker → TxChannel (WDSP TXA) → EP2 TX I/Q
  (`hl2_stream` `registerTxControl`/`TxControl`, ~h:1163/539/275). Need the
  cleanest seam to feed a synthetic mono buffer as the SOLE TX source for the
  burst (mute mic, feed generator).
- **Flat-TX-like-digital:** `wdsp_engine.cpp` `setMode` gates digital at
  DIGU(7)/DIGL(9); RX-EQ digital bypass `rxEqModeBypass_` (~h:1740); TX EQ /
  PHROT have the same DIGU/DIGL auto-off (~:1426 "on for DIGU/DIGL"). Replicate
  that flat config for the burst; ALC stays.
- **Programmatic keying:** `hl2_stream` `setMox` + the TUN orchestrator in
  `main.cpp` (~880–925, `tuneDriveMode`) is the closest precedent for an
  auto-keyed transmission; honor its keydown/keyup ordering. Detect operator
  voice MOX to implement defer-while-keyed.
- **Band/freq for lockout:** TX freq via `rx1FreqHz_`/`pushEffectiveTxFreq`;
  11m/CB window 26.965–27.405 MHz (matches the "Show 11m/CB band row" text).
- **Settings/Prefs:** TX tab in `settingsdialog.cpp`; Prefs Q_PROPERTY +
  QSettings pattern (skip the QSettings write for `wfIdEnabled` to keep it
  non-persistent).

## 6. Notes
- Auto-keying = Lyra transmits on a timer when armed (the intent). Guardrails:
  re-arm-each-session + 11M lockout + routes through the safety FSM.
- It's a *visual courtesy* ID — does NOT replace the legal voice/CW station ID
  (tooltip note).
- Source: `Prefs::callsign` (settingsdialog.cpp:1033). See [[project-lyra-cpp-tx]],
  [[project-lyra-cpp-cw]] (CW method), [[project-lyra-cpp-eq]] (digital bypass).
