# TX power model + fine drive — design (A+B arc)

Status: **building — Stage 1 (txgain port, inert) SHIPPED `cc7758b`.** Date: 2026-06-28.
Operator-reported problem + RTL-grounded root cause + the A+B fix the operator
scoped ("A + B together"), modelled on Thetis ("do as Thetis does") and so
PureSignal-safe by construction.

---

## 1. The problem (operator, 2026-06-28)

- At "1 %" drive the HL2 still puts out ~0.78 W (confirmed by LPA100; Lyra's
  PWR meter ~agrees) — enough to slam a 100 W amp to 150–180 W.
- Below that, **the tune-drive slider does nothing** — output stuck at ~1 W.
- "Is the power scale based off 100 W? How do we make it align with different
  radios later?"

## 2. Root cause (RTL + code verified — NOT a 100 W constant)

- **PWR meter watts** = a fixed HL2 coupler formula `((fwd−6)/4095·3.3)²/1.5`
  (`hl2_stream.cpp:1440`). Reads the HL2's real coupler ADC ⇒ agrees with the
  LPA100. It is NOT 100 W-referenced; it's just a single hardcoded HL2 curve
  with no per-band/per-radio scaling. **The meter is ~right; it is not the bug.**
- **Drive %** is a dumb linear map to the wire byte: `raw = round(pct·255/100)`
  (`TxPanel.qml:58`, `main.cpp:959/984`). The full byte is sent
  (`FrameComposer.cpp:457` `C1 = drive_level`).
- **Gateware uses only the top 4 bits** of that byte as `tx_gain` — the AD9866
  TX PGA gain code, **16 fixed steps** (`hl2_rtl_ad9866ctrl.v:135-138`
  `tx_gain <= cmd_data[31:28]`).
  - ⇒ `pct 1…6 % → raw 3…15 → tx_gain nibble = 0`. **The bottom ~6 % of the
    drive slider is a dead zone** — every value sends the same minimum AD9866
    gain. (`pct ≥ 7 %` → raw ≥ 18 → nibble 1 = the first real step.)
  - At minimum AD9866 gain the modulator I/Q is still full-scale, so the HL2 PA
    **floors at ~0.78 W** — the lowest it can key via `drive_level` alone.
- "Stuck regardless of the tune slider" = the bottom dead zone (all `tx_gain=0`)
  plus the Max-cap pinning the raw low. **The tune tone magnitude is held fixed
  at `kMaxToneMag = 0.99999`** (`main.cpp:1331-1337`) and never scaled by the
  tune %, so there is no continuous amplitude knob under the AD9866 floor.

**To go below ~0.78 W you must reduce the digital TX amplitude** (the WDSP TX
I/Q level / the TUN tone magnitude). Lyra has no such control today.

## 3. The fix (A + B, operator-scoped)

### A — continuous fine drive (the Thetis model, txgain.c — PureSignal-safe)

**The reference already solves this exactly, and PS-safe.** Thetis's HL2 power
chain (verified): per-band PA Profile `{Max Power(W), PA Gain(dB)}` →
`RadioVolume = min(drive · PAgainByBand/100 / 93.75, 1.0)` (console.cs:47777) →
drives BOTH `SetOutputPower(RadioVolume·1.02)` (the coarse `drive_level` byte,
the 16-step AD9866 path, PS auto-attenuator tracks it) AND
`cmaster.CMSetTXOutputLevel()` = `SetTXFixedGain(0, RadioVolume·HighSWRScale, …)`
(audio.cs:262-269, cmaster.cs:1124-1127). That `SetTXFixedGain` is a **digital
fixed gain applied to the TX I/Q inside ChannelMaster** — the FINE continuous
control that fills in between the 16 coarse DAC steps and goes below the PA floor.

It is **PureSignal-safe by construction**: the gain lives in `txgain.c`'s
`xtxgain`, run inside the CMaster TX pump where PS taps its reference, so PS
calibrates against the post-gain signal (a post-predistortion host scalar would
break PS — this is not that).

⇒ **A = port `txgain.c`/`txgain.h` into Lyra's CMaster and drive it** (not a
host wire-pack scalar — the cos² wire-pack fade was deleted "do as reference"):

- **Stage 1 (DONE, `cc7758b`):** `wire/TxGain.{h,cpp}` ported (attributed); the
  block is created/pumped/destroyed in CMaster, INERT (unity, run-off → TX
  byte-identical).
- **Stage 2:** compute a `RadioVolume`-style level from the operator drive % /
  per-band Max-Power and feed `SetTXFixedGain(0, lvl, lvl)` + `SetTXFixedGainRun
  (0,1)` (the fine continuous control) alongside the existing `drive_level` byte
  (the coarse). **Drive %→level:** power ∝ level², so `level = sqrt(pct/100)`
  gives a power-linear "% of power" feel (1 % ⇒ ~1 % power, smoothly to ~0).
- The AD9866 16-step `drive_level` stays a sensible high coarse so the hardware
  isn't the limiter; `txgain` is the continuous fine control beneath it.
- The Max-cap becomes a ceiling on the level (continuous), not the coarse byte —
  so "limit to X" actually limits.
- Covers TUN and SSB and all modes uniformly (it's in the one CMaster TX pump),
  so the #95 tune-mode controls keep working but finally move power continuously.

### B — per-radio power model (the "align with different radios" foundation)

First real per-radio power values, structured to become the §6.7 capabilities
struct later (today they're `kDefault*`-style HL2 constants/fields):

- `ratedPowerW` — HL2 ≈ 5.0 (ANAN ≈ 100). Meter full-scale + the reference the
  drive % and the cap are expressed against.
- `fwdPowerCalK` (+ optional per-band 3-point) — the coupler→watts cal, so
  displayed watts are true per radio.
- **Operator surface becomes watts-referenced:** the Max-TX cap in **watts**
  (e.g. "limit to 2 W"), drive as **% of rated power**, meter full-scale =
  `ratedPowerW`. Adding a radio = adding its `{ratedPowerW, fwdPowerCal}` — the
  TX core never changes.

## 4. Staged plan (each bench-gateable; TX-core discipline)

1. **A Stage 1 — port txgain.c, ship INERT (DONE `cc7758b`).** `wire/TxGain.
   {h,cpp}` + the four CMaster sites (create/xtxgain-pump/destroy/SetTXGainSize)
   activated at unity / run-off → `xtxgain` is a no-op, TX byte-identical, RX
   untouched. Bench: confirm normal startup + unchanged TX (provable inert).
2. **A Stage 2 — wire the control.** Compute a `RadioVolume`-style level from the
   operator drive % / Max-Power; feed `SetTXFixedGain(0, lvl, lvl)` +
   `SetTXFixedGainRun(0,1)` (fine) + the `drive_level` byte (coarse), via
   `level = sqrt(pct/100)`. Make the Max-cap a continuous ceiling on the level.
   Bench: 1 % ⇒ genuine sub-watt, smooth, slider-responsive; PS unaffected.
3. **B — per-radio power model.** `ratedPowerW` (HL2≈5) + coupler-cal as named
   per-radio values (becomes the §6.7 caps struct). Re-express drive as **% of
   rated power** + the Max-cap in **watts**, both referencing `ratedPowerW`; meter
   full-scale references it. Adding a radio = adding its `{ratedPowerW, cal}`.
4. **A Stage 4 (optional) — amp-protect.** `txgain`'s `run_amp_protect` +
   `SetAmpProtectADCValue` driven from the HL2 PA-current telemetry Lyra already
   decodes (auto-attenuate on over-current). Off by default.

(The old A1/A2/B1/B2 split + the wire-pack-scalar approach are superseded — the
txgain.c finding unifies A onto the reference's own PS-safe TX gain stage.)

## 5. Constraints / open items
- **PureSignal forward-compat (major future pillar):** PS predistorts TX I/Q
  against a stable emitted amplitude. The digital scalar must be applied where PS
  sees the post-scalar signal, and must NOT change mid-PS-calibration. Static
  per-transmission scalar = fine; flag before PS work.
- **AD9866 step dB** (the 16-step size) not yet pinned — needed to design the
  coarse/fine split precisely (B2). Confirm from the AD9866 datasheet/RTL.
- **No-attribution rule:** reference study lives here/docs only; shipped code in
  first-principles RF terms.

## 6. Reference anchors
- `src/hl2_stream.cpp` (`fwdPowerW` 1440, `setTxDriveLevel` 1780, `maxDriveRaw`
  hl2_stream.h:1457), `src/qml/TxPanel.qml` (drive slider 58/112, tune 320-369),
  `src/main.cpp` (tune orchestrator 901-989, TUN tone mag 1331-1337),
  `src/wire/FrameComposer.cpp` (`set_drive_level` 103, C1 pack 457),
  `Y:/Claude local/_hl2src/hl2_rtl_ad9866ctrl.v:135-138` (tx_gain = top nibble).
