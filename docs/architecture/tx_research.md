# TX bring-up research — HPSDR Protocol 1, HL2 / HL2+ (clean-room from Thetis)

**Purpose.** Authoritative reference for implementing TRANSMIT in Lyra-SDR-cpp
(C++23 / Qt6). Everything here was extracted by reading the open-source
**Thetis 2.10.3.13** source + its bundled **WDSP** library (GPL, Warren Pratt
NR0V). We implement Lyra-native code; WDSP DSP may be ported/called directly
with attribution. **No reference-app name appears in shipped code/comments —
provenance lives only in this doc.**

**Scope.** HL2 / HL2+ are **Protocol 1 only**. ANAN and Brick SDR (future)
bring **Protocol 2** — P2 divergences are flagged throughout so we don't paint
into a corner, but P2 is not implemented now. PureSignal is wanted; §5 is the
build-requirements map.

**Source roots** (read-only reference, not in this repo):
`D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\` —
`ChannelMaster\` (networkproto1.c, network.c, network.h, cmaster.cs, sync.c),
`Console\` (console.cs, radio.cs, setup.cs, PSForm.cs, HPSDR\), `wdsp\`.

---

## 1. HPSDR Protocol-1 TX wire path (HL2)

HL2 = `HPSDRModel_HERMESLITE` (value 14, `network.h:475`). It dispatches to a
dedicated **`WriteMainLoop_HL2`** (`networkproto1.c:869-1201`), *not* the
generic loop — TX register details below are the HL2 path.

### 1.1 EP2 host→radio frame (TX audio + I/Q)
- UDP datagram = **1032 bytes**: `0xEF 0xFE 0x01` + endpoint + 4-byte **big-endian**
  outbound seq, then 1024-byte payload (`MetisWriteFrame`, `networkproto1.c:216-237`).
  **TX goes to endpoint `0x02`** (`:864`/`:1198`).
- Payload = **two 512-byte USB frames**. Each: `0x7F 0x7F 0x7F` sync + C0..C4 +
  504 bytes of samples (`:600-602`, `:845-849`/`:1186-1190`).
- Sample packing (`sendProtocol1Samples`, `:1204`): per 8-byte tuple =
  **L(16) R(16) I(16) Q(16)**, each **16-bit signed big-endian, high byte first**
  (`:1257-1258`). 63 tuples × 8 = 504 B/frame. Quantize: `round(sample*32767)`
  half-away-from-zero (`:1245-1246`). **When not transmitting the I/Q half is
  zeroed** (`if(!XmitBit) memset(outIQbufp,0)`, `:1227`).
- **MOX/PTT = C0 bit 0 of *every* frame**: `C0 = (unsigned char)XmitBit` (`:896`).
  Upper C0 bits select the addressed C&C register.

### 1.2 C&C registers carrying TX state (`WriteMainLoop_HL2`)
HL2 rotates `out_control_idx` 0..18 (`:946-1184`, wraps at 18). Addressed by
`C0 |= addr`:

| TX state | C0 addr | bytes | notes |
|---|---|---|---|
| **TX VFO freq** | `0x02` (case 1) | C1..C4 = freq **BE** | `tx[0].frequency` (`:974-980`) |
| **TX drive / output power** | `0x12` (case 10) | **C1** = `tx[0].drive_level` | `:1078`; HL2's real power knob. Set `netInterface.c:527-529`; SWR/cal scaling upstream in `SetOutputPowerFactor()` |
| **PA enable** | `0x12` (case 10) | **C2 bit 3** (Apollo path) + legacy C3 bit7 | see §2.4 — the real HL2 enable is the Apollo "Enable PA" bit, **NOT** C3-bit7 |
| **TX step attenuator** | `0x1C` (case 4) **C3** = `tx_step_attn & 0x1F` (5-bit) | AND `0x14` (case 11) **C4** = `(tx_step_attn & 0x3F)\|0x40` when XmitBit (6-bit + enable) | `:1019`, `:1099-1100`. Encoding `31-x` applied **C#-side**, HL2 only (§2.1) |
| **MOX/XmitBit** | every frame | C0 bit0 | `:896`; set via `SetPttOut`→`XmitBit` (`netInterface.c:362-371`) |
| TX latency / PTT-hang | `0x2e` (case 17) | C3=`ptt_hang&0x1F`, C4=`tx_latency&0x7F` | `:1162-1167` (HL2-only) |
| reset-on-disconnect | `0x74` (case 18) | | `:1170-1175` (HL2-only) |

HL2 also preempts the rotation with **I2C transactions** when its I2C queue is
non-empty (`C0 |= (0x3c<<1) | (ctrl_request<<7)`, `:903-943`) — absent from the
generic loop. A TX-bit change forces an immediate DDC0-freq C&C re-send
(`:605-610`).

> **nddc=4 on HL2.** DDC count encoded `C4 |= (nddc-1)<<3` (`:968`) and in the
> start/force frame (`:118`). Several DDC freq slots are hardwired to
> `tx[0].frequency` (cases 6/7, `:1036`/`:1046`) — TX freq is mirrored to the
> feedback DDCs (matters for PureSignal, §5).

### 1.3 EP6 radio→host TX telemetry (`MetisReadThreadMainLoop_HL2`, `:422-586`)
- Control bytes `ControlBytesIn[0..4] = bptr[3..7]` (`:475-476`). HL2 first checks
  `ControlBytesIn[0]&0x80` for **I2C-read responses** (`:478-493`) before PTT decode.
- **Hardware PTT-in / dash / dot**: `ptt_in = C0 & 0x1`, dash `<<1`, dot `<<2`
  (`:496-498`). **Level-driven, no debounce.**
- **Status rotation keyed on `C0 & 0xF8`**, each value pairs two bytes BE:
  - `0x00`: ADC0 overload = C1 bit0 (`:501-503`)
  - `0x08`: `exciter_power`=(C1,C2) (AIN5/drive); `fwd_power`=(C3,C4) (AIN1) (`:505-509`)
  - `0x10`: `rev_power`=(C1,C2) (AIN2); `user_adc0`=(C3,C4) (AIN3) (`:510-514`)
  - `0x18`: `user_adc1`=(C1,C2) (AIN4); `supply_volts`=(C3,C4) (AIN6) (`:515-518`)
  - `0x20`: ADC0/1/2 overload bits (`:519-523`)
- **HL2 conversions (console.cs):** PA **current** from `getUserADC0()` (AIN3, slot
  0x10 C3:C4) — `computeHermesLitePAAmps`, `:25552-25565`; HL2 amps =
  `((3.26*(raw/4096))/50)/0.04 / (1000/1270)` (`:25127-25130`). Supply volts =
  `(raw/4095)*5*(23/1.1)` (`:25086-25089`). Temp from `getExciterPower()` (AIN5,
  slot 0x08 C1:C2) = `(3.26*(raw/4096)-0.5)/0.01` °C (`:25079`). Fwd/Rev power use
  peak-decayed globals with HL2 case `bridge_volt=1.5, refvoltage=3.3, offset=6`
  (`:25195-25199`, `:25269-25273`); W = volts²/bridge_volt.
  > ⚠ These AINx→slot mappings can be **gateware-rev-specific** on HL2+. Treat
  > as a starting point; calibrate against a known reference on the operator's unit.

### 1.4 Protocol-2 flag (do not implement now)
P2 lives in `network.c` with a **fixed byte-offset** command layout (not a
C0-addressed rotation): PA-enable `packetbuf[58]` with **inverted polarity**
(`network.c:891`), TX freq `[329..332]` BE (`:995-998`), drive `[345]` (`:1001`),
per-ADC TX attn `[57/58/59]` (`:1225-1229`), separate high-priority vs general
command ports (`network.h:58-59`). When ANAN/Brick land, write a **separate
fixed-layout P2 encoder**, not a variant of the P1 rotation.

---

## 2. HL2 / HL2+ TX hardware quirks

### 2.1 TX step attenuator + ATT-on-TX (RX-ADC protection — safety-critical)
- **Range −28..+31 dB** (UI min −28, `setup.cs:1084`/`:20351`). Negative = gain.
- **Wire encoding `31 - x`, HL2 only.** Every site branches on `HERMESLITE` and
  sends `SetTxAttenData(31 - x)` (`console.cs:10657-10661`, `:19164-19167`,
  `:27814-27817`); other radios send raw. ATT-off → `SetTxAttenData(0)`.
- **Shared AD9866 PGA**: HL2 RX1 step-att uses the identical `31-x` inversion
  (`:11075`,`:11251`,`:19380`) → **TX att and RX1 att are the same physical
  attenuator.** Forcing TX att on keydown is exactly what protects the RX ADC.
- `m_bATTonTX` **default true** (`:19148`). On keydown (`:30293-30322`): compute
  `txAtt = getTXstepAttenuatorForBand`, then **force 31** when
  `(!FWCATUBypass && _forceATTwhenPSAoff)` OR mode is CW (`:30310-30312`;
  `_forceATTwhenPSAoff` default true). `SetupForm.ATTOnTX=31` → wire
  `SetTxAttenData(31-31)=0` = max physical attenuation = RX-ADC safe. Restored
  on keyup via `updateAttNudsCombos()`+`UpdatePreamps()` (`:30391-30410`).
- **Auto-ATT-on-TX** (overload tracking) `_auto_attTX_when_not_in_ps` default
  false (`:21331`): steps att up to 31 on ADC0 overload while `_mox`, unwinds on
  keyup (`:21587-21638`).
- **Lyra-cpp takeaway:** ATT-on-TX is mandatory RX-ADC protection. Implement the
  single shared step-attenuator with the `31-x` HL2 encoding, force max-att (wire
  0) on keydown (always in CW; in SSB unless an ATU-bypass option is set), restore
  the band value on keyup. This actuator is **shared with PureSignal's
  auto-attenuator** (§5) — keep one writer.

### 2.2 Mic input (HL2+ AK4951 vs standard HL2)
- Thetis sends standard P1 control bits, **not** AK4951 I2C: `SetMicBoost`,
  `SetLineIn`, `SetLineBoost`, `SetMicXlr` (`console.cs:41781-41800`). Defaults:
  MicBoost true, LineIn false, MicXlr true. The AK4951 register programming is in
  HL2 firmware; host only flips these control bits. **No codec I2C path in the
  reference.** Standard HL2 (no codec) → PC-soundcard mic; the host code path is
  identical for both.
- Mic *samples* arrive in the EP6 mic slot (firmware-handled). (§10 open question
  in the Python notes: AK4951 mic-slot value-vs-zero at rest — verify on hardware.)

### 2.3 TR sequencing delays (defaults + meaning, `console.cs`)
| knob | default | gates |
|---|---|---|
| `rf_delay` | **30 ms** | keydown: gap between MOX/relay asserted and TX DSP channel started — **external-amp hot-switch protection** (`:19800`, applied `:30342-30345`) |
| `mox_delay` | **10 ms** | keyup non-CW: let in-flight samples clear before RX (`:19772`, `:30367`) |
| `ptt_out_delay` | **20 ms** | keyup: HW T/R settle before RX DSP restart (`:19807`, `:30377`) |
| `space_mox_delay` | **0 ms** | start of keyup branch (`:19782`, `:30352`) |
| `key_up_delay` | **10 ms** | keyup CW (non-FW-keyer); also added to CW hang (`:19790`, `:30362`) |
- HL2-only extra latency knobs: `SetTxLatency` (`udTxBufferLat`) and `SetPttHang`
  (`udPTTHang`) — HL2 firmware TX-buffer latency / PTT hang. Make all of the above
  operator-configurable; defaults above.

### 2.4 PA enable (HL2)
- The Apollo checkboxes are **repurposed for HL2** (`setup.cs:20228-20240`):
  `chkApolloTuner.Text = "Enable PA"`, `chkApolloFilter.Text = "Enable Full Duplex"`.
- **"Enable PA" → `EnableApolloTuner(1)`** (`console.cs:19207-19213`) — the host→radio
  bit that biases the onboard PA. **"Enable Full Duplex" → `EnableApolloFilter(1)`**
  (`setup.cs:15892-15895`).
- The legacy "C3 bit-7 pa" path is **not** used for HL2. (HL2+ gateware reads the
  Apollo/PA-enable bit; confirm the exact wire bit on the operator's gateware —
  this is the difference between "keys but no RF" and real output.)
- Full-duplex (RX-during-TX): `full_duplex` default false; when true the keydown
  handler **skips RX shutdown** (`console.cs:30274`). On HL2 this is the same
  "Enable Full Duplex" bit.

### 2.5 CW transmit state bits
During CW TX, HL2 replaces the **TX I-sample 16-bit word** with a keyer bit field
(`networkproto1.c:1241-1259`): when `cw_enable && j==1 && HERMESLITE`,
`temp = (cwx_ptt<<3 | dot<<2 | dash<<1 | cwx) & 0x0F` — **bit3 = CWX PTT (HL2-only)**,
non-HL2 omits bit3. CW C&C frames: `0x1e` carries cw_enable/sidetone/rf_delay
(`:802-807`), `0x20` carries hang_delay/sidetone_freq (`:810-814`). (CW TX is a
later sub-phase; modulator I is overwritten by these bits on the wire.)

### 2.6 EER / misc
EER lives in ChannelMaster (`cmaster.cs:278-297`), LR audio overwritten with
envelope from IQ+256 when `peer->run && XmitBit` (`networkproto1.c:1222-1226`).
Not needed for SSB bring-up.

---

## 3. WDSP TXA DSP chain (the core modulator)

### 3.1 `xtxa()` execution order (`wdsp/TXA.c:557-592`)
Samples flow `inbuff`(in_rate) → `midbuff`(dsp_rate, in-place) → `outbuff`(out_rate):

| # | block | field | run |
|---|---|---|---|
| 1 | input resampler | `rsmpin` | iff in≠dsp rate |
| 2 | gen0 (PreGen, input tone) | `gen0` | off |
| 3 | **panel — MIC gain (`gain1`)** | `panel` | **always** |
| 4 | phase rotator | `phrot` | off |
| 5 | mic meter | `micmeter` | always |
| 6-7 | TX squelch / downward expander | `amsq` | off |
| 8-9 | TX EQ + meter | `eqp` | off |
| 10 | FM pre-emph (pos 0) | `preemph` | FM |
| 11 | **leveler (wcpagc mode 5)** | `leveler` | off by default (Thetis turns on) |
| 12 | leveler meter | `lvlrmeter` | — |
| 13-14 | CFC speech comp + meter | `cfcomp` | off |
| 15 | **bandpass bp0 (primary BPF / sideband select)** | `bp0` | **always** |
| 16 | compressor (COMP) | `compressor` | off |
| 17 | bandpass bp1 | `bp1` | only if COMP |
| 18 | CESSB overshoot | `osctrl` | off |
| 19 | bandpass bp2 | `bp2` | only if COMP+CESSB |
| 20 | comp meter | `compmeter` | — |
| 21 | **ALC (wcpagc mode 5)** | `alc` | **always on** |
| 22 | AM/DSB modulator | `ammod` | AM/SAM/DSB |
| 23 | FM pre-emph (pos 1) | `preemph` | FM |
| 24 | FM modulator | `fmmod` | FM |
| 25 | gen1 (PostGen — **TUN / two-tone**) | `gen1` | off |
| 26 | up-slew (AM/FM/gens) | `uslew` | — |
| 27 | ALC meter | `alcmeter` | — |
| 28 | spectrum tap | `sip1` | display |
| 29 | **PureSignal IQ correction** | `iqc` | off (§5) |
| 30 | compensating FIR | `cfir` | **P2 only** |
| 31 | output resampler | `rsmpout` | iff dsp≠out |
| 32 | output meter (PWR) | `outmeter` | at out_rate |

**SSB bring-up needs only the always-on stages: panel(mic gain) → bp0 → ALC →
out.** Leveler is a quality add. Everything else is later sub-phases.

### 3.2 SetTXA* API (signatures matter — int ms vs double dB)
- **Mode**: `SetTXAMode(ch, mode)` `TXA.c:752` (enum `TXA.h:31`: LSB=0 USB=1 DSB=2
  CWL=3 CWU=4 FM=5 AM=6 DIGU=7 SPEC=8 DIGL=9 SAM=10 …). Calls `TXASetupBPFilters`.
- **Bandpass**: `SetTXABandpassFreqs(ch, f_low, f_high)` `TXA.c:791` (use the TXA.c
  one; per-stage version is commented out). `SetTXABandpassRun(ch,int)` drives bp1.
- **Mic/panel gain**: `SetTXAPanelGain1(ch, double gain)` `patchpanel.c:208` —
  **linear, not dB**.
- **Leveler** (wcpAGC.c): `SetTXALevelerSt(ch,int)`:612, `…Attack(ch,int ms)`:620,
  `…Decay(ch,int ms)`:629, `…Hang(ch,int ms)`:638, `SetTXALevelerTop(ch,double dB)`:647
  (applied `pow(10,dB/20)`).
- **ALC** (wcpAGC.c): `SetTXAALCSt`:569, `…Attack(int ms)`:577, `…Decay(int ms)`:585,
  `…Hang(int ms)`:594, `SetTXAALCMaxGain(ch,double dB)`:603. **No SetTXAALCThresh** —
  threshold is the fixed create-time `out_targ=1.0`; only MaxGain is tunable.
- **Compressor**: `SetTXACompressorRun(ch,int)`:99 (also enables bp1),
  `SetTXACompressorGain(ch,double dB)`:111.
- **CFC**: `SetTXACFCOMPRun/Position/profile/Precomp/PeqRun/PrePeq` (`cfcomp.c:632+`).
- **EQ**: `SetTXAEQRun`:742, `SetTXAEQProfile(ch,n,F,G,Q)`:779, `SetTXAGrphEQ10`:859
  (`eq.c`).
- **PHROT** (UPPERCASE!): `SetTXAPHROTRun`:664, `SetTXAPHROTCorner(ch,double)`:674,
  `SetTXAPHROTNstages(ch,int)`:685 (`iir.c`).
- **CESSB**: `SetTXAosctrlRun(ch,int)` `osctrl.c:141`.
- **FM emph**: `SetTXAFMEmphPosition(ch,int)` `emph.c:108`, `SetTXAFMPreEmphFreqs`:145.
- **Gen**: gen0 = `SetTXAPreGen*` (input), gen1 = `SetTXAPostGen*` (output — incl.
  **TUN tone** + **two-tone** `SetTXAPostGenTTMag/TTFreq`, `gen.c:817/826`).
- **Meter**: `double GetTXAMeter(ch, mt)` `meter.c:150`. `txaMeterType` (`TXA.h:49`):
  MIC_PK=0 MIC_AV=1 EQ_PK=2 EQ_AV=3 LVLR_PK=4 LVLR_AV=5 LVLR_GAIN=6 CFC_*=7-9
  COMP_*=10-11 ALC_PK=12 ALC_AV=13 **ALC_GAIN=14** OUT_PK=15 (PWR) OUT_AV=16.
  > TX meters have **no AGC** — AGC is RX-only. The TX meter row shows ALC/PWR
  > while transmitting.

### 3.3 OpenChannel for TX (`channel.c:76`)
`OpenChannel(ch, in_size, dsp_size, in_rate, dsp_rate, out_rate, type, state,
tdelayup, tslewup, tdelaydown, tslewdown, bfo)`. **type=1 for TX.** **bfo = "block
until output available"** (`iobuffs.c:494`) — a TX feeding the wire at fixed rate
uses **bfo=1**. `SetInputSamplerate`/`SetOutputSamplerate` are **shared RX/TX**,
dispatch by `ch.type` (`channel.c:196/227`). Push/pull via `fexchange0(ch, in, out,
&err)` (`iobuffs.c:465`) — `in` = baseband audio-as-complex, `out` = IQ to radio.
Channel-id convention: logical TX = `WDSP.id(1,0)` (`dsp.cs:934-937`).

### 3.4 Mode → sideband (the SSB sign convention)
`UpdateTXLowHighFilterForMode` (`console.cs:8079`): **LSB/CWL/DIGL** → `l=-high,
h=-low` (negative baseband); **USB/CWU/DIGU** → `l=low, h=high` (positive baseband);
**DSB/AM/SAM** → symmetric `-high..high`. `TXASetupBPFilters` (`TXA.c:827`) finalizes
bp0 (gain 2.0 SSB). **The wanted sideband is emitted on the matching baseband sign —
USB on positive (0..+f), LSB on negative (−f..0).** (This is the TX analogue of the
RX convention; combined with the HL2 mirrored baseband it lands correct on air.
Verify with a synthetic-tone bench, USB+LSB mirror-symmetric.)

### 3.5 Thetis defaults (radio.cs / create-time)
Leveler: attack 1 ms, decay 100 ms (runtime) / 500 ms (create), maxgain 15 dB
(runtime) / ~5 dB (create), **on by default**. ALC: attack 1 ms, decay 10 ms,
maxgain 1.0, out_targ 1.0, **always on**. PHROT corner 338 Hz, 8 stages. Pre-emph
300–3000 Hz, pos 1. bp0 gain 2.0, osctrl clip 1.95, rsmpout 0.980.

### 3.6 Port gotchas (carry into the C++ engine)
1. attack/decay/hang setters = **int ms**; gain/top = **double dB** (internal
   `pow(10,dB/20)`). 2. PHROT setters are **all-caps** `SetTXAPHROT*`. 3. **No ALC
   threshold setter** — MaxGain only. 4. bp1/bp2 enabled implicitly by COMP/CESSB
   run setters. 5. gen0=input, gen1=output (TUN/two-tone). 6. In/Out samplerate
   setters shared with RX.

---

## 4. MOX / PTT state machine + RX↔TX sequencing

**Single-state FSM.** Every TX trigger (CAT, TCI, hardware-PTT, CW, VOX, spacebar,
TUN, two-tone) ORs into **one** bit, `chkMOX.Checked`; the whole hardware sequence
is in one handler `chkMOX_CheckedChanged2` (`console.cs:30058`). `PollPTT()`
(`:26003`) samples sources each loop: `mic_ptt = dotdashptt & 0x01` (hardware PTT-in,
level-driven, no debounce), `cw_ptt`, `vox_ptt`, `cat_ptt`; when `!_mox` a true
source sets the mode + `chkMOX.Checked=true`; when keyed a `switch` releases when the
owning source drops.

### 4.1 Keydown (RX→TX), exact order
1. `_mox=true` (`:30109`); compute/validate TX freq (`:30112-30239`).
2. **Stop RX DSP**: `SetChannelState(id(0,0),0,1)` — **blocking flush** (`:30289`).
3. TX att / preamp setup (§2.1, `:30293-30327`).
4. `UpdateAAudioMixerStates()`, `UpdateDDCs()` (`:30329-30330`).
5. **`HdwMOXChanged(tx,freq)`** (`:30332`): writes **TX DDS freq** (`:29764`) →
   `SetTRXrelay(1)` (`:29786`) → **`SetPttOut(1)`** = assert wire MOX bit (`:29794`).
6. `cmaster.Mox=tx` loads the XmitBit into outgoing frames (`:30335`).
7. **`Thread.Sleep(rf_delay)`** (30 ms) → `AudioMOXChanged(tx)` (mute/AF→MON) →
   **`SetChannelState(id(1,0),1,0)`** = start TX DSP (non-blocking up-ramp) (`:30342-30345`).
   *(CW: skip the TX-channel start; FPGA keyer keys the carrier.)*

**Invariant: TX freq is written BEFORE the relay and BEFORE the MOX bit, and RF
(TX DSP up-ramp) only after `rf_delay`.**

### 4.2 Keyup (TX→RX), exact order
1. `space_mox_delay` (0 ms) `:30352`; `_mox=false` `:30355`.
2. **Stop TX DSP**: `SetChannelState(id(1,0),0,1)` — **blocking until the TX
   down-ramp flushes** (`:30357`).
3. `mox_delay` (10 ms) drain — CW uses `key_up_delay` (`:30359-30369`).
4. `AudioMOXChanged(tx)` (`:30373`).
5. **`HdwMOXChanged(tx,freq)`** (`:30374`): **`SetPttOut(0)`** (`:29801`) →
   `SetTRXrelay(0)` (`:29802`) → restore RX DDS freqs.
6. `cmaster.Mox=false` clears XmitBit (`:30376`).
7. **`Thread.Sleep(ptt_out_delay)`** (20 ms) — HW T/R settle (`:30377`).
8. **Restart RX DSP** (non-blocking): `SetChannelState(id(0,0),1,0)` (+RX2/sub) (`:30379-30383`).

**Two invariants — both hold and are load-bearing:**
- **Wire MOX bit cleared only AFTER the TX I/Q down-ramp completes** (blocking
  stop at step 2, before clearing PTT/XmitBit). Clearing early = key-click/splatter.
- **RX DSP restarted only AFTER the hardware T/R settles** (`ptt_out_delay` after
  the relay drop). Restarting early = the receiver grinds the T/R-transition IQ.

> These two are exactly the ordering rules we must reproduce Lyra-native. A single
> authoritative TX-state owner, the blocking TX-stop on keyup, and the
> stop-RX-on-keydown / restart-RX-after-settle pattern.

---

## 5. PureSignal — build-requirements map

PS needs two synced I/Q streams during TX: **TX reference** (what we send) and
**RX feedback** (PA output via a directional coupler). On HL2 both come back as
**two sync-locked DDCs off the same ADC clock** into WDSP's `pscc()`.

### 5.1 Feedback path (HL2, P1)
- DDC routing (`console.cs:8469-8488`): MOX+PS → `DDCEnable=DDC0`,
  `SyncEnable=DDC1`, **`cntrl1=4`** (ADC-mux: point a DDC at the PA coupler),
  `P1_DDCConfig=6`. DDC0+DDC1 sync-paired.
- **HL2-specific rate**: feedback DDCs run at **`rx1_rate`**, NOT the 192 k `ps_rate`
  ANAN uses (`:8476-8485`). Most important sizing difference.
- One ADC on HL2 → the coupler feedback is multiplexed onto the same ADC via the
  **on-board PureSignal hardware mod** (the HL2≠ANAN hardware difference).
- Feedback reaches the consumer via `InboundBlock(id==1)` → `pscc(ch, n,
  data[ps_tx_idx], data[ps_rx_idx])` (`sync.c:45-78`).

### 5.2 Protocol bits
- `SetPureSignal(bit)` → `puresignal_run` (`netInterface.c:836-844`), sent
  high-priority. **`puresignal_run` OR'd into C2 bit 6** (`networkproto1.c:768`/`:832`).
- **Duplex bit mandatory** (`C4 |= 0x04`), `nddc` field correct. When PS+TX the
  feedback DDC is tuned to the **TX frequency** (`:656-657`/`:671-672`).

### 5.3 WDSP PS math modules
- **`lmath.c::xbuilder`** (`:411`) — constrained cubic-spline least-squares fitter
  (`decomp`/`dsolve` LU, `cull` outliers). **Port first** (foundational).
- **`calcc.c::calc()`** (`:324-483`) — coefficient solver: from time-aligned TX/RX
  envelope-binned pairs computes magnitude (`ym`), in-phase (`yc`), quadrature (`ys`)
  correction curves, fits each with `xbuilder` → `cm/cc/cs` coefficient arrays.
- **`iqc.c::xiqc()`** (`:122-203`) — applies correction inline in the TX I/Q:
  `PRE0=ym*(I*yc−Q*ys)`, `PRE1=ym*(I*ys+Q*yc)`. **5-state RUN/BEGIN/SWAP/END/DONE
  crossfade** (raised-cosine, double-buffered `cset`) so coeff swaps don't click.
  Entry: `SetTXAiqc{Start,Swap,End,Values}` (`iqc.c:241-299`).
- **`delay.c::xdelay()`** (`:71-99`) — fractional-sample delay to time-align
  TX-ref vs feedback (`SetPSTXDelay`, `calcc.c:993-1013`).

### 5.4 PS control state machines
- WDSP `pscc()` FSM (`calcc.c:525-537`, switch `:636-834`):
  `LRESET→LWAIT→LMOXDELAY→LSETUP→LCOLLECT→MOXCHECK→LCALC→LDELAY→{LSTAYON|…}`. A
  dedicated **calc thread** `doPSCalcCorrection` (`:485-507`) waits on `Sem_CalcCorr`,
  runs `calc()`, then `SetTXAiqcStart/Swap`. Plus turnoff/save/restore semaphore threads.
- C# command FSM `timer1code` (`PSForm.cs:555`): OFF / auto-cal / single-cal /
  stay-on / restore, driving `SetPSControl(reset,mancal,automode,turnon)`. ~10 ms loop.
- Auto-attenuator `timer2code` (`PSForm.cs:728`): `Monitor→SetNewValues→RestoreOperation`.
  `FeedbackLevel = 256*hw_scale/rx_scale`. Recalibrate `>181 || (≤128 && att>−28[HL2])`;
  step `20·log10(level/152.293)` clamped **±10 (HL2)** vs ±100 (ANAN); HL2 att may go
  negative to −28.

### 5.5 BUILD CHECKLIST (what Lyra-cpp must add for PS — v0.3)
- [ ] Port WDSP `lmath.c` (xbuilder) → `calcc.c` (pscc + calc + 4 worker threads) →
      `iqc.c` (xiqc + 5-state crossfade) → `delay.c`. Attribute NR0V.
- [ ] **Dedicated PS calc thread** per TX channel, blocked on a semaphore, released
      by the collection FSM at LCALC. Replace Win32 `CRITICAL_SECTION`/`Interlocked`/
      `Semaphore` with `std::mutex`/`std::atomic`/`std::counting_semaphore`.
- [ ] ~10 ms host control loop = command FSM (every tick) + auto-att FSM (every 10th).
- [ ] **TX I/Q reference tap** (the `sip1`/iqc point) + the `xiqc` corrector placed in
      the TX chain before the IQ→radio hand-off (stage 29 above).
- [ ] **DDC routing**: enable DDC0+DDC1 sync-locked, `cntrl1=4`, feedback DDC at
      **rx1_rate**, tuned to TX freq; `puresignal_run`→C2 bit6; duplex bit; nddc.
- [ ] **Coefficient persistence**: save/restore `cm/cc/cs` (`ints×4` doubles, `%.17e`).
- [ ] **Auto-attenuator** sharing the §2.1 step-attenuator actuator (one writer),
      HL2 thresholds/bounds.
- [ ] **Operator self-attestation**: HL2/HL2+ need the hardware feedback mod (one ADC);
      ANAN G2 does not. Gate PS enable behind an opt-in.
- [ ] Keep the **model→routing table** data-driven (`nddc`, `cntrl1`, feedback rate,
      auto-att bounds) so ANAN-P2 is a table entry, not a rewrite.

### 5.6 HL2-P1 vs ANAN-P2 divergence (don't paint into a corner)
| | HL2 (P1) | ANAN (P2) |
|---|---|---|
| Feedback rate | `rx1_rate` | `ps_rate` 192 k |
| ADC mux | `cntrl1=4`, 1 ADC, **needs mod** | `(…&0xf3)\|0x08`, 2nd ADC, no mod |
| DDC count / feedback | nddc=4, DDC0+DDC1 | nddc=5, RX5 |
| Auto-att bounds | neg to −28, ±10 clamp | floor 0, ±100/31.1 |
| Frame builder | legacy P1 rotation | fixed-offset P2 |

---

## 6. Proposed Lyra-cpp TX phasing (for discussion — not yet started)

Mirrors the proven small-step, bench-gated discipline. **SSB first; never break RX.**

- **TX-0 — wire plumbing (no RF):** EP2 TX I/Q packing (§1.1), the TX-state C&C
  registers (§1.2: TX freq, drive_level, step-att with `31-x`, PA-enable bit),
  EP6 TX telemetry decode (§1.3: fwd/rev power, PA current/volts, temp). MOX bit
  stays 0 — wire-identical to RX until the FSM lands. Bench: telemetry reads sane.
- **TX-1 — MOX/PTT FSM + sequencing (§4):** single-state owner, keydown/keyup
  ordered exactly (stop-RX-blocking on keydown, blocking-TX-stop + restart-RX-after-
  `ptt_out_delay` on keyup), TR delays operator-configurable, **ATT-on-TX** RX-ADC
  protection (§2.1), TX-safety timeout, hardware PTT-in (opt-in). Still no modulation.
- **TX-2 — WDSP TXA SSB modulator (§3):** `wdsp_tx_engine` with a TX channel
  (type=1, bfo=1), always-on panel(mic gain)→bp0→ALC, mode→sideband, mic input
  (HL2+ EP6 / PC soundcard), leveler. **First RF** into a dummy load (bench: ~5 W,
  PA current sane, clean single-tone TUN via gen1). Phase-exit kill-test (PA bias
  drops on host death).
- **TX-3 — polish:** drive %→`drive_level` calibration, PA-current/VDD meter, red-
  on-air visuals, TX meter row (ALC/PWR), band BPF/LPF select before antenna.
- **TX-4 — speech chain:** EQ, compressor/CFC, PHROT, de-esser etc. (operator's
  ESSB feature set, layered as native pre-processors + WDSP stages).
- **CW / AM / FM** modulators: own sub-phase (CW uses the §2.5 I-LSB keyer bits).
- **PureSignal (v0.3):** the §5 checklist — separate release line.

**Forward-compat:** keep TX freq/drive/attenuator/PA-enable behind a thin
hardware-capability layer and the PS routing data-driven, so ANAN/Brick (P2) slot
in as a second protocol encoder + capability table, not a rewrite.

---

*Sources: clean-room reads of Thetis 2.10.3.13 + WDSP (GPL, NR0V). Citations are
for our own provenance; no reference-app identifiers appear in shipped code.*
