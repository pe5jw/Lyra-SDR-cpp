# TXA chain — the WDSP TX DSP pipeline

**Source read 2026-06-04:** `wdsp/TXA.c:1-680` (constructor +
destructor + flush + `xtxa()` execution + rate setters).

---

## The chain — `xtxa()` execution order

`wdsp/TXA.c:557-592`. **THIS is the canonical TX DSP order** —
every Lyra-native port must mirror it.

```c
void xtxa (int channel) {
    xresample (rsmpin);       //  1. input resampler          (48k → 96k)
    xgen      (gen0);         //  2. input signal generator   (test/tune)
    xpanel    (panel);        //  3. includes MIC gain         (inselect=2, I-as-input)
    xphrot    (phrot);        //  4. phase rotator             (PHROT)
    xmeter    (micmeter);     //  5. MIC meter                 (read-only)
    xamsqcap  (amsq);         //  6. AM-squelch capture
    xamsq     (amsq);         //  7. AM-squelch action
    xeqp      (eqp);          //  8. pre-EQ (10-band)
    xmeter    (eqmeter);      //  9. EQ meter
    xemphp    (preemph, 0);   // 10. FM pre-emphasis (option 0, position 0)
    xwcpagc   (leveler);      // 11. Leveler (wcpagc mode 5)
    xmeter    (lvlrmeter);    // 12. Leveler meter
    xcfcomp   (cfcomp, 0);    // 13. Continuous Frequency Compressor
    xmeter    (cfcmeter);     // 14. CFC meter
    xbandpass (bp0, 0);       // 15. **PRIMARY BANDPASS — SSB SIDEBAND SELECTOR** (always-on)
    xcompressor(compressor);  // 16. COMP compressor
    xbandpass (bp1, 0);       // 17. aux bandpass (runs only if COMP)
    xosctrl   (osctrl);       // 18. CESSB Overshoot Control
    xbandpass (bp2, 0);       // 19. aux bandpass (runs only if CESSB)
    xmeter    (compmeter);    // 20. COMP meter
    xwcpagc   (alc);          // 21. **ALC (always-on, load-bearing splatter protection)**
    xammod    (ammod);        // 22. AM modulator
    xemphp    (preemph, 1);   // 23. FM pre-emphasis (option 1, position 1)
    xfmmod    (fmmod);        // 24. FM modulator
    xgen      (gen1);         // 25. **OUTPUT signal generator (TUN + Two-tone)** — POST-ALC
    xuslew    (uslew);        // 26. up-slew (5 ms cos² ramp on AM/FM/gens start)
    xmeter    (alcmeter);     // 27. ALC meter
    xsiphon   (sip1, 0);      // 28. **siphon — THE PURESIGNAL FEEDBACK TAP**
    xiqc      (iqc.p0);       // 29. **PureSignal correction (pre-distortion apply)**
    xcfir     (cfir);         // 30. compensating FIR (Protocol 2 only — HL2 P1 no-op)
    xresample (rsmpout);      // 31. output resampler          (96k → 48k wire)
    xmeter    (outmeter);     // 32. output meter
}
```

**Everything operates IN-PLACE on `txa[channel].midbuff`** — every
stage has midbuff as both input and output buffer. Read the rsmpin
output → midbuff; chain stages mutate midbuff; rsmpout reads midbuff
→ outbuff for the wire.

## Critical insights from the chain order

### bp0 vs bp1 — the §15.23 trap, now source-verified

- **`bp0` is ALWAYS-ON** (`run=1` at constructor line 240).
  It's the SSB sideband selector. Its passband edges are
  `txa.f_low`, `txa.f_high` — set via `SetTXAMode` /
  `SetTXABandpassFreqs`.
- **`bp1` is OFF by default** (`run=0` at constructor line 261).
  Comment: "ONLY RUNS WHEN COMPRESSOR IS USED". It's an aux
  bandpass that re-shapes the compressor's output.
- **`bp2` is OFF by default** (same — only with CESSB).

`xbandpass(bp1, 0)` at xtxa() line 575 runs unconditionally, but
xbandpass internally checks `bp->run` and no-ops if zero.

**The §15.23 Lyra bug:** Lyra called `SetTXABandpassRun(ch, 1)` in
its TX init thinking it was turning ON the SSB sideband filter. But
`SetTXABandpassRun` toggles `bp1.run`, not `bp0.run`. Forcing
`bp1.run=1` cascaded the stale-create-time bp1 filter (passband
`(-5000, -100)` — LSB shape, never refreshed because TXASetupBPFilters
only recomputes it when the compressor runs) AFTER the correctly-
shaped bp0. Result: LSB worked (mostly), USB was killed (bp1's
negative-only passband rejected the positive baseband).

**The correct mechanism:** never touch `bp1.run` from Lyra. Let
xcompressor set it (when compressor runs). `bp0` configures via
`SetTXAMode` + `SetTXABandpassFreqs` only. Lyra commit `86ac228`
(2026-05-15) shipped this fix.

### gen1 is POST-ALC — TUN drive math

`gen1` (line 583, between fmmod and uslew) is the output-side test
generator used by TUN and Two-tone modes. **Its level passes
through ALC** (xwcpagc.alc at line 579) — actually NO, gen1 fires
AFTER `xwcpagc(alc)` at line 579. So gen1 injects AFTER ALC.

This means:
- The tune carrier's level is governed by `gen1.tone_mag` directly,
  NOT by ALC.
- The §15.26 `_TUNE_TONE_MAG` 0.5 → 0.95 fix Lyra shipped
  (`9cef8fc`) was tweaking this exact level. Verified-correct
  against the reference's chain order: ALC doesn't bound gen1.

### sip1 → iqc — the PureSignal hook

The PS feedback tap (`sip1`, line 586) sits AFTER all the
modulation/compression/ALC processing but BEFORE the output
resampler. It captures TX I/Q at the post-everything pre-wire
point — exactly what feeds the antenna (modulo cfir+rsmpout
+ quantize).

`iqc` (line 587) applies pre-distortion AFTER sip1 has captured
the reference signal. So:
- sip1 captures the **target** signal (what the operator/mod wants
  the radio to send)
- calcc reads sip1 + the PA-coupler feedback (DDC0+DDC1 on HL2 PS)
  → learns the PA's nonlinearity → updates iqc coefficients
- iqc applies those coefficients to the signal flowing through →
  pre-distortion that linearizes the PA

For the Lyra rip, the sip1 tap must be present and instantiated
from v0.2 onward (Rule 10 — PS-shaped from day one). iqc can be
inert until v0.3.

### preemph appears twice — position-driven FM emphasis

`xemphp(preemph, 0)` at line 568 (option 0, position 0 = before
the leveler/compressor stack) and `xemphp(preemph, 1)` at line 581
(option 1, position 1 = AFTER the compressor + ALC). Both call
sites use the SAME `preemph` object. The position parameter
selects which call is "active" — controlled by
`SetTXAFMEmphPosition` (preemph.position == 0 or 1).

For FM TX, the operator can pick whether pre-emphasis happens
BEFORE the leveler+compressor (preserves dynamic range but
amplifies HF noise) or AFTER (less HF noise but less dynamic
preservation). Lands in v0.2.2 FM modulator task.

## Component table (from create_txa constructor)

`wdsp/TXA.c:31-479`. Default `run` states, key parameters.

| Order in xtxa() | Component | File:line in TXA.c | Default run | Notes / defaults |
|---|---|---|---|---|
| 1 | rsmpin | 40-49 | 0 (toggled by TXAResCheck if in_rate != dsp_rate) | gain=1.0 |
| 2 | gen0 | 51-57 | 0 | mode=2 (test gen) |
| 3 | panel | 59-69 | **1** (always) | gain1=1.0; gain2I/Q=1.0; **inselect=2 (I-as-input)**; copy=0 |
| 4 | phrot | 71-78 | 0 | 338 Hz / 8 stages |
| 5 | micmeter | 80-93 | **1** (always-on meter) | tau_avg=0.100, tau_peak=0.100 |
| 6-7 | amsq | 95-109 | 0 | mute_gain=0.200, signal/tail timings |
| 8 | eqp | 111-128 | 0 | 10-band defaults: -12dB low end, +12dB at 4k, -10dB at 8k+ |
| 9 | eqmeter | 130-143 | follows eqp.run | |
| 10/23 | preemph | 145-156 | 0 | type=0, f_low=300, f_high=3000 |
| 11 | leveler | 158-181 | **0** (OFF by default) | mode=5, max_gain=1.778, tau_attack=0.001, tau_decay=0.500 |
| 12 | lvlrmeter | 183-196 | follows leveler.run | |
| 13 | cfcomp | 198-222 | 0 | fft_size=2048, overlap=4, 5-band defaults |
| 14 | cfcmeter | 224-237 | follows cfcomp.run | |
| 15 | **bp0** | 239-251 | **1 (ALWAYS RUNS)** | gain=2.0; passband initially `(-5000, -100)` (LSB default) — overwritten by SetTXAMode |
| 16 | compressor | 253-258 | 0 | gain=3.0 |
| 17 | bp1 | 260-272 | **0 (only with COMP)** | gain=2.0 |
| 18 | osctrl | 274-280 | 0 | gain=1.95 |
| 19 | bp2 | 282-294 | **0 (only with CESSB)** | gain=1.0 |
| 20 | compmeter | 296-309 | follows compressor.run | |
| 21 | **alc** | 311-334 | **1 (ALWAYS ON)** | mode=5, env-mode=1, **max_gain=1.0, tau_attack=0.001, tau_decay=0.010**, out_targ=1.0 |
| 22 | ammod | 336-342 | 0 | mode=0 (AM), carrier_level=0.5 |
| 24 | fmmod | 345-359 | 0 | dev=5000, BW=300-3000, CTCSS run=1, ctcss_freq=100, ctcss_level=0.10 |
| 25 | gen1 | 361-367 | 0 | mode=0 (single-tone TUN) |
| 26 | uslew | 369-377 | (channel-controlled) | delay=0, slew_time=0.005 (5ms cos²) |
| 27 | alcmeter | 379-392 | **1** | |
| 28 | **sip1** | 394-403 | **1 (ALWAYS-ON tap)** | position=0, mode=0, buffer=16384 samples, fft=16384 |
| 29 | iqc | 424-432 | 0 | ints=16, changeover=0.005, spi=256 |
| 30 | cfir | 434-449 | 0 (P2 only) | CIC settings — interpolation 640, 5 comb pairs |
| 31 | rsmpout | 451-460 | 0 (toggled by TXAResCheck if dsp_rate != out_rate) | gain=0.980 |
| 32 | outmeter | 462-475 | **1** | |

Also created but not in xtxa() execution:
- `calcc` (line 405-422) — PS calibration math (runs on its own
  thread, semaphore-driven; see §9 of mapping doc)

## TX channel parameters (`create_txa` context)

- `txa[channel].mode` default = `TXA_LSB`
- `txa[channel].f_low` default = -5000.0
- `txa[channel].f_high` default = -100.0
  (negative-baseband — selects LSB by convention)
- `txa[channel].inbuff`, `outbuff`, **`midbuff`** allocated
  per-channel; midbuff is `2 * dsp_size * complex` (twice as
  big as the others — gives the in-place stages headroom).

## Resamplers (TXAResCheck)

`wdsp/TXA.c:478` calls `TXAResCheck(channel)` which toggles
`rsmpin.run` and `rsmpout.run` based on whether in_rate, dsp_rate,
and out_rate differ. For HL2 (in=48k, dsp=96k, out=48k):
- rsmpin must run (48 → 96)
- rsmpout must run (96 → 48)

The resampler `gain` is 1.0 on the input side, 0.980 on the
output side — a deliberate small attenuation on the way out (to
keep float full-scale below int16 clip after quantize).

## Flush sequence

`flush_txa()` at lines 520-555 zeroes all three buffers
(inbuff, outbuff, midbuff) and calls each stage's flush function
in the same order as xtxa(). This is called on a TX-channel stop
(SetChannelState OFF) to ensure no stale samples persist into
the next keydown.

The destroy_txa() function (line 481-518) tears down in REVERSE
order of construction (LIFO). Lyra's `~TxChannel()` must follow
suit.

## Lyra-native mapping (synthesis)

```
lyra::wdsp::TxChannel    // the C++ wrapper around the WDSP TXA cffi

constructor: calls
  - OpenChannel(in_rate=48k, dsp_rate=96k, out_rate=48k, type=1,
                state=0, tdelayup=0.000, tslewup=0.010,
                tdelaydown=0.000, tslewdown=0.010, block=1)
  - then the per-stage setters to match the reference's create_txa()
    defaults (run-states, parameter values).
  - sip1 ALWAYS-ON (Rule 10 forward-compat for PS).

dsp_step(in[insize], out[outsize], &err):
  - calls fexchange0(channel_id, in, out, &err) — the cffi entry
    that runs xtxa() internally.
  - does NOT walk the chain in Lyra C++ code; WDSP does it.

setters (each routed through a single Lyra wrapper method that
takes the per-stage Lyra-meaningful parameter and calls the
correct WDSP cffi):
  - set_mode(USB/LSB/AM/FM/CW)        → SetTXAMode + bandpass refresh
  - set_passband(low_hz, high_hz)     → SetTXABandpassFreqs (calls
                                        TXASetupBPFilters → bp0 update)
  - set_mic_gain(linear)              → SetTXAPanelGain1
  - set_phrot(run, freq, n_stages)    → SetTXAPHROTRun /
                                        SetTXAPHROTCorner /
                                        SetTXAPHROTNstages
  - set_eq(...)                       → SetTXAEQRun + EQ coefs
  - set_leveler(...)                  → SetTXAlevelerRun / MaxGain / Decay
  - set_alc_max_gain(linear)          → SetTXAALCMaxGain (always-on,
                                        operator tunes max_gain only;
                                        attack/decay default 1ms/10ms)
  - set_compressor(run, gain)         → SetTXACompressorRun (xcompressor
                                        SELF-RUNS bp1/bp2 internally via
                                        TXASetupBPFilters — DO NOT touch
                                        bp1/bp2 run from Lyra)
  - set_cessb(run, gain)              → SetTXAosctrlRun
  - set_tune_gen(run, freq, mag)      → SetTXAPostGenRun /
                                        SetTXAPostGenTone* (drives gen1)
  - set_iqc(...)                      → SetTXAiqcRun + coeffs (v0.3)
  - get_meter(type) → dB              → GetTXAMeter

DO NOT call SetTXABandpassRun in Lyra.  bp0 runs always (no setter
needed); bp1/bp2 are managed internally by the compressor/CESSB
run paths.  Touching bp1.run is the §15.23 trap.
```

## Pending follow-ups

- `setSize_*`, `setSamplerate_*`, `setBuffers_*` patterns at lines
  594-680 — captured the signatures; need to walk these when
  Lyra implements rate-change support.
- Setters for individual stages (SetTXAMode, SetTXAALCMaxGain,
  etc.) — those live in TXA.c lines 680-873 and the per-stage
  C files. Capture as needed during Phase 2 component port.

*File written 2026-06-04 during Phase 0 read.*
