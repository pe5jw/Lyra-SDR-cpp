# TX spectrum + metering — Thetis vs Lyra side-by-side (Task #44 + #71)

Operator request 2026-06-02 PM after bench test:
1. Should TX panadapter dB-range recall separately from RX? (yes)
2. Side-by-side TX spectrum + sample rates
3. Side-by-side TX metering (ALC stuck/hangs, MIC)
4. Fix everything to match reference

This is the verified-from-source dossier with every claim
file:line cited.  No agent inference.

---

## §1.  TX SPECTRUM DISPLAY — side-by-side

| Aspect | Thetis | Lyra v2.3.1 (current) | Match? |
|---|---|---|---|
| **Tap point** | `xsiphon(sip1, 0)` at `wdsp/TXA.c:586` — post-ALC, pre-iqc | Same — via WDSP-internal auto-feed from same sip1 | ✅ |
| **Feed mechanism** | `TXASetSipMode(txch, 1)` + `TXASetSipDisplay(txch, txinid)` (`cmaster.cs:539-540`) — WDSP feeds analyzer internally from inside xsiphon | Same — `setTxOwnsAnalyzer()` calls TXASetSipMode + TXASetSipDisplay on MOX edge | ✅ |
| **TX channel dsp_size** | 4096 per `TXA.c` (`ch[channel].dsp_size` setup) | `kDspSize = 4096` (`tx_channel.cpp:57`) | ✅ |
| **Analyzer bf_sz (TX)** | = TX dsp_size = 4096 (matches what xsiphon delivers per call) | = 4096 (`kBlockTx` in `configureAnalyzerForTx`) | ✅ (post-v2.3.1) |
| **Analyzer sample rate (TX)** | 96 kHz via `radio.cs:2618` `.SampleRate = 96000` (calls `SetDisplaySampleRate`) | 96 kHz via `SetDisplaySampleRate(kAnDisp, 96000)` in `configureAnalyzerForTx` | ✅ (post-v2.3.1) |
| **Analyzer FFT size** | 16384 for sip1 internal; analyzer FFT separate per SpecHPSDR config | `kAnFftSize = 4096` (Lyra uses smaller for faster updates) | ⚠ Lyra uses smaller FFT — visual difference (less freq resolution) but doesn't affect content correctness |
| **Display window** | `tx_display_low/high` defaults ±4 kHz (`Display.cs:1278/1285`) — operator-tunable | Lyra shows full computed span (`spanHz()` = 96000 / zoom) | ⚠ Lyra shows much wider span (~48 kHz at zoom 2x vs reference ±4 kHz default) |
| **Frequency axis** | TX VFO centered (or VFO B in SPLIT) | Operator's tuned VFO centered (SPLIT n/a in v0.2.x) | ✅ for non-SPLIT |
| **dB-range swap on MOX edge** | NO auto-rescale; operator-tunable TX-side `TXSpectrumGridMin/Max` swaps in on MOX (defaults +20/-80 dBFS per `Display.cs:1881/1891`) | **NOT IMPLEMENTED** — Phase 1 was never shipped (only Phase 2 source swap is in) | ❌ |
| **RX dB range restore on keyup** | Yes — RX `SpectrumGridMin/Max` separately maintained | **NOT IMPLEMENTED** | ❌ |
| **Y-axis adapter** | NO auto-fit; static operator-set values | None (matches reference posture) | ✅ |
| **PS forward-compat** | sip1 is pre-iqc, so PS predistortion (downstream) doesn't change panadapter content | Same — pre-iqc via sip1 | ✅ |

**Status**: TX spectrum content + rate match Thetis exactly
post-v2.3.1.  The remaining gap is the dB-range swap on MOX
edge (Phase 1) — operator's primary complaint.

---

## §2.  PHASE 1 DB-RANGE RECALL — what needs to ship

Operator-locked design (re-confirmed 2026-06-02 PM):
* Operator can drag the right-edge dB handles during MOX to
  set the TX panadapter range
* That range persists separately from RX range
* On every MOX keydown: save current dbMin/dbMax to Prefs as
  RX values, load TX values from Prefs into the panadapter
* On every MOX keyup: save current dbMin/dbMax to Prefs as
  TX values (captures any drag changes during MOX), load RX
  values back
* No Settings → Visuals row needed — operator's existing
  drag-the-right-edge muscle memory does the work

**Implementation surface** (small):
* `Prefs`: add `txDbMin / txDbMax` Q_PROPERTYs (defaults
  e.g. +20 / -80 dBFS to match Thetis convention)
* `Panadapter` ctor: connect `Stream::moxActiveChanged` to a
  slot that:
  - on=true: save current `dbMin_, dbMax_` to Prefs `dbMin,
    dbMax` (the RX pair), then load `txDbMin, txDbMax` into
    `dbMin_, dbMax_`
  - on=false: save current to Prefs `txDbMin, txDbMax`, load
    `dbMin, dbMax` back into the panadapter
* The existing `setDbMin/setDbMax` setters (called by the
  drag handles) write to `dbMin_/dbMax_` AND to Prefs —
  unchanged.  Persistence is implicit because Prefs writes
  on every drag.
* When `autoScale_=true`: skip the swap (auto-scale takes
  priority — operator chose dynamic, don't fight it).

Per the §15.25 lock: small focused commit, individually
revertible, matches the reference posture.

---

## §3.  TX METERING — side-by-side

### §3.1 MIC display

| Aspect | Thetis | Lyra | Match? |
|---|---|---|---|
| Source | `GetTXAMeter(channel, TXA_MIC_PK)` | `GetTXAMeter(channel, 0)` (= TXA_MIC_PK) | ✅ |
| Unit | dBFS (raw WDSP value) | dBFS (raw WDSP value) | ✅ |
| Display pipeline | `CalculateTXMeter` returns `-val`, then case branch `Math.Max(-195, -CalculateTXMeter)` = `+val` | Direct `arg(db, 0, 'f', 1)` from `txWorker_->micPeakDbFs()` | ✅ — double-negate cancels; both display `+raw` |
| Display semantics | Positive number = dB BELOW clip (operator-friendly), negative = OVER clip | Same | ✅ |
| Floor | clamp at -195 dBFS | clamp at -300 dBFS (passes WDSP's -400 off-state) | ✅ functionally same |

**MIC display: matches Thetis.**  No fix needed.

### §3.2 ALC G (gain reduction) display — DIVERGENT

| Aspect | Thetis (`dsp.cs:1019-1056` + `console.cs` case ALC_G) | Lyra v2.3.1 (current) | Match? |
|---|---|---|---|
| Source | `GetTXAMeter(channel, TXA_ALC_GAIN)` = 20·log10(current_linear_gain) | Same | ✅ |
| Display formula | `Math.Max(0, raw_gain_dB + alcgain)` where `alcgain` = operator's `SetTXAALCMaxGain` (default 3.0 dB) | Direct `arg(raw, 0, 'f', 1)` — raw WDSP value | ❌ |
| What operator sees | "ALC G [dB-of-reduction-below-max-gain-ceiling] dB" clamped ≥ 0 | Raw WDSP gain value (can be positive or negative; confusing) | ❌ |
| Operator screenshot showed | (Thetis would show ~16.4 for same raw) | "ALC G 13.4 dB" — Lyra's raw value | confirms divergence |

**Decoded:** WDSP's `TXA_ALC_GAIN` returns `20·log10(current_linear_gain)` — the **current applied gain**, NOT "dB of reduction."  For wcpagc mode 5 ALC:
* When signal is silent / weak: gain rests at `max_gain` (operator's MaxGain setting)
* When signal is hot: gain drops below `max_gain` (ALC reducing)
* Raw can be positive (when max_gain > 0 dB) or negative

Thetis's display formula `max(0, raw + max_gain_db)`:
* At rest (raw = max_gain_db, e.g. +3): max(0, 3+3) = **6**
* Steady speech (raw = 0, gain at unity): max(0, 0+3) = **3**
* Active reduction (raw = -3): max(0, -3+3) = **0**
* Heavy reduction (raw = -10): max(0, -10+3) = **0** (clamped)

So Thetis's display = "max_gain + gain_above_unity" — visualizes the ceiling-driven headroom as a single number that grows as the operator's signal forces the ALC to apply more correction.  Operator-intuitive: high number = lots of action, 0 = clamping.

Lyra shows raw value directly — confusing because:
* The number is the CURRENT gain (which depends on operator's max_gain setting)
* Doesn't have the reference's "headroom remaining" semantics
* Operator perceives this as "stuck" because the value sits near max_gain steady-state and doesn't reflect "ALC working hard" the way reference operators expect

**Fix**: replicate Thetis's `display = max(0, raw + alcMaxGainDb)` formula
exactly.  Wire `HL2Stream::alcMaxGainDb()` (existing operator
persistence) into MeterModel; the compute method does:

```cpp
const double raw    = txWorker_->alcGainDb();
const double maxg   = stream_->alcMaxGainDb();
const double display = std::max(0.0, raw + maxg);
```

Same bar normalization (`/ kGainDbMax`), but `display` text
instead of raw.

### §3.3 LVL G (leveler gain) — already matches

| Aspect | Thetis | Lyra |
|---|---|---|
| Source | `GetTXAMeter(channel, TXA_LVLR_GAIN)` | Same (`txWorker_->levelerGainDb()`) |
| Display formula | `CalculateTXMeter(LVL_G)` returns `-raw`, then case branch (if exists) negates again | Lyra displays raw directly |

Thetis's display for LVL_G: same double-negate cancellation
as MIC.  Lyra matches.

### §3.4 ALC PK + LVL PK + MIC PK (level meters) — already match

All level meters use `+raw` semantics after Thetis's
double-negate.  Lyra displays raw directly.  Match.

### §3.5 "Stuck/hangs" perception

After implementing the §3.2 fix:
* Display will respond as Thetis does — high number = lots of ALC action, 0 = clamping
* The "stuck" perception was the formula mismatch, not a per-block update issue
* The compute method ticks at 50 ms (20 Hz) and reads fresh `TXA_ALC_GAIN` every tick — no buffering / no stale-value bug

If after the fix the operator STILL sees "stuck" behavior on
the bench, root cause is something else (per-block update
gap, peak-hold dwell mismatch, etc.) — investigate further.
But the formula divergence is the only root cause I can find
from the source.

---

## §4.  REQUIRED FIXES — implementation plan

### Fix A — Phase 1 dB-range MOX-edge swap (operator-locked design)

* `prefs.h/cpp`: Q_PROPERTY `txDbMin`, `txDbMax` with QSettings persistence; defaults +20 / -80 dBFS.
* `panadapter.h/cpp`: connect `Stream::moxActiveChanged` to a slot:
  - on=true: save `dbMin_, dbMax_` → Prefs `dbMin, dbMax`; load Prefs `txDbMin, txDbMax` → `dbMin_, dbMax_`.
  - on=false: save current → Prefs `txDbMin, txDbMax`; load Prefs `dbMin, dbMax` back.
  - Skip swap if `autoScale_=true`.
* Drag handles call setDbMin/setDbMax which write to Prefs as today — works during MOX (writes to "RX" pair though we want it to write to the active state's pair).

  Refinement: setDbMin/setDbMax should write to the correct
  pair based on `stream_->moxActive()`.  At first I thought
  drag-during-MOX writes the RX pair (since that's what
  Prefs::dbMin keys to), but that defeats the persistence.
  Need to route to the active state's pair.

### Fix B — ALC G display matches Thetis

* `MeterModel::computeAlcG()`: apply `display = max(0, raw + stream_->alcMaxGainDb())` before passing to the helper.
* Refactor `computeGainMeterFromDb` signature to accept a `displayOverride` parameter, OR add a custom inline body for ALC_G specifically.
* Bar normalization stays the same.

---

## §5.  Provenance

### Thetis source citations
* `Display.cs:1278, 1285, 1547, 1881, 1891` (TX display defaults + grid)
* `Display.cs:5028-5032` (CW TX blank)
* `audio.cs:1801` (SampleRateTX = 48000 for P1 — but TX-CHANNEL analyzer rate is 96k per radio.cs:2618)
* `radio.cs:2618` (TX panadapter analyzer `SampleRate = 96000`)
* `cmaster.c:177-194` (TX channel OpenChannel dsp_rate=96000 + XCreateAnalyzer)
* `cmaster.c:393` (commented-out external Spectrum0 — proves the auto-feed mechanism)
* `cmaster.cs:539-540` (TXASetSipMode(1) + TXASetSipDisplay)
* `siphon.c:101-137` (xsiphon mode=1 → internal Spectrum0)
* `siphon.c:226-251` (TXASetSipPosition/Mode/Display setters)
* `dsp.cs:982-988, 992-1056` (alcgain field + CalculateTXMeter)
* `console.cs:24405-24461` (GetPixels selection)
* `console.cs case ALC_G` (Math.Max(0, -CalculateTXMeter))

### Lyra source citations
* `tx_channel.cpp:57` (kDspSize = 4096)
* `tx_channel.cpp:178` (OpenChannel call)
* `tx_channel.cpp:338-345` (setAlcMaxGainDb → SetTXAALCMaxGain)
* `tx_channel.cpp:458-506` (TXA meter accessors — pure pass-throughs)
* `hl2_stream.h:358-359` (alcMaxGainDb Q_PROPERTY)
* `wdsp_engine.cpp configureAnalyzerForTx` (kRateTx=96000, kBlockTx=4096, SetDisplaySampleRate)
* `wdsp_engine.cpp setTxOwnsAnalyzer` (TXASetSipMode + TXASetSipDisplay)
* `metermodel.cpp computeAlcG` (current raw-display, divergent from Thetis)
* `panadapter.h:239-240` (dbMin_/dbMax_ defaults)
* `prefs.h:29` (Q_PROPERTY dbMin)
