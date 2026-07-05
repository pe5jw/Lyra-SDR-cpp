# Lyra-cpp — OC Control (editable per-band J16 band-filtering) design

**Status:** LOCKED DESIGN v2 (2026-07-05) — red-team reconciled + operator-decided (§7). NO code yet; Stage 1 (`OcControl` core) is next, UI stage awaits the operator's Thetis screenshot.
**Operator directive (2026-07-05):** port Thetis's OC Control to Lyra for
**ANAN and BrickSDR** band-following (external amps / tuners / filter boards),
plus the HL2 HardRock-50 / Icom AH-4 support. "Get that setup correctly first."
**Reference:** Thetis 2.10.3.13 (`D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13`) +
HL2+ ak4951v4 gateware RTL (`Y:\Claude local\_hl2src`). Study-and-implement-native;
open-attribution per `THETIS_DIRECT_PORT_PLAN.md` provenance rules.

---

## 0. Why this (and how it reconciles with the HardRock/AH-4 finding)

- The HL2+ **HardRock-50 / AH-4** support is a **gateware** feature (the `extamp`/
  `exttuner` modules keyed off the TX-freq register) — see
  `project_lyra_cpp_hardrock_atu.md`. That is HL2-specific and needs NO OC table.
- **ANAN and BrickSDR have no such gateware.** Their band-following external
  amps / tuners / filter boards are driven by the **generic HPSDR J16 open-
  collector (OC) outputs** — the exact mechanism Thetis's "OC Control" tab
  configures. So the OC table is the **portable multi-hardware** path; the
  gateware amp/ATU support is the HL2 bonus. No contradiction.
- On HL2 itself the OC pins drive the N2ADR filter board (+ any OC accessory),
  so this feature is **benchable on the operator's HL2 today** (relays audibly
  follow the band) even though ANAN/Brick are design-only until a tester has one.

## 1. Current Lyra state (what exists)

- **`src/bands.cpp:85 n2adrOcPattern(bandIndex, transmitting)`** — a single
  **hard-coded** N2ADR preset (11 amateur bands 160m–6m; RX adds pin-7 3 MHz HPF
  above 80m; TX = band-select only). NOT operator-editable.
- **`src/hl2_stream.cpp:4370 updateOcPattern(bool transmitting)`** — gated behind
  one `filterBoardEnabled_` bool (`hw/filterBoard` QSetting). Looks up
  `n2adrOcPattern(bandIndexForFreq(rx1FreqHz_), transmitting)`, writes
  `lyra::wire::prn->oc_output = pattern` and atomically stores
  `ocC2_ = (pattern<<1)&0xFE`. Called on: open-seed (281/1092), band change (1617),
  **TX edge `updateOcPattern(true)` (2846)**, **RX edge `updateOcPattern(false)`
  (2876/3085/1255)**.
- **Wire composer**: frame-0 (C0=0x00) C2 = `(prn->oc_output << 1) & 0xFE`.
  **This is already byte-identical to Thetis** (`networkproto1.c:621`/`:950`
  `C2 = (cw.eer & 1) | ((oc_output<<1) & 0xFE)`). ⇒ **no wire-layer change needed**.
- **Settings UI**: Hardware tab has an "Enable external filter board (N2ADR /
  compatible)" checkbox + a live OC-pin readout ("Live / Band table RX / TX").
- **Family model**: `src/wire/RadioNet.h` has `enum class HPSDRHW`
  (Atlas/Hermes/HermesII/Angelia/Orion/OrionMKII/**HermesLite**/Saturn…) and a
  family-parameterized net struct with per-family `nddc` — the hook for
  per-family routing.
- **TR sequencing**: the TX arc already ships operator-tunable TR-sequencing
  delays (rf_delay / mox_delay / ptt_out_delay) and MOX-edge ordering
  (see `project_lyra_cpp_tx.md`).

**So the port is mostly: (a) replace the hard-coded preset with an editable
per-band table + gating layer, (b) build the editor UI, (c) confirm the MOX-edge
safety timing, (d) route per family.** The wire byte is already correct.

## 2. Thetis reference — the authoritative facts (from the 2 deep-dives)

### 2.1 Data model (`Console/HPSDR/Penny.cs`)
- Four `byte[41]` per-band tables (index = `band - B160M`): `RXABitMasks`,
  `TXABitMasks`, `RXBBitMasks`, `TXBBitMasks` (A = VFO-A path, B = VFO-B/split).
- `TXPinAction[3][7]`, `TXPinPA[3][7]`, `RXPinPA[3][7]` — indexed `[group][pin]`.
- Groups (`Penny.getGroup`): **0 = HF** (GEN,160m–2m,WWV), **1 = VHF** (VHF0–13),
  **2 = SWL** (broadcast). Pin N ↔ bit N-1 (7 pins).
- Scalars: `SplitPins` (bool), `RxABitMask` (0xf=4×3 / 0x7=3×4), `BandBBitMask`
  (0x70 / 0x78), `VFOBTX` (bool).
- `TXPinActions` enum: `MOX / TUNE / TWOTONE / MOX_TUNE / MOX_TWOTONE /
  TUNE_TWOTONE / MOX_TUNE_TWOTONE` (combo index 0–6).
- Defaults: all masks 0, all pin-actions = `MOX_TUNE_TWOTONE`, all xPA = false.

### 2.2 The single emit choke — `Penny.UpdateExtCtrl(band, bandb, tx, tune, twoTone, pa)`
1. `idx=band-B160M; idxb=bandb-B160M`; out of range ⇒ `bits=0`.
2. Pick raw byte:
   - **Split on**: `bits = tx ? (TXA[idx]&RxABitMask)|TXB[idxb]
     : (RXA[idx]&RxABitMask)|RXB[idxb]`.
   - **HermesLite, no split**: TX ⇒ `TXA[VFOBTX?idxb:idx]`; RX ⇒ pick the *higher*
     band when RX2 on (`idxb>idx ? RXA[idxb] : RXA[idx]`).
   - **Non-HL, no split**: `tx&&VFOBTX→TXA[idxb]; tx→TXA[idx]; else RXA[idx]`.
3. Gate per pin:
   - TX ⇒ `adjustForTXAction`: per pin, `group=getGroup(band)`, read
     `TXPinAction[group][pin]`; if `TXPinPA[group][pin]` require `pa`; then keep the
     pin only if the current (mox/tune/twoTone) state matches the pin's action.
   - RX ⇒ `adjustForRX`: per pin, if `RXPinPA[group][pin]` require `pa`, else pass.
4. **Emit only on change** (`bits != m_nOldBits`) ⇒ `NetworkIO.SetOCBits(bits)`;
   return `bits` (fed to the LED strip).
- `ExtCtrlEnable(false)` ⇒ `SetOCBits(0)`.

### 2.3 Triggers (recompute+emit): band/freq change (A **and** B), MOX up/down,
TUN toggle, 2-Tone toggle, external-PA toggle, **any OC-table edit**.

### 2.4 Amp-safety MOX timing (`console.cs` ~30269–30389)
- **Keydown**: `HdwMOXChanged(tx=true)` emits the **TX OC pattern + T/R relay +
  PTT first** → `Thread.Sleep(rf_delay)` (default **30 ms**) → **then** start the
  transmitter. OC/relay **lead** RF.
- **Keyup**: stop the transmitter **first** → `Thread.Sleep(mox_delay)` (**10 ms**)
  → `HdwMOXChanged(tx=false)` restores the **RX OC pattern** + drops relay/PTT →
  `Thread.Sleep(ptt_out_delay)` (**20 ms**) → RX back on.
- **Invariant: RF never overlaps a stale band-select.** This is the amp protection.
- Hot switching: TX-pin checkboxes are greyed during TX unless "Allow Hot
  Switching" is on; `AllowHotSwitchingForOCTXPins`.

### 2.5 Wire encoding per family
| | P1 (HL2 + ANAN, one path) | P2 (Brick / ANAN-G2) |
|---|---|---|
| OC location | frame C0=0, **C2 = `(oc_output<<1)&0xFE`** (bit0=EER) | High-Priority pkt (port 1027) **byte `[1401] = (oc_output<<1)&0xfe`** |
| ref | `networkproto1.c:621`/`:950` | `network.c:1018` |
- **HL2 gateware note (OPEN — bench):** base HL2 gateware does **not** consume the
  OC bits or break out J16 pins; on HL2 they're only meaningful on filter/
  companion-board gateware, forwarded over **I²C to addr 0x20** (the N2ADR
  MCP23017). The operator *hears* N2ADR relays follow today, so on the ak4951v4
  setup something reaches the board — confirm empirically whether editing the OC
  table changes the relays (vs a frequency-driven gateware path). Does NOT block
  design; it's a bench check.

### 2.6 Persistence
Thetis stores by control name (no compact blob). The Lyra port invents explicit
QSettings keys (per-band RX/TX bytes + pin-action + xPA arrays + scalars).

## 3. Proposed Lyra design

### 3.1 New module: `src/oc/OcControl.{h,cpp}` (`class OcControl : QObject`)
The **single owner** of the OC table + the emit choke (Lyra-native port of the
`Penny.UpdateExtCtrl` math). Model:
```
// Amateur/HF bands index-aligned with amateurBands() (160m..6m + optional 2m).
QVector<quint8> rxA, txA;              // per-band 7-bit masks (HF group v1)
QVector<quint8> rxB, txB;              // VFO-B/split (Phase 2)
TxPinAction     txPinAction[3][7];     // group x pin
bool            txPinPa[3][7], rxPinPa[3][7];
bool  splitPins = false; quint8 rxABitMask = 0x0f, bandBBitMask = 0x70;
bool  enabled   = false;               // master "OC Control enabled"
bool  allowHotSwitch = false;
```
- `quint8 compute(int bandA,int bandB,bool tx,bool tune,bool twoTone,bool pa)` —
  verbatim-logic port of `UpdateExtCtrl` (family-branch on `HPSDRHW`). Pure; no I/O.
- `void refresh()` — reads current (bandA,bandB,mox,tune,twoTone,extPA) from the
  radio state, calls `compute`, and **iff changed** pushes to the wire via the
  existing `HL2Stream` OC sink (below). De-dup like `m_nOldBits`.
- Signals `ocBitsChanged(quint8 rx, quint8 tx, quint8 live)` for the live readout.

### 3.2 Wire sink — reuse, don't rebuild
`OcControl::refresh()` calls a thin `HL2Stream::setOcOutput(quint8 bits)` that does
exactly today's `updateOcPattern` tail: `prn->oc_output = bits; ocC2_.store((bits<<1)&0xFE)`.
**`n2adrOcPattern` becomes a preset-seed helper, not the live source.** The
existing `updateOcPattern(transmitting)` call sites become `OcControl::refresh()`
calls (or `OcControl` subscribes to the same band-change / MOX-edge signals).
`filterBoardEnabled_` is folded into `OcControl::enabled`.

### 3.3 MOX-edge safety timing — align to Thetis, reuse Lyra TR-sequencing
- **Keydown**: emit **TX** OC (`refresh()` with mox=true) BEFORE RF, honoring the
  existing `rf_delay` ordering already in the TX FSM (OC/relay → rf_delay → RF).
- **Keyup**: RF off → `mox_delay` → emit **RX** OC (`refresh()` mox=false) →
  `ptt_out_delay` → RX on.
- Verify against the shipped TX keydown/keyup ordering; the OC swap must sit at the
  §15.25-equivalent slots (OC leads RF; OC-revert trails RF-off). This is the
  amp-damage-critical part and the primary red-team target.

### 3.4 UI — NEW Settings tab **"Filters / BCD"** (operator-locked 2026-07-05)
The Hardware tab is already full, so ALL external band-following controls move to a
new dedicated Settings tab **"Filters / BCD"** (operator-confirmed name). The OC
editor is **INLINE** on the tab (like the Thetis OC-Control grid) but rendered in
**Lyra's design language — fancier than Thetis** (glassy panels, Lyra accent
palette, `LyraComboBox`/`LyraSpinBox`, styled checkboxes, the animated live
pin-state strip). **Move OFF the Hardware tab onto the new tab:**
- **From the current Hardware tab's filter/BCD block:** "Enable external filter
  board (N2ADR / compatible)" checkbox, the "OC outputs — Live / Band table RX/TX"
  readout, the ⚠ Pre-antenna-gate warning line, the "OC patterns…" button, AND the
  **entire USB-BCD block**: "Enable USB-BCD amp band output", the BCD-cable picker
  (FTDI, e.g. `FT7IQ0VM`), the two override checkboxes "60 m uses the 40 m filter
  (BCD 3)" / "11 m uses the 10 m filter (BCD 9)", the "Operate only within max power…"
  reg warning, and the "BCD output — BCD code: N" readout.
- **STAYS on the Hardware tab:** the Radio-connection block, Auto-LNA (creep-back +
  Hold time), and the whole Transmit safety group (TX timeout, Bypass, Enable PA,
  RF-safety, Auto-mute-RX, RX-resume-delay, HW-PTT, Space-bar PTT, Auto-start), plus
  the left column (Operator/Station, Band plan, Band panel, Diagnostics).
- The `usb_bcd` FTDI path (`project_lyra_cpp_bcd.md`) is **relocated, not
  duplicated** — same backend, new home + the §7.5(9) TX-freq-band coherence with OC.

**OC Control section of the new tab** (matches the operator's Thetis OC-Control
screenshot — HF/VHF/SWL sub-tabs, HF live in v1):
- Per-band **J16 Receive Pins** grid + **J16 Transmit Pins** grid (band × 7
  checkboxes), HF group v1 (160m–10m live; 6m/2m rows reserved).
- **Transmit Pin Action** column (per pin 1–7: Mox / Tune / 2-Tone / combo dropdown,
  default `Mox/Tune/2Tone`).
- **Ext PA Control (xPA)** RX + TX per-pin checkboxes (pins 1–7).
- **Split Pins** (Enable + 4×3 / 3×4 radio) — present-but-reserved until RX2 (§7.3).
- **Allow Hot Switching** checkbox.
- Buttons: **Ext Control** (master enable = `OcControl::enabled`), **N2ADR Filter**
  (seeds today's `n2adrOcPattern` map — existing behavior one click away, nothing
  regresses), **HF Reset** (clear the HF grids).
- **Hardware Pin State** 1–7 readout strip (reuse the existing live-OC readout;
  OrangeRed=TX active, GreenYellow=RX — display of last emitted bits). *The key
  operator instrument to verify the table with no amp connected.*

### 3.5 Persistence — QSettings `oc/*`
Explicit keys: `oc/enabled`, `oc/rxA/<band>`, `oc/txA/<band>` (byte per band),
`oc/txPinAction/<g>/<p>`, `oc/txPinPa/<g>/<p>`, `oc/rxPinPa/<g>/<p>`,
`oc/splitPins`, `oc/rxABitMask`, `oc/bandBBitMask`, `oc/allowHotSwitch`.
Migration: if `hw/filterBoard`==true and no `oc/*` exist, seed the N2ADR preset +
`oc/enabled=true` (operator's current behavior preserved on upgrade).

### 3.6 Family routing (`HPSDRHW`) — CORRECTED per red-team (§7 B1)
- **The wire SINK is one path** for HL2 + ANAN-P1 (the existing frame-0 C2). But
  **`compute()`'s raw-byte SELECT is family-branched**: the HermesLite branch has
  the RX2 "pick higher band" pick + the VFOBTX-for-TX pick (§2.2); the non-HL
  ANAN-P1 branch does not. So: one sink, two `compute()` raw-select branches — NOT
  "one code path".
- **P2 (Brick / ANAN-G2)**: OC → HP-packet byte 1401. **Lyra has NO P2 write path
  yet — `radioProtocol` is fixed to `USB`, and even ANAN-**P1** frame composition
  is a `DEFERRED` stub** (`NetworkProto1.cpp` non-HL branch). So "lands with v0.4"
  means the OC sink rides on top of the v0.4 P2/ANAN protocol module **that does
  not yet exist** — NOT a small byte-1401 add. `OcControl::compute` stays
  family-agnostic (the genuinely portable part); document the byte now.
- The HL2 "pick higher band when RX2 on / VFOBTX for TX" branch (§2.2) matters
  because Lyra has RX2 — its **live inputs** complete with the RX2 arc (§7.3).

## 4. Scope proposal (operator decides; red-team weighs in)

**v1 (recommended first cut):** editable **HF** per-band RX+TX 7-pin table +
N2ADR/Reset presets + live pin readout + master enable + MOX-edge safety timing +
xPA + pin-action gating + hot-switch toggle + persistence + P1 family path (HL2/
ANAN). Benchable on the operator's HL2 (watch the N2ADR relays).

**Phase 2:** Split Pins (4×3/3×4) + VFO-B/RX2 filter branch + VHF/SWL groups.

**Phase 3 (with v0.4 P2):** Brick / ANAN-G2 P2 byte-1401 sink.

## 5. Safety posture (mandatory)
- Default **OFF** (or seeded from the existing N2ADR preset only if the operator
  already had `hw/filterBoard` on).
- Live pin-state readout is the no-amp verification instrument.
- User Guide line: "verify wiring + low-power test on every band before keying at
  full output — a wrong OC code into a keyed amp routes TX through the wrong filter."
- The MOX-edge ordering (OC leads RF; OC-revert trails RF-off) is the amp-damage
  interlock — must be red-teamed against the shipped TX FSM before any code.

## 6. Open questions for red-team / operator
1. **MOX-edge ordering**: does the shipped Lyra TX FSM already emit the OC swap at
   the safe slots (OC→rf_delay→RF on keydown; RF-off→mox_delay→OC-revert→ptt_out
   on keyup), or does `updateOcPattern` currently fire at a slightly different
   point? (Primary safety question.)
2. **HL2 OC delivery** (§2.5 bench): on the ak4951v4 gateware, does driving
   `oc_output` reach the N2ADR board (OC→I²C 0x20), or is band-following freq-
   driven? Determines whether v1 is verifiable on the operator's HL2.
3. **Scope**: v1 = HF-only (recommended) vs full (incl. split/VHF/SWL/VFO-B)?
4. **`bandIndexForFreq` vs Thetis `BandByFreq`**: Lyra's band index is amateur-
   band-only (11 bands); Thetis's is 41 bands incl. broadcast/VHF. v1 HF maps
   cleanly; VHF/SWL groups need a band-index extension (Phase 2).
5. **RX2 / VFO-B**: include the HL2 higher-band RX-filter branch in v1?

---

## 7. LOCKED DESIGN v2 — red-team reconciled + operator decisions (2026-07-05)

Both senior red-team agents returned **CONFIRM-WITH-AMENDMENTS, no redesign.** The
amp-safety interlock (the #1 question) was confirmed **already correct in the
shipped TX FSM** and is *stronger* than Thetis (keydown OC → `rf_delay` → RF;
keyup RF-off → `mox_delay`/`ptt_out_delay` → OC-revert). The wire byte is
byte-identical; the `filterBoard→OcControl` migration is safe; the port reuses the
existing safe OC call sites — no TR-sequencing FSM change.

### 7.1 Operator decisions (locked)
- **TX-side band source = follow Thetis's working design exactly** (`VFOBTX` +
  `bandb` → the TX OC pattern comes from the *actual TX frequency*, not RX1).
  Operator: "make it like Thetis — Thetis works, it's also PureSignal-safe by
  design." This closes the cross-band-split wrong-filter-into-a-keyed-amp hazard.
- **Scope = full Thetis-faithful port, built correctly now**, BUT structured so the
  RX2/split-dependent parts **complete cleanly when the RX2 arc lands** — operator:
  "we don't have the 2nd RX yet… build it out so it can be completed in the other
  phase… just make sure it is noted to complete… do what is needed to make this
  correct." Don't block on RX2; make the foundation correct + note the completion.
- **New Settings UI panel required.** Operator will attach a Thetis OC-Control
  screenshot to guide the layout — **request it when starting the UI stage.**

### 7.2 What's LIVE in v1 (benchable now on the operator's HL2)
Full data model (4 × per-band 7-bit masks RXA/TXA/RXB/TXB + `txPinAction[3][7]` +
`txPinPa`/`rxPinPa[3][7]` + `splitPins`/`rxABitMask`/`bandBBitMask`) + the complete
family-branched `compute()` port + N2ADR/Reset presets + Tx-Pin-Action + xPA +
hot-switch + live pin readout + master enable + persistence + the P1 (HL2/ANAN) C2
sink. **TX-side OC band source = the actual TX frequency** (works today for
non-split, where TX band == RX1 band). HF group (the 11 amateur bands via
`bandIndexForFreq`) is the live band axis.

### 7.3 ⚠ COMPLETES WITH RX2 (tasks #96–#100) — built-but-dormant
The full model + `compute()` handle these NOW; only their **live inputs** wait on
RX2 (the second-receiver arc, tasks #96–#100):
- RXB/TXB split arrays + Split-Pins (4×3 / 3×4) live wiring
- `VFOBTX`/`bandb` live TX-band selection on split (the field exists and
  `compute()` already takes `bandB` — no rework; it goes live when split does)
- the RX-side dual-RX "pick the higher band" listening filter (§2.2 HL branch)
`compute(bandA, bandB, tx, tune, twoTone, pa)` takes both bands from day one. When
RX2 ships, wire `bandB`/`splitEnabled_` to the real RX2/split state + enable the
Split-Pins UI and it switches on.

### 7.4 ⚠ RESERVED until band enumerations (VHF / SWL groups)
Lyra has **three disjoint band-index spaces** (`amateurBands()` / `broadcastBands()`
/ `cbBands()`) — NOT one 41-band array. The `[3][7]` group axis (HF/VHF/SWL) ships
with **group 0 (HF) live**; groups 1–2 (VHF/SWL) are reserved until Lyra adds a VHF
(2m) + broadcast band enumeration. QSettings `oc/txPinAction/<g>/<p>` etc. reserve
the group index for them.

### 7.5 LOCKED amendments (fold into implementation — both agents)
1. **De-dup gate seeded to −1** (Thetis `m_nOldBits = -1`), re-armed to −1 on the
   `enabled` toggle — NOT 0. Else the first real emit (incl. legitimate "all pins
   off" = 0) is silently swallowed and the board stays in its power-up state. Do
   NOT carry Lyra's current 0-init `ocPattern_` semantics into the new gate.
2. **Threading:** pin `OcControl` + its de-dup cache to the **Qt-main thread** (the
   FSM's callback thread); wire publication stays via the existing atomic
   `ocC2_.store(...)`. Never call `refresh()` from the EP2/EP6 threads.
3. **compute() family-branched, wire-sink one path** (see corrected §3.6).
4. **Preserve ALL revert call sites** when `n2adrOcPattern` becomes a seed and the
   live source moves to `OcControl::refresh()`: keydown `refresh(tx)` at
   `hl2_stream.cpp:2846`; keyup `refresh(rx)` at `:3085`; the **cancel-path reverts**
   (`:2876` + the mid-`rf_delay` unwind) and the **`close()` mid-TX RX-restore at
   `:1255`** (safety-critical — otherwise the board is left in TX config);
   open-seed `:281/:1092`; band-change `:1617`.
5. **`& 0x7F` mask explicit in `setOcOutput`** (EER lives in C2 bit 0; OC is bits
   7:1; the compose already does `<<1 | cw.eer`). Write raw 7-bit `prn->oc_output`.
6. **Semantics to replicate:** `getGroup(band) < 0` forces that pin **off** (not
   skip); v1 wires `twoTone` to literal **false** (no two-tone TX exists in Lyra
   yet, so TWOTONE-action pins assert only on their tune/tx terms).
7. **Migration:** if `hw/filterBoard`==true and no `oc/*` keys exist, seed the
   N2ADR preset + `oc/enabled=true` — existing operators keep N2ADR-follows-band on
   upgrade. Re-point the Settings filter-board checkbox binding when
   `filterBoardEnabled_` folds into `OcControl::enabled`.
8. **P2/Brick correction (§3.6):** no P2 write path exists; ANAN-P1 composition is a
   stub — Brick/ANAN OC rides the unbuilt v0.4 protocol module, documented only.
9. **USB/BCD coexists** with OC (different physical outputs — J16 pins vs the FTDI
   cable); both follow the band. Apply the same "TX band from the actual TX freq"
   discipline to `usb_bcd::applyForFreq` so a split operator's amp BCD and filter
   board agree on the TX band.

### 7.6 Staged build plan (each stage revertable; bench-gate on the operator's HL2)
- **Stage 1 — `src/oc/OcControl.{h,cpp}`**: data model + full family-branched
  `compute()` + −1-seeded de-dup + Qt-main threading. Pure/unit-testable; no wire.
- **Stage 2 — wire sink + call-site migration**: `HL2Stream::setOcOutput(bits)`
  (`prn->oc_output = bits & 0x7F; ocC2_.store((bits<<1)&0xFE)`); convert **all** the
  §7.5(4) `updateOcPattern` sites to `OcControl::refresh()`; `n2adrOcPattern`→preset
  seed; fold `filterBoardEnabled_`→`OcControl::enabled` + migration. **TX-side OC
  band = the actual TX freq.** RX-only wire bytes byte-identical when disabled or on
  the N2ADR preset.
- **Stage 3 — persistence** (`oc/*` QSettings, reserved keys for split/groups).
- **Stage 4 — Settings UI panel** (⚠ request the operator's Thetis OC-Control
  screenshot first): editable per-band RX/TX 7-pin grid + Tx-Pin-Action + xPA +
  hot-switch + N2ADR/Reset presets + live pin readout. HF live; split/VHF/SWL
  controls present-but-reserved per §7.3/§7.4.
- **Bench gate**: operator watches the N2ADR relays follow the editable table with
  no amp; low-power per-band verify before any keyed amp. (Open bench Q — §6.2:
  confirm driving `oc_output` reaches the ak4951v4 board over I²C 0x20.)

### 7.7 Completion note (do not lose)
The RX2/split live-input wiring (§7.3) is a **required completion item for the RX2
arc (tasks #96–#100)** — when split/RX2 lands, wire `bandB`/`splitEnabled_` into
`OcControl` and enable the Split-Pins UI. The VHF/SWL group axis (§7.4) completes
when the VHF/broadcast band enumerations are added. Both are structurally in place
now; only inputs/UI-exposure wait.
