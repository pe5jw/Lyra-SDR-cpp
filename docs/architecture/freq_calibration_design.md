# Frequency Calibration (WWV / time-station) — design

Operator-requested 2026-07-03. Port of Thetis's "Freq Cal", but rebuilt
as a **visual instrument** rather than a type-a-number form.

## What it does

Every HL2's reference oscillator is a few ppm off. When you tune a
standard time station (WWV 10.000000 MHz, etc.) its carrier lands
slightly off-centre in the receiver. Calibration measures that offset
and applies a global correction so the dial reads true.

## Reference mechanism (Thetis 2.10.3.13, verified 2026-07-03)

- UI: `setup.cs` Freq Cal group → `console.CalibrateFreq(freqMHz)`.
- Algorithm (`console.cs:9808-9883`): tune to the station, average 10
  FFT frames (50 ms apart), sum magnitude², find the peak bin within
  ±2500 Hz of DC. Then:
  - `offset_Hz = bin_width × (center_bin − peak_bin)`
  - `ppm_error = offset_Hz / station_freq_Hz`
  - **`correction_factor = 1.0 − ppm_error`** (default 1.0)
- Applied (`NetworkIO.cs:214-253`, `VFOfreq`): `sent = f × _freq_correction_factor`.
  Thetis's Ethernet path then converts to a DDS phase word
  (`2³² × f / 122.88 MHz`). **Lyra sends raw Hz** to the HL2 (the FPGA
  does the phase math), so the correction is just `round(f × factor)`.
- Thetis has **no** sub-bin interpolation and **no** signal/SNR gate —
  it will "calibrate" to a noise blip on a dead band. Lyra does better
  (below).

## Lyra port

### Injection point (the one choke)

`wire::set_rx_freq` / `wire::set_tx_freq` (`FrameComposer.cpp`) are the
single boundary every tune path funnels through — RIT, XIT, CTUN,
PS-feedback DDCs, waterfall-ID, TX-analyzer. `pushEffectiveRxFreq` /
`pushEffectiveTxFreq` do **all** display/offset/CTUN math in
operator-Hz and only *then* call these setters — so applying the
correction *inside* the setters means:
- every marker / passband / CTUN offset stays exact (operator-Hz);
- only the FPGA sees the nudged Hz;
- `prn->rx/tx[].frequency` holds the corrected value = same state as
  the reference (Thetis's `VFOfreq` writes the corrected value into the
  freq field `WriteMainLoop` reads).

Implementation: TU-scope `std::atomic<double> g_freq_correction{1.0}`
(mirror of Thetis `NetworkIO._freq_correction_factor`), applied via a
`corrected_freq(int)` helper with an **exact `== 1.0` fast path** so at
the default the behaviour is byte-identical to today.

### Safety / recall (locked with operator 2026-07-03)

Baseline today is **factor = 1.0** (no correction exists in lyra-cpp).
So the neutral value *is* current behaviour → recovery is trivial:
- **Reset → 1.0** restores exactly today's tuning.
- **Never auto-applies** — a measurement changes the live factor only on
  an explicit **Apply**.
- **Restore previous** — every Apply snapshots the prior value.
- **Manual entry** — type a factor by hand (e.g. copy the working value
  out of Thetis's diagnostic panel as a known-good seed / recovery).
- Isolated QSettings key (`cal/freqCorrection`), separate from Thetis.

### Per-radio ready (Brick / Anan later)

Mechanism is radio-agnostic (measure carrier offset in the RX FFT →
factor → multiply the requested freq). The **value** is per-radio (each
TCXO differs). Today: one global `cal/freqCorrection`. When multi-radio
lands: key it per radio (`cal/freqCorrection/<radioId>`) — additive, the
correction already sits *before* the FrameComposer family (nddc)
branches. Anan P2's different freq command is a one-line application
detail; the factor itself is family-agnostic. (Red Pitaya runs
HPSDR-compatible gateware → would incidentally work; not a target.)

## UX (locked: A+B hero, tab launches floating instrument)

- **Settings → Calibration tab**: the persisted factor, manual-entry
  override, Reset, "last calibrated" summary, and a **Calibrate now**
  button that pops the floating instrument. (Boring-but-necessary Thetis
  parity + the safety net live here.)
- **Floating instrument panel** (the delightful part): one-tap station
  picker (reuse `time_stations.cpp` — WWV/WWVH/CHU/BPM/… + "likely open"
  hint from the solar/continent data); the **A+B hero** = live carrier
  spike walking to a centre target line (B, spectrum-truthful) fused with
  a colour-coded null scale + big Hz/ppm readout (A); a carrier-strength
  gate ("carrier strong / no carrier — band open?"); averaging + Apply
  with before→after factor. Distinct high-contrast "calibrating" look;
  collapses to a one-line summary when done.

## Measurement engine

Reuse the RX FFT + the CW decoder's sub-bin parabolic AFC (A4 slice) for
sub-Hz carrier estimation. Tune the dial to the station's exact freq;
measure the carrier peak's offset from centre; SNR-gate on carrier
strength; average a few seconds. `factor = 1 − offset/station_freq`.

**Bench gate (the one unknown): the SIGN.** Thetis's `(center − peak)`
depends on FFT axis orientation, which may differ in lyra. Verify on
hardware — tune WWV, Apply, confirm the dial reads *true* (not
doubly-wrong) — before shipping.

## Operator settings — sample rate & RX BW (panel note)

Because Lyra measures the **demodulated audio** (fixed 48 kHz codec rate), not
the raw IQ spectrum, **the DDC sample rate has no effect on the reading** — the
carrier's audio pitch is `RF − dial`, independent of the DDC rate. (This is the
opposite of Thetis, whose IQ-domain measure improves with lower sample rate /
bigger FFT — that note on its panel is method-specific and does NOT apply here.
The 48 kHz audio clock being off by the same ~ppm shifts a 1 kHz tone by
< 0.0001 Hz — negligible.) **RX BW** doesn't change the correction (a symmetric
filter doesn't move a peak) but must comfortably contain the ~1 kHz carrier: a
normal SSB filter (~2.4–4 kHz USB) is ideal; a tight CW filter can clip/skirt
it. Panel note added to the measure group (settingsdialog.cpp buildCalibrationTab).

## Build stages

1. **Core plumbing** ✅ DONE (2026-07-03, builds clean, uncommitted):
   `g_freq_correction` atomic + `corrected_freq()` (exact `==1.0`
   fast-path) applied in `set_rx_freq`/`set_tx_freq` +
   `set_freq_correction`/`freq_correction` in FrameComposer;
   `HL2Stream::setFreqCorrection` (±1000 ppm clamp, NaN→1.0, persist
   `cal/freqCorrection`, re-push RX+TX, emit `freqCorrectionChanged`) +
   `freqCorrection()` getter + `freqCorrection_` atomic member + ctor
   restore→wire seed before first push. Default 1.0 = byte-identical
   to today. No UI yet — API ready.
2. **Settings → Calibration tab** ✅ DONE (2026-07-03, compiles clean):
   `buildCalibrationTab()` behind the `stream_` guard, registered after
   the CW tab. Live factor + ppm readout, manual-entry `QDoubleSpinBox`
   (8 dp, ±1000 ppm, keyboardTracking off → applies on Enter/focus-out),
   **Reset → 1.0**, **Restore previous** (reads `cal/freqCorrectionPrev`,
   which `setFreqCorrection` now snapshots on every change), and a
   `freqCorrectionChanged` sync so the readout/spin track any source.
   PA-Gain-style readable directions. The Stage-4 "Calibrate now"
   instrument launch is noted in-tab as coming next.
3. **Measurement engine** — split into 3a/3b/3c:
   - **3a** ✅ DONE (2026-07-03, builds clean, unwired = zero radio risk):
     `dsp/FreqCalMeasure.{h,cpp}` — Qt-free carrier tone estimator. Fed
     mono RX audio; Hann window → Goertzel coarse scan (±range, 2 Hz
     step) → median noise floor → parabolic sub-bin peak → SNR gate
     (default 10 dB) → EWMA over strong windows. Exposes `measuredHz`,
     `offsetHz` (=measured−target), `snrDb`, `strong`, `ready`,
     `windows`. Registered in CMakeLists after CwDecoder.
   - **3b** ✅ DONE (2026-07-03, builds+links clean) — wired live-measure
     (no auto-tune; operator watches live). `WdspEngine`: `freqCal_`
     member + `setFreqCalMeasuring(bool)` (target 1200 Hz ±1100, 10 dB
     gate) + feed in `dispatchAudioFrame` (pre-EQ mono, gated on
     `freqCalOn_`) + `freqCalUpdated(measuredHz,snrDb,windows)` signal
     (one emit per analyzed window — added `FreqCalMeasure::analyzed()`).
     Calibration tab "Measure from a time station" group: station MHz
     field, checkable **Measure**, live carrier/SNR/win readout,
     suggested factor + ppm + error, **Apply**. Math: `D=rx1FreqHz()`,
     `expected=station−D` (USB), sanity-gated, `e_hw = kMeasureSign·
     (station−D−measured)/D`, `factor=1/(1+e_hw)`. **kMeasureSign=+1.0**
     pending 3c.
   - (was 3b) feed from the existing mono RX tap
     (`wdsp_engine.cpp:3518`, gated on a "measuring" flag) + an
     `HL2Stream` coordinator `measureFreqCal(stationHz)` that saves the
     current freq/mode, tunes `dial = stationHz − targetPitch` in USB
     (carrier lands at ~targetPitch audio), collects until `ready()` or
     timeout via a QTimer, computes `factor`, emits a result
     (offset/snr/suggestedFactor), restores tuning. Result flows to the
     Calibration tab (a "Measure from signal" button) + later the
     instrument.
   - **3c** ✅ PASSED — SIGN CONFIRMED `kMeasureSign = +1.0` on live WWV
     10 MHz (operator N8SDR, 2026-07-03). Before Apply: carrier 1000.5 Hz
     (error +0.5), suggested 1.00000005; after Apply: carrier 999.6 Hz
     (error −0.4) — the correction **nulled** the error (crossed through
     1000), not doubled it → sign right. Physics confirms: carrier high →
     crystal slow → factor >1 raises DDC → carrier drops to target. No
     code change (already +1.0). NOTE: this HL2 is already ~0.05 ppm
     (0.5 Hz @ 10 MHz — excellent TCXO), so corrections are sub-Hz and
     the ±0.5 Hz dither is just the 8192-window measurement floor. A big
     confidence test = set 1.00001000 (+10 ppm) by hand → carrier jumps
     to ~1100 Hz → Measure+Apply snaps it back to ~1000.
4. **Floating instrument panel** ✅ BUILT (2026-07-03, compiles + qml
   cache clean; awaiting operator visual bench). `src/qml/
   CalibrationPanel.qml` — chip-launched floating dock (header **"Freq
   Cal"** chip via `addQuickDock("freqcal",…)` in mainwindow buildDocks,
   like CW/Tuner). Station picker = `Time.calStations()` (new
   `TimeStations::calStations()` — flat {label,freqHz,continent}). Tap →
   `startStation`: save freq+mode → `Stream.setRx1FreqHz(f−1000)` +
   `WdspEngine.setMode("USB")` + `setFreqCalMeasuring(true)`; the A+B
   hero = carrier marker sliding on a ±50 Hz centre-null scale (green
   band ±2 Hz + centre line) with big colour-coded "Hz off" + SNR + the
   suggested ppm; **Apply** → `Stream.setFreqCorrection`; **Stop** /
   close → disarm + restore. Same kMeasureSign=+1 math as the tab.
   Needed `Q_PROPERTY freqCorrection` on HL2Stream (QML readout);
   setRx1FreqHz/setFreqCorrection already public slots. FOLLOW-UPS: the
   tab's "Calibrate now" launch button (needs settingsdialog→mainwindow
   show-dock signal); "likely open" hint on chips (solar data); visual
   polish per operator.
   ⚠ **Auto-tune MUST force USB on every band** (dial = station − ~1 kHz,
   mode = USB), NOT the band's usual sideband. The sign gate
   (`kMeasureSign=+1.0`) is calibrated for USB (`audio = RF − dial`);
   LSB flips it (and tuned-below the carrier falls in the rejected
   sideband). WWV is AM, so there is no "correct" sideband — USB is our
   consistent measurement reference on 2.5/5 MHz just as on 10/15/20.
   (Panel note added to the tab 2026-07-03 saying exactly this.)
5. Docs (USER_GUIDE) + release.
