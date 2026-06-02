# Metering deep audit — reference vs Lyra (Task #71, 2026-06-02)

Operator flag (2026-06-01 EOD, re-raised 2026-06-02 AM): Lyra's
TX-side Mic / ALC / COMP meters "don't look correct or act
correct."  This document is the verified-not-guessed audit
covering the **full** RX + TX metering chain — what the
reference (Thetis 2.10.3.13) reads, scales, smooths and renders,
vs what Lyra currently does — so we can land a targeted fix
instead of guessing.

Methodology: two parallel read-only research agents (one on
each tree), then the most-suspicious claim verified directly
against the WDSP source before declaring a bug.  No code
changed; this is the dossier for the discussion that drives
the fix commit.

Reference name appears in this file because it lives in
`docs/architecture/refs/` (the no-attribution rule applies to
shipped code/UI/commits, NOT to research provenance — that is
why `refs/` exists; cf. CLAUDE.md §2).

---

## TL;DR — what the operator is seeing, and why

**One root cause explains every "doesn't look correct" symptom
the operator described.**  `tx_channel.cpp:450/459/468`
applies an extra `20·log10(...)` to a value that WDSP already
returns in dB — so MIC pegs at the −200 dB sentinel (renders
`"—"` even while voice is hitting the modulator), and ALC /
COMP rail the bar at full red showing `"−200 dB"` text
regardless of actual compression.

The remaining items (5 of them) are secondary divergences from
the reference's metering posture — semantic label mismatch,
slightly over-smoothed ballistic, fewer meter modes exposed,
and two deferred-cal items already on the backlog.  None of
them produce a stuck-at-rail symptom; the math bug is the
smoking gun.

---

## §1.  The smoking gun — TX meter math is double-logged

### What WDSP actually returns

WDSP `meter.c` lines 96-105 (`D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/meter.c`):

```c
// ON state — running meter
a->result[a->enum_av]   = 10.0 * mlog10 (a->avg  + 1.0e-40);    // dBFS
a->result[a->enum_pk]   = 10.0 * mlog10 (a->peak + 1.0e-40);    // dBFS
a->result[a->enum_gain] = 20.0 * mlog10 (*a->pgain + 1.0e-40);  // dB
// OFF state — meter not running
a->result[a->enum_av]   = -400.0;   // sentinel
a->result[a->enum_pk]   = -400.0;
a->result[a->enum_gain] =   +0.0;
```

So `GetTXAMeter(channel, TXA_MIC_PK)` returns a number **already
in dB** (typically negative, e.g. `−12.0` for a mic peaking 12 dB
below clip).  Same for `TXA_LVLR_GAIN` and `TXA_ALC_GAIN`: the
linear gain is already converted to dB inside WDSP.  Off-state
sentinel is `−400.0` for level meters, `0.0` for gain meters.

### What Lyra does on top

`src/tx_channel.cpp:442-469`:

```cpp
double TxChannel::micPeakDbFs() const {
    ...
    const double pk = api.GetTXAMeter(channel_, /*TXA_MIC_PK=*/0);
    return 20.0 * std::log10(std::max(pk, 1e-10));   // ← BUG
}

double TxChannel::levelerGainDb() const {
    ...
    const double g = api.GetTXAMeter(channel_, /*TXA_LVLR_GAIN=*/6);
    return 20.0 * std::log10(std::max(g, 1e-10));    // ← BUG
}

double TxChannel::alcGainDb() const {
    ...
    const double g = api.GetTXAMeter(channel_, /*TXA_ALC_GAIN=*/14);
    return 20.0 * std::log10(std::max(g, 1e-10));    // ← BUG
}
```

The intent was clearly "convert linear → dB" — but `pk`/`g` is
already in dB, not linear.  The extra `log10` on an already-dB
value runs the floor-clamp at `1e-10`:

* **MIC PK** is typically negative in dB (the mic is below clip).
  `max(−12, 1e−10) = 1e−10`; `log10(1e−10) = −10`; `× 20 = −200`.
  So the reading is **pinned at −200 dB any time the mic is below
  full scale** — i.e. always, in practice.
* **LVLR_GAIN / ALC_GAIN** in dB is ≤ 0 during compression.  Same
  trap: clamps at `1e−10`, output `−200`.
* On the off-state sentinel (−400 from WDSP): `max(−400, 1e−10) =
  1e−10`; output `−200`.  So MIC reads `−200` whether the channel
  is open or not.

### What the operator sees

* **MIC**: `computeMic` at `metermodel.cpp:1392` has
  `valid = !std::isnan(raw) && raw > −190.0`.  A `raw` of `−200`
  fails the check → `text_ = "—"`, level falls to floor.  MIC
  bar stays dead even while you're talking into the mic.
* **ALC** (`computeAlc` at `metermodel.cpp:1479-1506`): the
  `valid` gate is only `!std::isnan(raw)` (no `> −190` clamp), so
  `−200` is treated as valid; `reduction = −(−200) = +200`;
  `n = clamp(200 / 20) = 1.0`.  Bar pegged at full red,
  text `"−200.0 dB"`.
* **COMP** (`computeComp` at `metermodel.cpp:1432-1459`): identical
  logic to ALC.  Same symptom — pegged at full red, text
  `"−200.0 dB"`.

That is exactly the "doesn't look correct or act correct" failure
mode the operator described.

### The fix

Three one-line corrections in `tx_channel.cpp`:

```cpp
double TxChannel::micPeakDbFs() const {
    ...
    const double pk = api.GetTXAMeter(channel_, /*TXA_MIC_PK=*/0);
    return pk;   // WDSP already returns dBFS
}

double TxChannel::levelerGainDb() const {
    ...
    return api.GetTXAMeter(channel_, /*TXA_LVLR_GAIN=*/6);  // dB
}

double TxChannel::alcGainDb() const {
    ...
    return api.GetTXAMeter(channel_, /*TXA_ALC_GAIN=*/14);  // dB
}
```

Two follow-on corrections (defensive, not strictly required for
correctness):

* WDSP off-state sentinel is `−400` for level meters, not `−200`.
  Align the `if (!opened_ || !wdsp_) return −200.0` early-returns
  with the WDSP convention: `return −400.0` for `micPeakDbFs`,
  `return 0.0` for the gain accessors.  This lets the meter
  validity gates use one consistent floor.
* `computeMic` validity is `raw > −190.0` but the off-state
  sentinel is now `−400`; widen to `raw > −300.0` so the
  "channel-not-open" path renders `"—"` correctly.

---

## §2.  Semantic mismatch — "COMP" reads the leveler, not the compressor

Reference TX chain (TXA.c) has **three distinct dynamics blocks**
in series — Leveler (input wcpagc mode-5) → CFC (5-band feedback
compressor) → Compressor (compress.c output peak limiter) → ALC
(output wcpagc mode-5) — each with its own meter.

The reference operator-facing `MeterTXMode` (enums.cs:176-195)
exposes ALL of them as separate picker entries:

| Mode | WDSP source | What operator sees |
|---|---|---|
| `LEVELER` | `TXA_LVLR_PK` / `TXA_LVLR_AV` | Leveler **output level** |
| `LVL_G` | `TXA_LVLR_GAIN` | Leveler **gain reduction** |
| `CFC_PK` | `TXA_CFC_PK` / `TXA_CFC_AV` | CFC output level |
| `CFC_G` | `TXA_CFC_GAIN` | CFC gain reduction |
| `COMP` | `TXA_COMP_PK` / `TXA_COMP_AV` | Compressor output level |
| `ALC` | `TXA_ALC_PK` / `TXA_ALC_AV` | ALC output level |
| `ALC_G` | `TXA_ALC_GAIN` | ALC gain reduction |
| `ALC_GROUP` | `ALC_PK` + `ALC_GAIN` summed | Composite |

Lyra's `MeterModel::Source` enum (`metermodel.h:45-55`) collapses
this to one TX entry labeled `COMP` that routes to
`TXA_LVLR_GAIN` (`tx_channel.cpp:458`).  So the operator clicking
"COMP" actually sees **leveler gain reduction**, not compressor
output.

Three reasonable options:

* **(a)** Rename the picker label `COMP` → `LVLR` to match the
  actual signal source.  Smallest change.
* **(b)** Re-route: leave the picker labeled `COMP` but read
  `TXA_COMP_PK` instead.  Requires deciding whether the
  compressor block is even RUN-enabled in the current TX chain
  (it's gated on `compressor.run`; if Lyra hasn't called
  `SetTXACompressorRun(1)` the meter reads `−400`).
* **(c)** Expose the full reference set as separate picker
  entries (LEVELER / LVL_G / CFC_PK / CFC_G / COMP / ALC /
  ALC_G / ALC_GROUP) so the operator can monitor whichever
  stage they care about.  Largest change; matches the reference
  posture; fits the §15.19 v0.2.1 EQ + Combinator scope.

Operator decision — most likely (c) given the §15.19 lock on
"more knobs than Thetis at the operator level," but (a) is the
quick fix if we want to ship the §1 math fix today and defer
the picker expansion to its own commit.

---

## §3.  Ballistic / smoothing divergence (minor)

| Stage | Reference | Lyra | Note |
|---|---|---|---|
| WDSP IIR τ_av | 100 ms | 100 ms (same `wdsp.dll`) | ✓ match |
| WDSP τ_peak_decay | 100 ms | 100 ms | ✓ match |
| UI pump | 50 ms (20 Hz) | 50 ms (`kTickMs`) | ✓ match |
| UI IIR smoothing | none (raw WDSP reading used) | `α = 0.30` per 50 ms tick (`kTelSmooth`) ≈ extra 100 ms τ on top | Lyra over-smooths ~2× |
| UI peak-hold time | ~1 s (operator-tunable) | 800 ms default (`peakHoldMs_`) | ✓ close |
| UI peak decay | ~400 ms (DecayRatio 0.2 / 50 ms) | 250 ms (`kTelPeakDecay = 0.06`) | Lyra decays faster |
| UI max-hold | yes | yes (3 s default, `kMaxDecay = 0.012` ≈ 4 s fall) | ✓ |

Operator-visible effect: Lyra's MIC / ALC / COMP needles feel
slightly more sluggish on the attack and slightly faster on the
peak-fade than the reference.  Not a "broken" symptom; a
"different feel" symptom.  Once §1 lands, decide whether to
match the reference's ballistic exactly or keep Lyra's slightly
heavier smoothing (the operator's taste; not a divergence the
operator has explicitly complained about).

---

## §4.  Missing RX-side AGC-gain meter source

Reference exposes `MeterRXMode.AGC_GAIN` (reads `RXA_AGC_GAIN`,
the dB of gain currently applied by the RX AGC loop — useful
for seeing "is AGC pumping right now").  Lyra's RX-side Source
enum has only `RX_SMETER`; `WdspEngine::agcGainDb()` at
`wdsp_engine.cpp:955-956` already exposes the value, but it is
not wired into `MeterModel::Source`.

Pure additive fix: add `RX_AGC` to the Source enum and a
`computeRxAgc()` method.  Operator decision — probably yes given
the §15.19 "more knobs" posture; small change.

---

## §5.  PWR meter — known deferred (Task #45)

Lyra's `fwdPowerW()` decode (`hl2_stream.cpp:869-876`) uses the
HermesLite ADC scaling: `v = (raw − 6) / 4095 × 3.3`,
`W = v² / 1.5` — matches the reference's per-hardware bridge_volt
table for HL2 (bridge_volt = 1.5 Ω, refvolt = 3.3 V, offset = 6
ADC counts; `console.cs:25244-25279`).  Math is right; the
**3-point per-band operator-cal trim** that adjusts the watts-
on-the-bar against an external meter (operator's Palstar) is
deferred per Task #45.  Not a regression; just calibration
debt.

Per CLAUDE.md §15.26 the operator-bench A/B vs Thetis at 28 MHz
full tune now reads ~5 W on the Palstar (matches Thetis 5.1 W
reference) AND PA-current 1.76 A (matches Thetis 1.8 A) — i.e.
the wire-level math is correct, only the operator-friendly
per-band cal curve is missing.  Task #45 owns this.

---

## §6.  VDD / supply-volts decode — operator empirical re-check needed

Lyra's `hl2SupplyV()` (`hl2_stream.cpp:857-861`) formula
`(raw / 4095) × 5 × (23 / 1.1)` and slot map were verified
working in CLAUDE.md §15.26 ("VDD 12.3 V" operator-confirmed
across the dev sessions; matches the supply-V anchor decoded
from EP6 addr 0x18 C3:C4).

This isn't itself the operator's "doesn't look correct"
complaint — but worth flagging that the slot-map history was
contentious (§15.26 went through a re-map after PA-current
landed on the wrong slot).  Operator should A/B-check VDD on
the next bench session against their PSU display: if it agrees
within 0.1 V we're done; if it drifts we re-derive against the
HL2 Telemetry Probe (Help → HL2 Telemetry Probe).

---

## §7.  Other reference modes Lyra doesn't expose (information-only)

| Reference mode | Reads | Lyra has it? |
|---|---|---|
| `FORWARD_POWER` | EP6 fwd_power ADC → watts | ✓ as `PWR` |
| `REVERSE_POWER` | EP6 rev_power ADC → watts | ✗ not exposed (decoded but no Source entry) |
| `SWR_POWER` | composite SWR + power | ✗ |
| `MIC` | `TXA_MIC_PK` | ✓ (broken per §1) |
| `EQ` | `TXA_EQ_PK` / `_AV` | ✗ (no EQ block in Lyra TXA yet — v0.2.1) |
| `LEVELER` | `TXA_LVLR_PK` / `_AV` | ✗ (Lyra reads `_GAIN` only) |
| `LVL_G` | `TXA_LVLR_GAIN` | ✓ (mislabeled `COMP` — see §2) |
| `CFC_PK` | `TXA_CFC_PK` | ✗ (no CFC block in Lyra TXA yet — v0.2.1) |
| `CFC_G` | `TXA_CFC_GAIN` | ✗ (same) |
| `COMP` | `TXA_COMP_PK` | ✗ (compressor block exists in WDSP but Lyra doesn't enable it; current "COMP" is mislabeled — §2) |
| `ALC` | `TXA_ALC_PK` / `_AV` | ✗ (Lyra reads `_GAIN` only) |
| `ALC_G` | `TXA_ALC_GAIN` | ✓ (correctly labeled `ALC`, but math broken per §1) |
| `ALC_GROUP` | `ALC_PK + ALC_GAIN` | ✗ |
| `SWR` | (1+ρ)/(1−ρ) | ✓ |
| `OFF` | — | n/a |

Reference RX side: `SIGNAL_STRENGTH`, `SIGNAL_AVERAGE`, `AGC_GAIN`,
`AGC_PK`, `AGC_AV`, `ADC_PK`, `ADC_AV`.  Lyra exposes
`SIGNAL_STRENGTH` (as `RX_SMETER`) and nothing else.

---

## §8.  Proposed fix sequence (operator decision required)

Ordered smallest-blast-radius → largest:

1. **§1 math fix (mandatory, smallest possible commit).**  Three
   `return` lines in `tx_channel.cpp` + the two defensive
   sentinel/floor alignments.  Closes the "doesn't look correct"
   operator symptom.  ~10 lines net.

2. **§2 COMP label / routing decision (operator's call).**
   Either rename to `LVLR` (quick) or expand the picker to
   separate LEVELER / LVL_G / COMP / ALC / ALC_G entries.

3. **§4 add RX_AGC Source.**  Pure additive; matches §15.19
   "more knobs" posture.  ~30 lines.

4. **§3 ballistic alignment** (only if operator's ear says
   current smoothing feels wrong after §1 lands; otherwise
   ship as-is).

5. **§5 PWR cal** — already Task #45.  Independent.

6. **§7 expansion to richer mode set** — natural fit with the
   §15.19 v0.2.1 EQ / Combinator work (Tasks #50/#51 add new
   TXA stages with their own meters; the picker grows then).

---

## §9.  Provenance

Reference files cited:
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/meter.c:69-105`
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/TXA.h:50-69`
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/TXA.c:80-93, 224-237, 296-309, 379-392`
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/RXA.h:47-56`
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/wdsp/RXA.c:142-155, 379-392`
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/Console/console.cs:24692-24846, 25143-25289`
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/Console/dsp.cs:950-1057`
* `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/Console/enums.cs:176-195`

Lyra files cited:
* `src/tx_channel.cpp:442-469`
* `src/metermodel.h:45-55, 96-214`
* `src/metermodel.cpp:35-41, 202-203, 1380-1530`
* `src/wdsp_engine.cpp:940-956`
* `src/hl2_stream.cpp:842-882`
* `src/qml/MeterPanel.qml:66-81, 97+`

Audit ran 2026-06-02 06:30-07:00 EDT.  Two parallel research
agents (one per tree), then the smoking-gun claim verified
directly against the WDSP source before declaring the bug.
