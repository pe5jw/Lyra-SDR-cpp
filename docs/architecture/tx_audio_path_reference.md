# TX Audio Path Reference — Thetis 2.10.3.13

Architecture spec for the lyra-cpp TX chain, derived from a read of OpenHPSDR
Thetis 2.10.3.13 (C# console + WDSP C DSP). All `file:line` cites are absolute
into `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/...`.

This document lives in `docs/architecture/` and uses reference names
(`SetTXAPanelGain1`, `MicPreamp`, etc.) so the spec is directly verifiable
against Thetis source. Shipped lyra-cpp code/UI is still subject to the
no-attribution rule — this doc is the bridge.

> **READ-FIRST NOTE — reference vs ship divergence (operator-locked).** This
> document maps **what Thetis does** (so the chain positions, gain stages,
> meter taps, etc. are pinned to a verifiable known-good). It is NOT a
> 1:1 spec for what Lyra ships. The most important divergence:
>
> - **§8 documents Thetis CFC / CFCOMP for chain-position reference only.
>   Lyra does NOT ship CFC.** Per `FEATURES.md` §3.3 and
>   `tx1_ssb_design.md` §6.7 + §6.7 v0.2.1 deferral table (line 1111),
>   operator preference (locked 2026-05-20, reaffirmed 2026-05-31) is to
>   **replace CFC with a Lyra-native 5-band Combinator** (X-Air-style
>   Linkwitz-Riley crossovers + per-band threshold/ratio/attack/release/
>   makeup, optional top-band harmonic exciter). Compander/CPDR also NOT
>   planned. Knowing where CFC sits in the reference chain still matters
>   because the Combinator slots into the same pre-`fexchange0` position
>   in the Lyra-native pre-processor chain.
>
> - **§3 documents reference-faithful 20 dB Mic Boost wiring as a HARDWARE
>   codec PGA bit (NOT a software multiplier).** Lyra-cpp Component 8a+
>   must follow this — a TXA-side software boost in parallel would
>   double-count.
>
> - **§13 (Open questions / Lyra-cpp implications) is the actionable
>   discoveries list.** Read it after §1; it surfaces the bug-class
>   findings (the ALC `max_gain=0 dB` default trap being the load-bearing
>   one for the 2026-05-31 bench result).

---

## 1. The WDSP TXA DSP Chain Order

Canonical chain definition: `wdsp/TXA.c:557-592` (`xtxa()`), with construction
defaults in `create_txa()` at `wdsp/TXA.c:31-479`. The chain processes complex
double samples in-place in `txa[channel].midbuff`. Mic samples are interleaved
(I, Q) where the bare codec mic typically lives on the I lane (`inselect=2`).

Order of execution per TX frame (`xtxa()` body):

| #  | Stage           | API to enable/configure                    | Default (per `create_txa`)                                                    | Where vs PanelGain1 |
|----|-----------------|--------------------------------------------|-------------------------------------------------------------------------------|---------------------|
|  1 | `rsmpin`        | auto via `TXAResCheck`                     | off unless `in_rate != dsp_rate` (line 40-49, 809-817)                        | before              |
|  2 | `gen0`          | TUN/two-tone input source                  | run=0, mode=2 (TXA.c:51-57)                                                   | before              |
|  3 | **`panel`**     | `SetTXAPanelRun` / `SetTXAPanelGain1` / `SetTXAPanelSelect` | run=1, gain1=1.0, inselect=2 (I-only), copy=0 (TXA.c:59-69; patchpanel.c:201-227) | **THIS IS MIC GAIN** |
|  4 | `phrot`         | phase rotator (LSB/USB SSB phasing test)   | run=0, 8 stages, 338 Hz center (TXA.c:71-78)                                  | after PG1           |
|  5 | `micmeter`      | `GetTXAMeter(_, TXA_MIC_AV/_PK)`           | always run=1, 100 ms tau (TXA.c:80-93)                                        | after PG1           |
|  6 | `amsq` (cap+run)| AM squelch / downward expander             | run=0 (TXA.c:95-109)                                                          | after PG1           |
|  7 | `eqp` (EQ)      | `SetTXAEQRun`                              | run=0; default 10-band shape `(-12,-12,-12,-1,+1,+4,+9,+12,-10,-10)` dB (TXA.c:111-128) | after PG1   |
|  8 | `eqmeter`       | `GetTXAMeter(_, TXA_EQ_*)`                 | gated on `eqp.run`                                                            | —                   |
|  9 | `preemph` (1st) | FM only, position=1                        | run=0, 300-3000 Hz (TXA.c:145-156)                                            | after PG1           |
| 10 | **`leveler`**   | `SetTXALevelerSt` + `Top` + `Decay` (wcpagc) | run=0, max_gain=1.778 (≈5 dB), tau_att=1 ms, tau_decay=500 ms (TXA.c:158-181) | after PG1, BEFORE bandpass |
| 11 | `lvlrmeter`     | `GetTXAMeter(_, TXA_LVLR_*)`               | gated on leveler.run                                                          | —                   |
| 12 | `cfcomp` (CFC)  | `SetTXACFCOMPRun`, post-EQ run             | run=0, 5 bands, 2048 FFT, ovl=4 (TXA.c:198-222)                               | after PG1           |
| 13 | `cfcmeter`      | `GetTXAMeter(_, TXA_CFC_*)`                | gated on cfcomp.run                                                           | —                   |
| 14 | **`bp0`** (BPF) | `SetTXABandpassFreqs`                      | **always run=1**, gain=2.0, f_low/high per mode (TXA.c:239-251, 827-901)      | after PG1           |
| 15 | `compressor`    | `SetTXACompressorRun` / `SetTXACompressorGain` | run=0, gain=3.0 (TXA.c:253-258)                                           | after PG1           |
| 16 | `bp1`           | aux bandpass; only runs when compressor on | run=0 (TXA.c:260-272)                                                         | —                   |
| 17 | **`osctrl`** (CESSB) | `SetTXAosctrlRun`                     | run=0, osgain=1.95, bw=3000 (TXA.c:274-280; osctrl.c:52-70). Hershberger W9GR CESSB overshoot controller. | after PG1 |
| 18 | `bp2`           | only when compressor && osctrl             | run=0 (TXA.c:282-294)                                                         | —                   |
| 19 | `compmeter`     | `GetTXAMeter(_, TXA_COMP_*)`               | gated on compressor.run                                                       | —                   |
| 20 | **`alc`** (wcpagc) | `SetTXAALCMaxGain` / `SetTXAALCDecay` (Run is always 1) | run=1 always, max_gain=1.0 (0 dB)!, tau_att=1 ms, tau_decay=10 ms (TXA.c:311-334) | after PG1, final RF-side limiter |
| 21 | `ammod`         | enabled by `SetTXAMode(AM/SAM/DSB/AM_LSB/AM_USB)` | run=0 at create; carrier=0.5; switched in `SetTXAMode` (TXA.c:336-342, 752-789) | — |
| 22 | `preemph` (2nd) | FM only, position=2                        | run conditional                                                               | —                   |
| 23 | `fmmod`         | enabled by `SetTXAMode(FM)`                | run=0, dev=5000, AF 300-3000, CTCSS run=1@0.10 level (TXA.c:345-359)          | —                   |
| 24 | `gen1`          | TUN/two-tone output source                 | run=0 (TXA.c:361-367)                                                         | —                   |
| 25 | `uslew`         | up-slew for AM/FM/gen pop suppress         | 5 ms up-slew (TXA.c:369-377)                                                  | —                   |
| 26 | `alcmeter`      | `GetTXAMeter(_, TXA_ALC_*)`                | always run                                                                    | —                   |
| 27 | `sip1`          | siphon for TX panadapter (display tap)     | run=1                                                                         | —                   |
| 28 | `iqc`           | PureSignal predistortion correction        | run=0 unless PS on                                                            | —                   |
| 29 | `cfir`          | Protocol-2 only compensating FIR           | run=0                                                                         | —                   |
| 30 | `rsmpout`       | auto, gain=0.980                           | off unless `dsp_rate != out_rate` (TXA.c:451-460)                             | after PG1, final    |
| 31 | `outmeter`      | `GetTXAMeter(_, TXA_OUT_*)`                | always run; this is the I/Q PEP feeding the radio                             | —                   |

**Conclusion (where Mic Gain lives):** `panel.gain1` is stage #3 — applied
**immediately after the input resampler/gen0 and before everything else**
(phrot, mic meter, EQ, leveler, CFC, bandpass, compressor, OSCtrl, ALC). It is
a simple linear scalar multiplied into both I and Q (`patchpanel.c:55-101`,
`xpanel()`):

```c
double gainI = a->gain1 * a->gain2I;
double gainQ = a->gain1 * a->gain2Q;
// gain2I=gain2Q=1.0 by default, so gainI = gainQ = gain1
```

This means lyra-cpp's Mic Gain control must apply **before** the mic meter,
before bandpass shaping, and well before ALC pinning. There is no separate
HW PGA control in this codebase (the codec PGA settings come via NetworkIO —
see §3).

---

## 2. Mic Gain — Operator Control Mapping

**Main-screen slider:** `ptbMic` PrettyTrackBar in `console.cs` —
`ptbMic_Scroll()` at `console.cs:29450-29503` is the value-change handler. It:

1. Clamps to `[mic_gain_min, mic_gain_max]` (operator-configurable; see below)
2. Updates `lblMicVal.Text` to "{N} dB"
3. Calls `setAudioMicGain((double)ptbMic.Value)` at `console.cs:29504-29533`

**dB-to-linear conversion** (`console.cs:29518`):

```csharp
Audio.MicPreamp = Math.Pow(10.0, gain_db / 20.0);  // convert to scalar
```

So `MicPreamp = 10^(dB/20)`. Mic-muted state sets `MicPreamp = 0.0`.

**Audio.MicPreamp setter** at `audio.cs:215-224` then calls
`cmaster.CMSetTXAPanelGain1(WDSP.id(1, 0))` (TX channel = 1, subchannel 0)
which routes through the mode/VAC selection table (see §5) and eventually:

`cmaster.cs:1109` →

```csharp
Audio.console.radio.GetDSPTX(0).MicGain = gain;
```

`radio.cs:4127-4144` (`DSPTX.MicGain`):

```csharp
private double mic_gain_dsp = 0.5;
private double mic_gain = 0.5;          // 0.5 linear = ~-6 dB
public double MicGain
{
    set
    {
        mic_gain = value;
        if (update && (value != mic_gain_dsp || force))
        {
            WDSP.SetTXAPanelGain1(WDSP.id(thread, 0), value);
            mic_gain_dsp = value;
        }
    }
}
```

Final DLL entry: `wdsp/patchpanel.c:208-215`:

```c
PORT void SetTXAPanelGain1 (int channel, double gain)
{
    EnterCriticalSection (&ch[channel].csDSP);
    txa[channel].panel.p->gain1 = gain;
    LeaveCriticalSection (&ch[channel].csDSP);
}
```

**Trackbar value range** — `console.cs:19262, 19273`:

```csharp
private int mic_gain_min = -40;   // floor for the on-screen slider
private int mic_gain_max =  10;   // default ceiling
```

The slider exposes integer dB; these floor/ceiling values are themselves
operator-tunable. Setup → Transmit also has independent **profile** bounds
(`udMicGainMin` / `udMicGainMax`, `setup.cs:9855-9864`) that write
`console.MicGainMin / MicGainMax`. The Setup spinner caps `udMicGainMax` at
40 (`setup.cs:1085 → udMicGainMax.Maximum = 40`), matching the screenshot
("Max=40, Min=-90" — operator profile widens the range below the default
`-40`).

**Persistence:** stored per TX Profile (see §11). Each profile row contains
`Mic_Gain` (the dB slider value, not the linear) plus `Mic_Input_On`,
`Mic_Input_Boost`, `Line_Input_On`, `Line_Input_Level`.

---

## 3. 20 dB Mic Boost — What It Actually Does

**Setup checkbox handler** at `setup.cs:7851-7855`:

```csharp
private void chk20dbMicBoost_CheckedChanged(object sender, System.EventArgs e)
{
    if (chk20dbMicBoost.Checked) udVOXGain_ValueChanged(this, e);
    console.MicBoost = chk20dbMicBoost.Checked;
}
```

**`console.MicBoost`** setter at `console.cs:13310-13320` flows into
`SetMicGain()` at `console.cs:41787-41800`:

```csharp
public void SetMicGain()
{
    var v = mic_boost ? 1 : 0;
    NetworkIO.SetMicBoost(v);          // → ChannelMaster.dll → HL2 C&C

    v = line_in ? 1 : 0;
    NetworkIO.SetLineIn(v);            // codec input mux

    if (!lineinarrayfill) MakeLineInList();
    var lineboost = Array.IndexOf(lineinboost, line_in_boost.ToString());
    NetworkIO.SetLineBoost(lineboost); // 0-31 PGA dB step
}
```

The four calls are P/Invoke into ChannelMaster.dll
(`HPSDR/NetworkIOImports.cs:200-210`). These set fields in the HPSDR C&C
output bytes that the device firmware decodes to program the AK4951 codec on
HL2 (or analogous codec on full ANAN devices).

**Implication for lyra-cpp on HL2:** the 20 dB boost is a **hardware codec
bit** (NOT a software gain). It changes the analog PGA before the codec ADC.
That means:

- Toggling it does NOT pass through `SetTXAPanelGain1` — the WDSP-side gain
  is unaffected.
- Lyra-cpp needs to send the corresponding HL2 C&C bit (in P1 frames, this
  lives in the C2 status byte of frame index 2; bit position matches the HL2
  firmware's mic-boost decode — verify against `gateware-hl2/Hermes-Lite.v`
  before wiring).
- VOX threshold is auto-rescaled when boost toggles
  (`setup.cs:7853 → udVOXGain_ValueChanged`); cmaster doubles the threshold
  by `VOXGain` if `MicBoost == true` (`cmaster.cs:1063-1068`).

---

## 4. Mic Source — Mic In vs Line In (Analog Phone Modes)

**Radio handlers** at `setup.cs:14567-14589`:

```csharp
private void radMicIn_CheckedChanged(...)
{
    if (!radMicIn.Checked) return;
    console.LineIn = false;
    radLineIn.Checked = false;
    chk20dbMicBoost.Visible = true;       // Mic Boost only valid with Mic In
    chk20dbMicBoost.Enabled = true;
    lblLineInBoost.Visible = false;
    udLineInBoost.Visible = false;
}

private void radLineIn_CheckedChanged(...)
{
    if (!radLineIn.Checked) return;
    console.LineIn = true;
    radMicIn.Checked = false;
    chk20dbMicBoost.Visible = false;      // Mic Boost hidden when Line In
    chk20dbMicBoost.Enabled = false;
    lblLineInBoost.Visible = true;
    udLineInBoost.Visible = true;         // Line gain 0-31 dB exposed instead
}
```

`console.LineIn` setter (`console.cs:13286-13296`) → `SetMicGain()` →
`NetworkIO.SetLineIn(bits)` (codec input mux register). `LineInBoost` setter
(`console.cs:13298-13308`) does the same and ends in
`NetworkIO.SetLineBoost(lineboost)` where `lineboost` is an integer 0..31
mapped from a string lookup table (`console.cs:41797`). So Line In Boost is
**also HW** (codec PGA in dB steps), not software.

**Applies to all phone modes** that use the mic input — selection is at the
codec; the WDSP TXA chain doesn't know which one is selected. The same
`MIC_Input` / `LineIn` arrangement is in effect for LSB, USB, DSB, AM, SAM,
FM, DIGL, DIGU when MIC is the audio source (see §5).

---

## 5. Per-Mode TX Audio Source Matrix

The decision tree is centralized in `cmaster.cs:1070-1110`
(`CMSetTXAPanelGain1`). Reading that:

```csharp
if ((!Audio.VACEnabled && (LSB|USB|DSB|AM|SAM|FM|DIGL|DIGU)) ||
    (Audio.VACEnabled && Audio.VACBypass && (DIGL|DIGU|LSB|USB|DSB|AM|SAM|FM)))
{
    if (Audio.WavePlayback)
        gain = WavePreamp * WavePreampAdjust;       // WAV file playback override
    else if (!Audio.VACEnabled && (DIGL || DIGU))
        gain = Audio.VACPreamp;                     // DIG modes use VAC slider even w/o VAC
    else
        gain = Audio.MicPreamp;                     // normal phone mic
}
// else: gain stays at 1.0 — VAC stream is what drives TX; no extra panel gain
```

| DSPMode | VAC1 off                            | VAC1 on, no bypass             | VAC1 on, MOX/PTT bypass active   |
|---------|-------------------------------------|--------------------------------|----------------------------------|
| LSB     | MIC (MicPreamp)                     | VAC1 (panel=1.0)               | MIC (MicPreamp)                  |
| USB     | MIC (MicPreamp)                     | VAC1                           | MIC                              |
| DSB     | MIC                                 | VAC1                           | MIC                              |
| AM      | MIC                                 | VAC1                           | MIC                              |
| SAM     | MIC                                 | VAC1                           | MIC                              |
| FM      | MIC                                 | VAC1                           | MIC                              |
| DIGL    | **VAC1 slider scalar (VACPreamp)**  | VAC1                           | MIC                              |
| DIGU    | **VAC1 slider scalar (VACPreamp)**  | VAC1                           | MIC                              |
| CWL     | (TX audio not used)                 | —                              | —                                |
| CWU     | (TX audio not used)                 | —                              | —                                |
| DRM     | VAC2 driven; TXA bandpass 7-17 kHz  | VAC2                           | n/a                              |
| SPEC    | spectrum / not a phone mode         | —                              | —                                |

**`VACBypass` override truth (`console.cs:26034-26045`):**

```csharp
if (chkVAC1.Checked &&
    (((mic_ptt || cw_ptt) && _allow_vac_bypass) ||
     (VOXEnable && _allow_micvox_bypass)))
{
    if (!Audio.VACBypass) Audio.VACBypass = true;
}
else if (chkVAC1.Checked && Audio.VACBypass)
{
    Audio.VACBypass = false;
}
```

- `_allow_vac_bypass` ←→ Setup → `chkVACAllowBypass` ("Allow PTT to override
  VAC for Phone") — `setup.cs:12392-12395`.
- `_allow_micvox_bypass` ←→ VAC1 "VOX uses MIC instead of VAC" ←→
  `cmaster.cs:CMSetTXAVoxRun` (chooses where the DEXP-VOX detector listens).
- The SPACE key behaves as `mic_ptt` (handled in the same PTT loop).
- When MOX is released, the symmetric block at `console.cs:26097-26159` calls
  `Audio.VACBypass = false` to restore VAC1 as TX source on the next key-down.

**Implication for lyra-cpp:** the source selector is **(VAC enable) × (bypass
state) × (DSP mode)**. The simplest first-pass model:

- Mic-Source UI = a 4-position selector that maps to:
  - `Mic In`  → VAC1 disabled, MicIn radio
  - `Line In` → VAC1 disabled, LineIn radio
  - `VAC1`    → VAC1 enabled, no bypass
  - `VAC2`    → VAC2 enabled (different flow, see §6)
- "Allow PTT to bypass VAC" is a separate checkbox affecting only the
  VAC1-enabled state.

---

## 6. VAC1 / VAC2 Routing

**Roles** (inferred from `audio.cs`):

- **VAC1**: the primary phone/digital VAC. Carries the TX audio source for
  DIGL/DIGU (and for SSB/AM/FM when the operator routes a soft modem out of
  a VAC pair). RX1 audio goes back out.
- **VAC2**: a secondary VAC, defaultly tied to DRM and to RX2. Used to feed a
  separate decoder while VAC1 carries the operator's main digital app.

Per-VAC properties (sample, not exhaustive):

| Concept             | VAC1 API                           | VAC2 API                            | Default            |
|---------------------|------------------------------------|-------------------------------------|--------------------|
| Enable              | `Audio.VACEnabled`                 | `Audio.VAC2Enabled`                 | false              |
| Sample rate         | `SampleRate2` → `SetIVACvacRate(0,_)` | `SampleRate3` → `SetIVACvacRate(1,_)` | 48000        |
| Block size          | `BlockSizeVAC` → `SetIVACvacSize(0,_)`| `BlockSizeVAC2` → `SetIVACvacSize(1,_)`| 1024        |
| Stereo              | `VACStereo` → `SetIVACstereo(0,_)` | `VAC2Stereo` → `SetIVACstereo(1,_)` | false              |
| TX preamp           | `Audio.VACPreamp` → `SetIVACpreamp(0,_)` and feeds `CMSetTXAPanelGain1` (DIG-mode case) | `Audio.VAC2TXScale` → `SetIVACpreamp(1,_)` | 1.0 |
| RX scale            | `Audio.VACRXScale` → `SetIVACrxscale(0,_)` | `Audio.VAC2RXScale` → `SetIVACrxscale(1,_)` | 1.0     |
| Latency in/out (ms) | `Latency2 / Latency2_Out`          | `VAC2Latency... `                   | 120/120 manual     |
| PA latency in/out   | `LatencyPAIn / LatencyPAOut`       | `VAC2LatencyPAIn/Out`               | 120                |
| Combine I+Q→mono    | `VACCombineInput`                  | `VAC2CombineInput`                  | false              |
| MOX gating          | `ivac.SetIVACmox(0,_)`             | `ivac.SetIVACmox(1,_)`              | dynamic            |
| Bypass for phone    | `Audio.VACBypass` (see §5)         | not applicable                      | dynamic            |
| Allow PTT/MOX/SPACE override | `chkVACAllowBypass` → `_allow_vac_bypass` | (no per-VAC2 equivalent in the same dialog) | false |
| VOX uses MIC instead of VAC  | `_allow_micvox_bypass`     | n/a                                 | false              |

**"VOX uses MIC instead of VAC"** (`console.cs:26014`):

```csharp
bool vox_ok = !_mic_muted ||
              ((vac_enabled || vac2_enabled) && !_allow_micvox_bypass);
```

When unchecked, the DEXP/VOX detector listens to whichever VAC is enabled. When
checked, VOX listens to the physical mic input even though VAC is the TX
source — useful when the operator wants foot-PTT-free voice-activated TX while
running a digital app.

**TX-gain semantics for VAC** — the VAC1 "Gain (dB) RX:0 TX:3" sliders are
SEPARATE from the WDSP Mic Gain:

- VAC1 TX gain (`Audio.VACPreamp`) is applied **inside IVAC** before audio is
  handed to TXA (`SetIVACpreamp(0,value)`).
- It is ADDITIONALLY copied into `panel.gain1` only in the special
  "DIGL/DIGU without VAC enabled" case (cmaster.cs:1103-1104).
- So when VAC1 is on, both `IVAC preamp` AND the operator's Mic Gain slider
  are bypassed (panel stays at 1.0); the VAC slider is the only TX-gain knob.

**Implication for lyra-cpp:** VAC1/VAC2 are PortAudio-side virtual cable
clients. For the first beta we can stub these — the only thing the Mic Source
selector needs to do for "VAC1/VAC2" today is route a different audio source
into the TXA producer and leave PanelGain1 at 1.0.

---

## 7. ALC vs Leveler — Distinct Stages

Both are instances of `wcpagc` (`wdsp/wcpAGC.c`), differently parameterized.

### ALC (always-on output limiter)

- **Position**: stage #20 in `xtxa()` — AFTER OSCtrl and the second bandpass,
  BEFORE AM/FM modulators and uslew.
- **`create_txa`** (`TXA.c:311-334`):
  - `run = 1` (always on)
  - `mode = 5`, `envelope detector` (`1` — envelope, not max(I,Q))
  - `max_gain = 1.0` (0 dB) at construction
  - `tau_attack = 0.001` s (1 ms)
  - `tau_decay = 0.010` s (10 ms)
  - `out_targ = 1.0` (clip ceiling)
- **Setup UI** (Setup → DSP → Options-AGC/ALC group, `grpDSPALC`):
  - `udDSPALCMaximumGain` — **default 3, range 0..120 dB**
    (`setup.designer.cs:39526-39553`). Setter at `setup.cs:9296-9301` calls
    `WDSP.SetTXAALCMaxGain(WDSP.id(1, 0), value)` AND mirrors the value into
    `WDSP.ALCGain` (used as a meter-display offset, see §10).
  - `udDSPALCDecay` — **default 10, range 1..50 ms**
    (`setup.designer.cs:39557-39585`). Setter at `setup.cs:9303-9307` calls
    `console.radio.GetDSPTX(0).TXALCDecay = value` → `radio.cs:2961-2976` →
    `WDSP.SetTXAALCDecay(WDSP.id(thread, 0), value)`.
- **DSPTX defaults** `radio.cs:2959-2960`: `tx_alc_decay = 10` (ms).
- **WDSP API symbols**: `SetTXAALCMaxGain`, `SetTXAALCDecay`, `SetTXAALCAttack`,
  `SetTXAALCSt`, `SetTXAALCHang`, `SetTXAALCSlope` (`dsp.cs:146-167`).
- **Why max=3 dB by default**: ALC is the last-mile RF-amplitude limiter and
  must NOT compress program material — it's there to catch peaks the leveler
  + CFC + compressor didn't bound. A tight 3 dB ceiling means the operator's
  drive-knob almost-exactly determines RF PEP. The leveler does the
  heavy program-level adjustment.

### Leveler (slow, pre-clipper, optional)

- **Position**: stage #10 — AFTER EQ and FM pre-emph, BEFORE CFC and the
  bandpass filter chain. So it shapes ENTERING the compressor stack.
- **`create_txa`** (`TXA.c:158-181`):
  - `run = 0` (off by default at construction)
  - `mode = 5`, `max(I,Q)` detector (`0` — energy detector)
  - `max_gain = 1.778` ≈ +5 dB at construction
  - `tau_attack = 0.001` s (1 ms)
  - `tau_decay = 0.500` s (500 ms)
  - `out_targ = 1.05`
- **Setup UI** (Setup → DSP → Options-Leveler group):
  - `TXLevelerMaxGain` default 15 dB, `radio.cs:2978-2995`
    → `SetTXALevelerTop`. Screenshot shows 13 — operator-set.
  - `TXLevelerDecay` default 100 ms, `radio.cs:2998-3015`
    → `SetTXALevelerDecay`. Screenshot shows 90 — operator-set.
  - `TXLevelerOn` `radio.cs:3017-3034`, **default `tx_leveler_on = true`** but
    Setup defaults checkbox unchecked in the operator's screenshot — there's
    a per-profile override that overrides the C# default.
- **WDSP API symbols**: `SetTXALevelerSt`, `SetTXALevelerTop`,
  `SetTXALevelerDecay`, `SetTXALevelerAttack`, `SetTXALevelerHang`
  (`dsp.cs:380-407`).

### "Use Peak Meter Readings for TX COMP and ALC"

`console.cs:12050-12054`:

```csharp
private bool peak_tx_meter = true;  // as opposed to avg
public bool PeakTXMeter { get => peak_tx_meter; set => peak_tx_meter = value; }
```

Gates the meter readback path in `console.cs:24768-24784`. When true, COMP/ALC
multimeter pulls `TXA_COMP_PK / TXA_ALC_PK`; when false, pulls the AV variants.
Pure display behavior; does NOT alter the DSP itself.

---

## 8. CFC / CFCOMP / CESSB

Three distinct optional stages, all default off:

- **`compressor`** (stage #15) — `compress.c` Lyle-Johnson-style
  soft-clip with `gain=3.0` default. Trigger via `SetTXACompressorRun` +
  `SetTXACompressorGain` (`radio.cs:3036-3074`,
  `dsp.cs:234-239`). When on, `bp1` (stage #16) also runs as pre-clip
  bandpass.
- **`cfcomp`** (stage #12) — Continuous Frequency Compressor with post-EQ
  (5-band per-frequency compression + EQ, FFT-based). Default off; enable
  via `SetTXACFCOMPRun` (`dsp.cs:755-757`, `radio.cs` cfc setters around
  3050+). Pre-comp = 0 dB, pre-post-eq = 0 dB at construction.
- **`osctrl`** (stage #17) — CESSB Overshoot Controller (Hershberger W9GR,
  QEX Nov/Dec 2014). Default off; enable via `SetTXAosctrlRun` (`dsp.cs:240-241`,
  `radio.cs:3076-3094`). When on AND `compressor.run == 1`, `bp2` (stage #18)
  also runs as second-stage bandpass shaper. `osgain = 1.95` default
  (`TXA.c:280`); `bw = 3000 Hz`.

**Implication for lyra-cpp:** these three exist between Leveler and ALC. Mic
Gain (PG1) sits BEFORE them, so the Mic Gain UI can be added independently
without touching them — they remain off and the Mic Gain slider behaves
linearly into the bandpass→ALC chain.

---

## 9. TX Bandpass Filter (`SetTXABandpassFreqs`)

**Setup UI** (Setup → Transmit, "Transmit Filter" group):

- `udTXFilterHigh.Value = 3000`, Min=0, Max=20000
  (`setup.designer.cs:48051-48080`).
- `udTXFilterLow.Value = 100`, Min=0, Max=20000
  (`setup.designer.cs:48015-48039`).
- Operator screenshot shows High=4995 Low=50 — custom values; defaults are
  100 / 3000.
- Setter at `setup.cs:9315+` (`udTXFilterHigh_ValueChanged`) drives
  `console.TXFilterHigh / TXFilterLow` → flows into `SetTXFilters(mode, low,
  high)`.

**Per-mode L/H translation** (`console.cs:8079-8118`, `UpdateTXLowHighFilterForMode`):

```
LSB / CWL / DIGL  : f_low = -high, f_high = -low
USB / CWU / DIGU  : f_low = low,   f_high = high
DSB               : f_low = -high, f_high = high
AM / SAM          : f_low = -high, f_high = high      // symmetric around carrier
FM                : f_low = -halfBw, f_high = halfBw  // halfBw = TXFMDeviation + TXFMHighCut
DRM               : f_low = 7000, f_high = 17000
```

These translated values are then handed to `radio.GetDSPTX(0).SetTXFilter(low,
high)` → `WDSP.SetTXABandpassFreqs(channel, low, high)` (`radio.cs:2730-2742`).

**WDSP application** (`TXA.c:792-800`, `SetTXABandpassFreqs`):

```c
if ((txa[channel].f_low != f_low) || (txa[channel].f_high != f_high))
{
    txa[channel].f_low = f_low;
    txa[channel].f_high = f_high;
    TXASetupBPFilters (channel);  // recomputes bp0 (and bp1, bp2 if applicable)
}
```

`TXASetupBPFilters` (`TXA.c:827-901`) is mode-aware — it sets gain=2.0 for
USB/LSB/CW/DIG/SPEC/DRM, gain=1.0 for DSB/AM/SAM/FM, and inverts the
half-band for AM_LSB/AM_USB.

**Important:** the BPF is applied PER-CHANNEL (channel 1 = TX, single
instance), not per-mode. When the operator changes mode, `SetTXAMode()` calls
`TXASetupBPFilters` again with the same `f_low/f_high`, but the actual
filter response changes because the per-mode case handles the symmetry
differently. There is no per-mode preset matrix — Setup → Transmit's Filter
group holds ONE pair, which gets re-interpreted per mode by
`UpdateTXLowHighFilterForMode`.

(There is a separate Setup → Filters tab for **RX** filter shape presets — TX
uses just the single H/L pair, which the operator-screenshot's "TX Filter" UI
exposes.)

---

## 10. Metering — TX Multimeter Sources

**Selector items** (`console.cs:5651-5662`, base list — Ref Pwr/SWR/Fwd SWR are
inserted at runtime for capable hardware in `console.cs:14930-14935`):

```
Fwd Pwr, Mic, EQ, Leveler, Lev Gain, CFC, CFC Comp, COMP, ALC, ALC Comp,
ALC Group, Off
```

The dispatcher is in the meter timer loop at `console.cs:24737-24827`. Meter
sources split into two families:

### Family A — WDSP TXA meters

Read via `WDSP.CalculateTXMeter(1, MeterType)` (`dsp.cs:992-1057`).
`channel = cmaster.CMsubrcvr * cmaster.CMrcvr` (effectively TX channel id =
1). All return dB; `dsp.cs:1056` flips sign with `return -(float)val;`, then
`console.cs:24745+` applies `Math.Max(-30.0f, -...)` to clamp the floor:

| UI item    | WDSP getter                         | UI scale     | Notes                                   |
|------------|-------------------------------------|--------------|-----------------------------------------|
| Mic        | `MIC_PK` (or MIC_AV when !peak)     | -195..0 dBFS | UI clamps to -30 for display, -195 for the floor |
| EQ         | `EQ_PK`                             | -30..0 dB    | Only meaningful when EQ on              |
| Leveler    | `LVLR_PK`                           | -30..0 dB    | Only meaningful when leveler on         |
| Lev Gain   | `LVL_G`                             | 0..+max dB   | Live make-up gain applied by leveler    |
| CFC        | `CFC_PK`                            | -30..0 dB    | Only meaningful when CFC on             |
| CFC Comp   | `CFC_G`                             | 0..+ dB      | Per-band reduction                       |
| COMP       | `CPDR_PK` (peak) or `CPDR` (avg)    | -30..0 dB    | Compressor gain-reduction               |
| ALC        | `ALC_PK` (peak) or `ALC` (avg)      | -30..0 dB    | ALC gain reduction (negative = limiting)|
| ALC Comp   | `ALC_G` + `alcgain` offset (line 1020) | 0..+ dB   | Includes the operator's Max-Gain setting|
| ALC Group  | ALC_PK + (-ALC_G) summed            | -30..+25 dB  | "white left of 0 dB / red 0..+25"       |

Meter scale drawing (`console.cs:22552-22606`) is fixed for the Mic/EQ/Lev/
CFC_PK/COMP/ALC family: the bar spans -30 dB (white) to 0 dB at the **66.5%
point**, then 0 to +12 dB (red) in the right 33.5%. Tick marks at -20, -10,
0, +4, +8, +12.

### Family B — Hardware meters (Fwd Pwr / Ref Pwr / SWR / VDD / ID)

Read via `NetworkIO` getters that poll the HPSDR C&C status bytes received
from the device. Computation happens in the loop at `console.cs:26500-26696`
(every 1 ms during MOX, 10 ms otherwise) and stores into `public float
alex_fwd / alex_rev / drivepwr / calfwdpower / alex_swr`. The multimeter
dispatcher then reads those floats directly.

| UI item    | Underlying NetworkIO get                | Per-model curve (excerpt)                              |
|------------|------------------------------------------|--------------------------------------------------------|
| Fwd Pwr    | `getFwdPower()` ADC raw → `computeAlexFwdPower()` (`console.cs:25228-25297`). Formula: `watts = (volts^2)/bridge_volt`, `volts = (adc - cal_offset)/4095 * refvoltage`. HL2 uses `bridge_volt=1.5, refvoltage=3.3, cal_offset=6` (`console.cs:25269-25273`). | HL2 returns 12-bit ADC on FWD line |
| Ref Pwr    | `getRevPower()` → analogous `computeRefPower()` (`console.cs:25210-25226`). | Same shape, different bridge constants. |
| SWR        | computed in `console.cs:26680-26686` from fwd/rev: `swr = (1+rho)/(1-rho)` (full code at 26661). NaN/inf → 1.0. | Clamped ≥ 1.0; SWR-protect logic at 26649-26696 winds back power. |
| Drive Pwr  | `getExciterPower()` → `computeExciterPower` piecewise polynomial (`console.cs:25299+`) — mW output of the predriver. | HL2-relevant for tune/drive metering when no Alex. |
| Fwd SWR    | Composite display, drives `new_swrmeter_data` from `alex_swr` while showing power. | Multi-bar display. |
| VDD / ID   | (full ANAN only — Andromeda PA telemetry; not present on HL2). | — |

**Update rate**: meter timer loop uses
`Task.Delay(Math.Min(meter_delay, meter_dig_delay))` where
`meter_delay = 50 ms`, `meter_dig_delay = 200 ms` (`console.cs:20197, 20208`).
So analog-meter sources update at 20 Hz, digital readout at 5 Hz. The
HW-status loop at `console.cs:26701` runs at 1000 Hz while MOX, 100 Hz idle.

**"Mic" meter scale** (operator-visible green/yellow/red bands):

- Drawn bar is -30 dB to +12 dB.
- Tick text -30 -20 -10 (low_brush, typically white) then 0 +4 +8 +12
  (high_brush, typically red).
- Numerical value passed in is the negated, clamped dBFS — so a -10 dBFS mic
  level draws as a needle two-thirds of the way up the white zone (which is
  the "good talk level" target).

---

## 11. TX Profile System

Profile rows are DataRow objects in a DataTable persisted via the Thetis
`database.cs` layer. A profile row stores ALL of:

(from `setup.cs:3046-3172` — `isTXProfileSettingDifferent` calls — and the
corresponding `dr["..."] = ...` writes at `setup.cs:3613-3740`)

Key columns (subset; full list is ~70+ items):

| Group         | Column                       | Source UI control                  |
|---------------|------------------------------|------------------------------------|
| Filter        | `FilterLow`                  | `udTXFilterLow.Value`              |
|               | `FilterHigh`                 | `udTXFilterHigh.Value`             |
| Mic           | `Mic_Input_On`               | `radMicIn.Checked`                 |
|               | `Mic_Input_Boost`            | `chk20dbMicBoost.Checked`          |
|               | `Line_Input_On`              | `radLineIn.Checked`                |
|               | `Line_Input_Level`           | `udLineInBoost.Value`              |
|               | `Mic_Gain` (per-profile; stored as integer dB) | `ptbMic.Value` |
| ALC           | `ALC_MaximumGain`            | `udDSPALCMaximumGain.Value`        |
|               | `ALC_Decay`                  | `udDSPALCDecay.Value`              |
| Leveler       | `Leveler_On`                 | `chkDSPLevelerEnabled.Checked`     |
|               | `Leveler_MaximumGain`        | `udDSPLevelerMaximumGain.Value`    |
|               | `Leveler_Decay`              | `udDSPLevelerDecay.Value`          |
| COMP          | `COMP_On`                    | compressor enable                  |
|               | `COMP_Gain`                  | `udTXCompandLevel.Value`           |
|               | `CESSB_On`                   | osctrl enable                      |
| CFC           | `CFC_On`, `CFC_PostEQ_On`, `CFC_Pre_Comp`, `CFC_Pre_Eq`, per-band freq/gain/eq arrays | the CFC group |
| EQ            | `EQ_On`, `EQ_Pre_Gain`, `EQ10_Freq0..9`, `EQ10_Gain0..9` | `tpDSPEQ` tab     |
| FM            | `FM_Deviation`, `FM_PreEmph_On`, CTCSS settings                  | FM group           |
| VAC1 link     | `VAC1_PTT_OverRide`          | `chkVACAllowBypass.Checked`        |
| Phase rot     | `Phase_Rot_Stages`           | phase rotator                      |
| TX Profile metadata | `Name`, `Default` flag | combobox                           |

Save flow: `setup.cs:3613-3740` packs all the controls into the DataRow,
`database.cs` upserts. Load flow: `setup.cs:9493-9603+` pulls each column
back into the same controls. "Highlight TX Profile Save Items" enables
visual diff between current control state and saved-profile state
(`setup.cs:3434-3540`).

**Implication for lyra-cpp:** persistence is per-profile, not per-mode-per-band.
The same profile can be "active" in multiple modes (Setup → TX → "Default" /
"Default Dig" / "Default FM" / "Default AM"). Lyra-cpp can defer the profile
system entirely for the first beta — just save Mic Gain / Filter / Boost
flags in the existing per-side QSettings store.

---

## 12. Mic-Bench Taps — dBFS at Each Stage

Six natural metering taps that lyra-cpp can place ticks on, mapped to WDSP
meters and the chain position:

| Tap                                 | Stage in TXA       | WDSP getter         | Default UI label  |
|-------------------------------------|--------------------|---------------------|-------------------|
| Raw mic in (post-codec, pre-PG1)    | between gen0 (#2) and panel (#3) | not directly exposed; could mirror MIC_AV before PG1 — Thetis doesn't currently | — |
| Post-PanelGain1 (post Mic Gain)     | after #3 panel, BEFORE everything except phrot | `TXA_MIC_PK` / `TXA_MIC_AV` (mic meter sits between phrot and amsq, so it reads post-PG1) | "Mic" |
| Post-EQ                             | after #7           | `TXA_EQ_PK / AV`    | "EQ"              |
| Post-Leveler                        | after #10          | `TXA_LVLR_PK / AV` + `TXA_LVLR_GAIN` (live make-up) | "Leveler" / "Lev Gain" |
| Post-CFC                            | after #12          | `TXA_CFC_PK / AV / GAIN` | "CFC" / "CFC Comp" |
| Post-Compressor                     | after #15          | `TXA_COMP_PK / AV`  | "COMP"            |
| Post-ALC (final, gain-reduction)    | after #20 (alcmeter at #26 reads same buffer) | `TXA_ALC_PK / AV / GAIN` | "ALC" / "ALC Comp" |
| Final I/Q amplitude (post-OSCTRL, post-rsmpout) | after #30   | `TXA_OUT_PK / AV`   | (drives "PEP" PA-drive bar) |

For the lyra-cpp Mic / Comp / ALC meter ticks in v1, the minimum useful set
is: **Mic (post-PG1), COMP (post-compressor when on), ALC (final reduction),
PEP (Fwd Pwr from HW)**. That matches the operator's existing mental model
for HL2 bench TX.

Note that micmeter is positioned at `xtxa()` line 563, AFTER `phrot` and
BEFORE `amsq+eqp`. So the "Mic" reading already includes the operator's Mic
Gain (and the optional phase rotator) but NOT the EQ or any compression
stages. This is exactly the behavior an operator expects from a Mic meter.

---

## 13. Open Questions / Lyra-cpp Implications

1. **Mic-source codec selection on HL2 is wire-level.** Mic In / Line In /
   Line In Boost / 20 dB Mic Boost are HL2 C&C bits in protocol-1 frame
   index 2 (C&C bytes C1/C2). We don't currently set any of these — the
   first-power-on default is whatever the HL2 firmware boots with. Beta-1
   action: send a known mic-input C&C frame at TX open (mirror Thetis's
   `SetMicGain()` boot sequence).

2. **VAC isn't wired at all.** All of §5 / §6 (mode-dependent source select,
   VAC bypass on PTT, VOX-uses-MIC checkbox) assumes a PortAudio-style VAC
   driver. For beta-1 the Mic Source selector can collapse to 2 positions
   (Mic In, Line In) and defer VAC1/VAC2 to v2. The 4-position selector in
   the operator's task list can be UI-only with VAC1/VAC2 disabled and a
   tooltip explaining "available in v2".

3. **ALC defaults are wrong in our build.** `TXA.c:322` constructs ALC with
   `max_gain=1.0` (0 dB) and Thetis immediately overwrites this on first
   Setup load — `setup.cs:9296-9301` calls
   `SetTXAALCMaxGain(channel, udDSPALCMaximumGain.Value)` with the profile
   default of **3 dB**. Lyra-cpp must call `SetTXAALCMaxGain(channel, 3.0)`
   during channel-open or the first TX will hit a hard 0-dB output limiter
   and clip prematurely.

4. **Leveler default ON/OFF mismatch.** `radio.cs:3017` defaults
   `tx_leveler_on = true` but the operator-screenshot Setup default is
   unchecked. The first `TXProfile load` event overrides it from the row's
   `Leveler_On` column. Without a profile system in lyra-cpp, we should
   match the operator-visible Setup default and force leveler OFF until the
   operator explicitly enables it.

5. **MicPreamp = 0.5 = -6 dB initial state.** `radio.cs:4127-4128` starts
   `mic_gain = 0.5`. Without a Mic Gain UI, lyra-cpp's `SetTXAPanelGain1`
   defaults to 1.0 from `create_panel(..., 1.0, ...)` at `TXA.c:65`. Our
   bench result of 0.2 W out is consistent with this: WDSP defaults at unity
   + ALC max-gain at 0 dB = whatever the mic envelope happens to be, hard-
   capped, with no operator-tunable headroom.

6. **Mic boost is HW, not software.** When the operator-task #34 (Mic
   Source selector) is implemented, "20 dB Boost" must be a separate
   checkbox that toggles the C&C bit, NOT a software multiplier on the WDSP
   side. Doubling it as both will double-count the boost.

7. **Peak vs Average meter readings is a display-only flag.** Don't expose
   it as a DSP tuning knob — it's `peak_tx_meter` (`console.cs:12050`) and
   gates only the meter timer's WDSP getter choice. Lyra-cpp can ship peak-
   only for v1 and add an "average" toggle when the multimeter is built.

8. **PTT-from-mic** (operator task #36) is bit 0 of
   `NetworkIO.nativeGetDotDashPTT()` (`HPSDR/NetworkIOImports.cs:189`) —
   meaning the HL2 firmware combines hand-mic PTT and rear-panel PTT-in into
   the same bit in C&C status byte 0. Lyra-cpp's TxStateMachine already has
   software-PTT state plumbing; bench wiring needs `Hl2Status::ptt_bit` from
   the EP6 frame parser to gate `software_ptt || hardware_ptt`.

9. **TX bandwidth's per-mode interpretation is in console.cs, not WDSP.**
   `UpdateTXLowHighFilterForMode` (`console.cs:8079-8118`) is the canonical
   transform from operator H/L spinner values to WDSP f_low/f_high. Lyra-cpp
   must replicate this transform table or the SSB bandpass will be wrong on
   LSB / AM / DSB. For beta-1 SSB only: USB → pass-through, LSB → negate-and-
   swap.

10. **No per-mode TX filter preset matrix.** Thetis stores ONE H/L pair per
    TX profile and re-interprets it per mode. So the Setup → Filters tab
    visible in operator screenshots is for **RX shape presets only** — TX
    has no equivalent. Don't waste UI on it.

---

## Appendix A — Symbol Cross-Reference

| Concept                    | C# Setter (Console)                       | WDSP C entrypoint                  |
|----------------------------|--------------------------------------------|-----------------------------------|
| Mic Gain (PG1, linear)     | `DSPTX.MicGain` (radio.cs:4129)            | `SetTXAPanelGain1`                |
| TX mode select             | `radio.cs SubAMMode + console.RX1DSPMode`  | `SetTXAMode`                      |
| Bandpass freqs             | `DSPTX.SetTXFilter`                        | `SetTXABandpassFreqs`             |
| ALC Max Gain               | `Setup udDSPALCMaximumGain` → `WDSP.ALCGain` (also Audio-side display offset) | `SetTXAALCMaxGain` |
| ALC Decay                  | `DSPTX.TXALCDecay`                         | `SetTXAALCDecay`                  |
| Leveler on/off             | `DSPTX.TXLevelerOn`                        | `SetTXALevelerSt`                 |
| Leveler Max Gain           | `DSPTX.TXLevelerMaxGain`                   | `SetTXALevelerTop`                |
| Leveler Decay              | `DSPTX.TXLevelerDecay`                     | `SetTXALevelerDecay`              |
| Compressor on/off          | `DSPTX.TXCompandOn`                        | `SetTXACompressorRun`             |
| Compressor gain            | `DSPTX.TXCompandLevel`                     | `SetTXACompressorGain`            |
| OSCtrl (CESSB) on/off      | `DSPTX.TXOsctrlOn`                         | `SetTXAosctrlRun`                 |
| CFC on/off                 | `DSPTX.TXCFCOMPRun`                        | `SetTXACFCOMPRun`                 |
| EQ on/off                  | `DSPTX.TXEQOn`                             | `SetTXAEQRun`                     |
| AM Carrier Level           | `DSPTX.TXAMCarrierLevel`                   | `SetTXAAMCarrierLevel`            |
| FM Deviation               | `DSPTX.TXFMDeviation`                      | `SetTXAFMDeviation`               |
| FM AF filter               | `DSPTX.TXFMLowCut / TXFMHighCut`           | `SetTXAFMAFFilter`                |
| Mic Boost (codec PGA)      | `console.MicBoost` → `SetMicGain()`        | `NetworkIO.SetMicBoost` (C&C bit) |
| Line In select             | `console.LineIn` → `SetMicGain()`          | `NetworkIO.SetLineIn`             |
| Line In Boost (0..31 dB)   | `console.LineInBoost` → `SetMicGain()`     | `NetworkIO.SetLineBoost`          |
| Mic XLR / Tip-Ring         | `console.MicXlr` → `SetMicXlr()`           | `NetworkIO.SetMicXlr`             |
| TX output gain (drive)     | `Audio.RadioVolume` → `CMSetTXOutputLevel` | `NetworkIO.SetOutputPower`        |
| TX Meter readback          | `WDSP.CalculateTXMeter(1, MeterType)`      | `GetTXAMeter(channel, txaMeterType)` |
| Fwd Power readback         | `console.computeAlexFwdPower()`            | `NetworkIO.getFwdPower()` (ADC counts) |
| Rev Power readback         | `console.computeRefPower()`                | `NetworkIO.getRevPower()`         |
| Exciter Drive Power        | `console.computeExciterPower()`            | `NetworkIO.getExciterPower()`     |
| SWR                        | computed in `console.cs:26680` from fwd/rev| (none — derived)                  |
| Mic-PTT bit                | `nativeGetDotDashPTT() & 0x01`             | (HL2 C&C status byte 0)           |

## Appendix B — File-Level Index (where each topic lives)

- TXA chain order, defaults, mode switch:
  `wdsp/TXA.c` (935 lines, read whole)
- Mic Gain enum + PanelGain1 setters:
  `wdsp/patchpanel.c:201-227`
- Console-side Mic plumbing:
  `console.cs:13286-13332, 29450-29533, 41781-41800`
- Audio.cs gain/source plumbing:
  `audio.cs:215-298, 456-633` (MicPreamp, VACPreamp, VACEnabled, VACBypass)
- cmaster mode→source decision tree:
  `cmaster.cs:1070-1110` (`CMSetTXAPanelGain1`)
- radio.cs DSPTX value cache + setter pattern:
  `radio.cs:2730-2782` (filter), `2959-3094` (ALC/Leveler/COMP/OSCtrl),
  `4127-4144` (MicGain)
- Setup handlers:
  `setup.cs:7851-7855` (mic boost), `9296-9307` (ALC), `9855-9870`
  (Mic Min/Max + Line Boost), `12392-12395` (VAC bypass), `14567-14589`
  (Mic In / Line In radio)
- TX filter L/H per-mode translation:
  `console.cs:8079-8118`
- TX meter dispatch:
  `console.cs:24737-24827` (main loop), `dsp.cs:992-1057`
  (`CalculateTXMeter`)
- HW power readback:
  `console.cs:25210-25297` (fwd/rev), `25299-25400+` (exciter),
  `26500-26713` (status loop + SWR)
- PTT input bit:
  `HPSDR/NetworkIOImports.cs:188-189`
- VAC bypass override decision:
  `console.cs:26034-26159`
