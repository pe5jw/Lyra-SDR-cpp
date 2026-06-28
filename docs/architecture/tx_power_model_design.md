# TX power model + fine drive — design (A+B arc)

Status: **design drafted, awaiting operator go to build.** Date: 2026-06-28.
Operator-reported problem + RTL-grounded root cause + the A+B fix the operator
scoped ("A + B together").

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

### A — continuous fine drive (UNIFIED, 2026-06-28 — collapses the old A1/A2 split)

**Key finding from the operator's screenshot + code:** Lyra ALREADY applies a
per-sample amplitude scalar to the outbound TX I/Q at the **EP2 wire-pack stage**
— the cos² MOX-edge fade (a Lyra invention; `hl2_stream.cpp:381-382` "host-side
cos² fade", the live fade driven from `hl2_stream`, NOT the empty
`src/tx/MoxEdgeFade.h` Phase-1 stub). Both the **tune carrier I/Q and the SSB
modulation I/Q** flow through this same wire-pack multiply. The Waterfall-ID
"digital drive" level (0.060, "≈¼ digital drive") is the same digital-amplitude
notion already in the UI.

⇒ **A is ONE change, not two:** a continuous **digital drive scalar
`txDriveAmp_ ∈ [0,1]`** multiplied into that existing wire-pack envelope
(alongside the cos² fade factor). It scales tune AND SSB AND all modes uniformly,
post-WDSP/ALC (ALC can't fight it), with the cos² fade preserved. This replaces
the dead-zone AD9866-byte path as the operator's drive control:

- The AD9866 16-step `drive_level` is pinned at a sensible high coarse (so the
  hardware isn't the limiter); `txDriveAmp_` is the continuous fine control.
- **Drive %→amp:** power ∝ amp², so `amp = sqrt(pct/100)` gives a power-linear
  "% of power" feel — what the operator wants (1 % ⇒ ~1 % power, smoothly to ~0).
- Same scalar serves TUN (replaces the fixed `kMaxToneMag` path) and SSB, so the
  #95 tune-mode controls keep working but finally move power continuously.
- The Max-cap becomes a ceiling on `txDriveAmp_` (continuous), not the coarse
  byte — so "limit to X" actually limits.

Exact insertion = the EP2 LRIQ TX-I/Q pack in `write_main_loop_hl2`
(folded there per task #121/#122); pin the line at build. ALC-safe by being the
last multiply before int16 quantization.

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

1. **A — continuous digital drive scalar (unified).** Add `txDriveAmp_` and
   multiply it into the existing EP2 wire-pack TX-I/Q envelope (the cos² fade
   site). AD9866 `drive_level` pinned high-coarse. Wire the drive % + the Max-cap
   to `txDriveAmp_` via `amp = sqrt(pct/100)`. Covers tune + SSB + all modes in
   ONE place; unsticks the tune slider, fixes the over-drive, makes the cap a real
   continuous ceiling. Bench: 1 % ⇒ genuine sub-watt, smooth, slider-responsive.
2. **B — per-radio power model.** `ratedPowerW` (HL2≈5) + coupler-cal as named
   per-radio values (becomes the §6.7 caps struct). Re-express drive as **% of
   rated power** + the Max-cap in **watts**, both referencing `ratedPowerW`; meter
   full-scale references it. Adding a radio = adding its `{ratedPowerW, cal}`.

(The old A1/A2/B1/B2 split is superseded — the wire-pack-scalar finding made A one
change, and B is cleanest as one model layer on top of it.)

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
