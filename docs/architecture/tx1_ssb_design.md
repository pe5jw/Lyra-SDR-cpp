# TX-1 SSB modulator arc — design document

Status: **DESIGN v1, awaiting operator decisions + 2-agent red-team round**
Date: 2026-05-29
Scope: SSB-only (USB/LSB).  CW/AM/FM/digital-modulator are later slices.
Reference: **Thetis 2.10.3.13 only** — operator directive 2026-05-29.  Old
Python lyra is NOT a reference for TX-DSP / WDSP-TXA / mic-input; it
remains relevant ONLY for (a) TX panadapter visual rule (red passband
rectangle on TX-active) and (b) 2RX + SUB/SPLIT UI design ideas.

This document is the input to the 2-agent red-team round.  Both agents
read this + the dossiers below, then file:line-grounded critiques per
the locked methodology (CLAUDE.md §15.28 — smallest revertable step,
operator-visible metric before design, no convergence theatre).

---

## 1. Operator-visible acceptance metric (defined BEFORE design)

Per §15.28 lesson #4: a change without a falsifiable operator-visible
behaviour metric is furniture, not a step.

**Tier-A unit bench** (synthetic, automatable):
- Inject a 1 kHz mic tone at -10 dBFS into `TxChannel::process()` in
  USB mode with operator passband (+200..+3100 Hz)
- FFT the resulting EP2 TX I/Q output, assert:
  - Wanted sideband at the correct baseband sign, ≥-20 dBFS
  - Unwanted sideband ≤-60 dBFS below wanted
  - Carrier (DC) ≤-60 dBFS
  - Mirror-symmetric in LSB mode at -1 kHz baseband sign

**Hardware bench** (N8SDR HL2+, dummy load, NO external amp):
- Mic into HL2+ codec, USB mode, freq 28.495 MHz, drive 50%, PA enabled
- Speak continuously for 5 seconds
- A/B against Thetis-on-same-hardware: voice intelligible at same freq
- External SDR / RSPdx confirms ≥60 dB sideband + carrier suppression
- Palstar reads stable power consistent with voice envelope (not zero,
  not pegged)
- Zero clicks at keydown / keyup (analyzer trace clean)
- 20× rapid Stop / Start cycles with mic active: no hang, no dead RX,
  no stuck carrier
- Mode swap LSB ↔ USB mid-TX: clean transition, no transient

**Phase-3-EXIT kill-test** (bundled here; otherwise pending):
- `taskkill /F` Lyra mid-mic-TX
- PA-current banner drops to idle within HL2 gateware watchdog window
  (~13 s per `hl2_stream.cpp:518`)
- Re-launch: comes up RX (cb58bcb / come-up-not-keyed invariant holds)

**FAIL → revert.**  Operator-empirical outranks any agent verdict.

---

## 2. Lyra-cpp current TX state (load-bearing context)

From `src/hl2_stream.{h,cpp}` (verified by direct grep 2026-05-29):

- **TUN carrier today** = constant DC injection inline in EP2 packer at
  `hl2_stream.cpp:1622-1638` (`kTuneCarrierI ≈ 0.95 × 32767`, Q=0).
  NOT WDSP TXA.  Zero CPU cost.  Mode-independent (correct on a DC
  zero-beat at the dial freq).  **Keep this for TUN** — it ships, it
  works at 5 W into dummy, no §15.23-class risk.  TX-1 ONLY builds
  the WDSP TXA chain for SSB / digital voice modes.
- **EP2 writer pacing** = producer-paced with silence-keepalive
  timeout at `hl2_stream.cpp:1652-1685`.  Audio buffer drained at
  126-stereo-frame chunks.  TX I/Q produced inline per-sample in the
  same packing loop.
- **MOX bit** = snapshotted once per datagram at
  `hl2_stream.cpp:1574`, used for both C0 and TX I/Q gating
  (`hl2_stream.cpp:1614`).
- **No mic input** — bytes 24-25 of each EP6 26-byte slot are
  explicitly documented "ignored, future TX work" at
  `hl2_stream.cpp:727`.
- **No WDSP TXA bindings** in `src/wdsp_native.{h,cpp}`
  (`SetTXA*` symbols not loaded).
- **No TxChannel** in `src/wdsp_engine.{h,cpp}` (RxChannel only).
- **No MoxEdgeFade** — TUN flips on/off purely via `tuneEnabled_` AND
  `moxBit` snapshot per datagram.  No envelope shaping.
- **PTT FSM** lives inside `HL2Stream` (NOT a separate `ptt.cpp`);
  keydown/keyup hooks `fsmKeydownSettled` / similar at
  `hl2_stream.h:521+`.
- **TX-0c register surface** = drive level (`set_tx_drive_level`),
  PA enable (`set_pa_enabled`), tune carrier (`setTuneEnabled`),
  step attenuator with `31-x` HL2 encoding (`set_tx_step_attn_db`),
  TX safety timeout, ATT-on-TX policy — all shipped, hardware-
  validated 2026-05-28.  Frame composers
  (`_compose_frame_0/4/10/11/etc.`) are byte-correct for HL2+
  ak4951v4 gateware.
- **CMakeLists.txt**: no `wdsp_tx_engine.cpp` / `tx_dsp_worker.cpp` /
  `mic_source.cpp` / `mox_edge_fade.cpp` referenced.

---

## 3. Thetis reference dossier A — wire surface

(Grounded by Explore agent 2026-05-29; cited Thetis source paths under
`D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\`.)

### 3.1 EP2 TX I/Q packing
- `networkproto1.c:1194-1195` + `:1241-1259`: main packing loop inside
  `sendProtocol1Samples()`.
- EP2 frame = 1032 bytes total = 8-byte Metis header + 2 × 512-byte USB
  frames.  Per USB frame: 3-byte sync (`0x7f 0x7f 0x7f`) at offset 0–2,
  5-byte C0..C4 at offset 3-7, then 504 bytes = 63 LRIQ tuples × 8
  bytes/tuple.  Total 63 × 2 = 126 LRIQ tuples per EP2 datagram.
- Per-tuple offsets: L (2B BE), R (2B BE), TX-I (2B BE), TX-Q (2B BE).
  Same layout Lyra-cpp already implements at `hl2_stream.cpp:1628-1637`.
- **Sample scaling**: Thetis stores TX I/Q as `double` in
  `prn->outIQbufp`, scales by `× 32767.0` then casts to `int16` BE.
  Lyra-cpp's TUN injection uses the same convention
  (`kTuneCarrierI ≈ 0.95 × 32767`).
- **CW mode bit-pack override** at `networkproto1.c:1248-1252`: for HL2
  in CW mode, the Q-slot of the IQ stream is bit-packed with CWX state
  (cwx_ptt bit 3, dot bit 2, dash bit 1, cwx bit 0).  CW = later slice;
  noted for forward compat (TX-1 must NOT clobber this — but TX-1 only
  touches USB/LSB so no current risk).

### 3.2 HL2 mic → TX path
- `networkproto1.c:570-573` (`MetisReadThreadMainLoop_HL2`): mic
  extracted from EP6 slot bytes 24-25 per slot, scaled `mic / 2^31`
  (mic is 24-bit signed BE sign-extended to int32), Q hardcoded `0.0`.
- `networkproto1.c:579`: `Inbound(inid(1,0), mic_sample_count,
  prn->TxReadBufp)` hands a buffer of `(I=mic_scaled, Q=0)` complex
  pairs to ChannelMaster.
- `cmbuffs.c:89-121`: `Inbound()` copies into ring `r1_baseptr`,
  signals `Sem_BuffReady`.
- `cmaster.c:112-253`: ChannelMaster TX DSP thread pumps the ring →
  routes through TX DSP (dexp VOX, EER, gain, filtering) →
  `prn->outIQbufp` for EP2 packing.

### 3.3 MOX bit gating
- `networkproto1.c:615` + `:896`: `C0 = (unsigned char)XmitBit` —
  MOX written to C0 bit 0.  Lyra-cpp already does the same at
  `hl2_stream.cpp:1574` (per-datagram snapshot).
- `networkproto1.c:1227`: `if (!XmitBit) memset(prn->outIQbufp, 0,
  ...)` — software belt+suspenders: TX I/Q ZEROED when RX, even though
  gateware would also gate.  Worth mirroring in Lyra-cpp for defence
  in depth.

### 3.4 TX-NCO freq writes
- `networkproto1.c:974-980`: case 1 in `WriteMainLoop_HL2()`,
  "TX VFO 0x01".  Pattern: `C0 |= 2; C1..C4 = (tx[0].frequency >>
  24,16,8,0) & 0xff` — big-endian word split.
- HL2 gateware caches the last write; same-cycle persistence implicit.
  Thetis updates on dial change.
- Lyra-cpp already implements this via `_compose_frame_*` slot 2 of
  the 18-case round-robin.

### 3.5 Drive level + PA enable
- `networkproto1.c:1076-1089`: case 10, "Drive/Filter/PA 0x09" (C&C
  address 0x09 = Lyra-cpp frame 10).
- `C1 = tx[0].drive_level` (0-255).
- `C3 bit 7 = tx[0].pa`.  **Note**: on the operator's HL2+/ak4951v4
  gateware this is the LEGACY path and is ignored — the real PA
  enable is C2 bit 3 (Apollo Tuner pin, per the §15.26 RTL findings
  already implemented in Lyra-cpp task #29).  Lyra-cpp already sets
  C2 bit 3 correctly.  Do NOT regress this in TX-1.
- `C2 bit 7 = VNA` forced to 0 (`networkproto1.c:750`, `& 0x7f`) —
  Lyra-cpp already does this.

### 3.6 C&C round-robin scheduling
- HL2 has **18 cases** (cases 0-17 plus case 18 for reset on
  disconnect).  Hermes (non-HL2) wraps at 16.
- Each USB frame consumes one slot; ~760 USB frames/s; each register
  revisits at ~40 Hz.
- Lyra-cpp `hl2_stream.cpp:1568+` implements `ccIdx_ = (ccIdx_ + 1)
  % 19` — **CONFIRM this matches the Thetis 18-case wheel**.  (Agent
  said HL2 extends to cases 17 + 18.  Lyra modulus 19 = 0..18
  inclusive = 19 cases.  Audit gate for the red-team.)

### 3.7 SendHighPriority on MOX edge
- `console.cs:30073`: `NetworkIO.SendHighPriority(1)` called on MOX
  keydown — emits immediate C&C frame outside round-robin, reduces
  MOX-keydown latency.
- Lyra-cpp does NOT have this today.  Optional flag for TX-1: emit
  next-available frame immediately on MOX/PTT edge so the gateware
  sees the MOX bit + step-att + drive_level coherently at the
  earliest possible moment.

### 3.8 Sample rate
- HL2 TX I/Q is fixed 48 kHz (AK4951 codec hard-locked, per
  CLAUDE.md §3.5).  No resampling on the wire.
- 126 LRIQ tuples per datagram / 2.625 ms per datagram → 48 kHz exact.

---

## 4. Thetis reference dossier B — WDSP TXA DSP chain (SSB only)

(Grounded by Explore agent 2026-05-29.)

### 4.1 TXA channel scaffolding
- `wdsp/TXA.c` `create_txa()` builds the chain.  Block instantiation
  order:
  - rsmpin (line 40) — input resampler, run=0
  - gen0 (line 51) — input signal gen, run=0
  - **panel** (line 59) — `inselect=2` (I=mic), copy=0, run=1
  - **phrot** (line 71) — run=0, fc=338 Hz, nstages=8
  - micmeter (line 80) — run=1
  - amsq (line 95) — run=0
  - leveler (line 158) — run=0, τa=1ms, τd=500ms, max_gain=+5dB
  - cfcomp (line 202) — run=0
  - **bp0** (line 239) — **run=1 always-on**; the SSB sideband selector
  - compressor (line 253) — run=0
  - **bp1** (line 260) — **run=0**; default frozen at f_low=-5000,
    f_high=-100; compressor-only aux; **NEVER touch via
    SetTXABandpassRun** (§15.23 trap)
  - osctrl (line 274) — run=0
  - bp2 (line 282) — run=0
  - **alc** (line 311) — **run=1 always-on**; τa=1ms, τd=10ms,
    max_gain=1.0
  - gen1 (line 361) — run=0; output-side gen for TUN / two-tone (NOT
    used by Lyra-cpp's TUN — Lyra-cpp uses DC inline instead)
  - uslew (line 369) — upslew_time=5ms
  - rsmpout (line 451) — run=0
- Runtime exec order in `xtxa()` line 557-592: input → gen0 → panel →
  phrot → meters → leveler → CFC → **bp0** → compressor → bp1 → osctrl
  → bp2 → **alc** → AM/FM mods → gen1 → uslew → rsmpout → output.
- **SSB hot path**: mic → panel(I=mic) → phrot (run=0 default — operator
  opt-in) → bp0 (sign-aware) → alc → uslew → out.

### 4.2 SetTXAMode + SetTXABandpassFreqs
- `wdsp/TXA.c:753-789`: `SetTXAMode` sets mode + flags ammod/fmmod off
  for SSB, then calls `TXASetupBPFilters(channel)`.
- `wdsp/TXA.c:792-800`: `SetTXABandpassFreqs` sets f_low / f_high then
  calls `TXASetupBPFilters`.
- `wdsp/TXA.c:827-901`: `TXASetupBPFilters`: SSB modes (USB/LSB/CWL/
  CWU/DIGL/DIGU/SPEC/DRM) set `bp0.run=1`, `bp1.run=0`, `bp2.run=0`,
  call `CalcBandpassFilter(bp0, f_low, f_high, 2.0)`.  Only if
  compressor.run does bp1.run get enabled (with same edges).
- **The trap (§15.23)**: bp1 created with f_low=-5000, f_high=-100
  (line 268-269).  Directly calling `SetTXABandpassRun(ch, 1)` toggles
  bp1.run without recomputing its kernel → cascades stale negative-
  baseband-only filter after correct bp0 → kills USB.
- **Verified**: zero call sites to `SetTXABandpassRun` in Thetis tree.

### 4.3 Per-mode SSB sign convention
- `Console/console.cs:8079-8118` `UpdateTXLowHighFilterForMode`.
- Operator passes positive `low` / `high` (e.g., 200, 3100).  Engine
  transforms per mode:
  - **USB / CWU / DIGU**: l=+low, h=+high (positive baseband)
  - **LSB / CWL / DIGL**: l=-high, h=-low (negative baseband)
  - **DSB / AM / SAM / FM**: l=-high, h=+high (symmetric)
  - **DRM**: fixed l=+7000, h=+17000
- TX-1 implements USB and LSB only.  Other modes structurally compat
  for later slices.

### 4.4 ALC defaults (always-on, splatter protection)
- Created at `TXA.c:311-334`: tau_attack=1 ms, tau_decay=10 ms,
  max_gain=1.0 (0 dB), out_target=1.0, hang_enable=0, **run=1**.
- Setters (`wcpAGC.c:578-610`): `SetTXAALCAttack/Decay/Hang/MaxGain/St`.
- **No `SetTXAALCThresh` exists.**  MaxGain alone governs ALC ceiling.
- **No operator opt-out** in TX-1 design — ALC is mandatory splatter
  protection.  Operator can tune MaxGain via Settings (range proposal:
  -3 to +3 dB around the WDSP default 0 dB).

### 4.5 Leveler defaults (operator opt-in)
- Created at `TXA.c:158-181`: tau_attack=1 ms, tau_decay=500 ms,
  max_gain=1.778 (+5 dB), out_target=1.05, hang_enable=0, **run=0**.
- Setters (`wcpAGC.c:613-654`): `SetTXALevelerAttack/Decay/Hang/Top/St`.
- TX-1 ships leveler **wired but OFF by default**, operator opt-in
  toggle in Settings → TX.  Defer cooler-pre-EQ / compressor / CFC to
  later slices.

### 4.6 PHROT (Phase Rotator)
- `wdsp/iir.c:665-703`.  Setters use **UPPERCASE PHROT**:
  - `SetTXAPHROTRun(ch, run)` (line 665)
  - `SetTXAPHROTCorner(ch, corner)` (line 675) — NOT `SetTXAPHROTFreq`
  - `SetTXAPHROTNstages(ch, nstages)` (line 686)
- Defaults: fc=338 Hz, nstages=8, run=0.
- Purpose: PEP-to-PAR reduction (~3-4 dB) by flattening group delay in
  passband.
- TX-1 ships PHROT **ON by default** (Thetis-faithful) — quietly
  improves on-air PEP-to-PAR without operator visible knobs.  Future
  operator opt-out if anyone wants the WDSP-default off.

### 4.7 Panel input selection
- `wdsp/patchpanel.c:55-101` `xpanel`: with inselect=2,
  `I = in[2i+0] * (inselect>>1) = in[2i+0] * 1 = mic`;
  `Q = in[2i+1] * (inselect & 1) = 0`.
- Default `inselect=2` at create.  **Zero call sites to
  `SetTXAPanelSelect` in Thetis** — defaults are correct, do NOT call
  it (would set copy=3 for I-Q swap balance-test mode).

### 4.8 Rate setters
- `wdsp/channel.c:197` `SetInputSamplerate`, `:227`
  `SetOutputSamplerate`, `:211` `SetDSPSamplerate` — **shared RX/TX,
  generic channel-level**.
- **No `SetTXAInRate` / `SetTXAOutRate`** (these don't exist).

### 4.9 Channel state lifecycle
- `wdsp/channel.c:259-297` `SetChannelState(ch, state, dmode)`:
  - **state=1 (keydown)**: sets `slew.upflag`, `ch_upslew`, clears
    `exec_bypass`, triggers DSP thread.  dmode ignored.
  - **state=0, dmode=1 (keyup blocking)**: sets `slew.downflag`,
    `flushflag`, waits up to 100 ms for flush.  Forcibly clears on
    timeout.
- `uslew.c` envelope: cos²-shaped 5 ms ramp gated by ch_upslew on
  keydown.
- **OPEN QUESTION for the red-team**: agent dossier asserts uslew is
  "bypassed for SSB mic" path.  Need direct verification from
  `wdsp/uslew.c` whether ch_upslew applies universally or only when
  certain block flags are set.  If uslew DOES envelope-shape SSB
  keydown universally, Lyra-cpp's MoxEdgeFade only needs to shape the
  POST-TXA EP2 output (and only for keyup, since WDSP already does the
  cos² in).  If uslew is bypassed for SSB, MoxEdgeFade carries the
  full envelope responsibility.

---

## 5. Lyra-cpp TX-1 architecture (design v1)

### 5.1 Component list (6 components, all new for SSB voice)

| # | Component | New file | Lines (est.) |
|---|---|---|---|
| 1 | WDSP TXA cdefs + loader | extend `src/wdsp_native.{h,cpp}` | ~150 |
| 2 | TxChannel class | extend `src/wdsp_engine.{h,cpp}` | ~350 |
| 3 | Mic source abstraction + 3 concretes | new `src/mic_source.{h,cpp}` | ~400 |
| 4 | TX DSP worker (dedicated thread) | new `src/tx_dsp_worker.{h,cpp}` | ~250 |
| 5 | MoxEdgeFade (cos² 50 ms) | new `src/mox_edge_fade.{h,cpp}` | ~120 |
| 6 | EP2 packer extension | edit `src/hl2_stream.cpp:1622-1638` | ~50 |

Plus: Settings UI (mic source picker, mic gain, ALC max gain, bandpass
edges, PHROT toggle) ~120 lines in `settingsdialog.cpp`.

**Total estimate**: ~1450 lines.  Not small.  Hence the staged
methodology.

### 5.2 WDSP TXA cdefs (component 1)

Symbols to add to `wdsp_native.{h,cpp}` (mirror existing RxChannel
loader pattern):

```
OpenChannel(int channel, int in_size, int dsp_size, int input_samplerate,
            int dsp_rate, int output_samplerate, int type, int state,
            double tdelayup, double tslewup, double tdelaydown,
            double tslewdown, int block);
CloseChannel(int channel);
SetChannelState(int channel, int state, int dmode);
SetInputSamplerate(int channel, int in_rate);
SetOutputSamplerate(int channel, int out_rate);
SetDSPSamplerate(int channel, int dsp_rate);
fexchange0(int channel, double* in, double* out, int* error);

SetTXAMode(int channel, int mode);
SetTXABandpassFreqs(int channel, double low, double high);
SetTXAPHROTRun(int channel, int run);
SetTXAPHROTCorner(int channel, double corner);
SetTXAPHROTNstages(int channel, int nstages);
SetTXAALCAttack(int channel, int attack_ms);
SetTXAALCDecay(int channel, int decay_ms);
SetTXAALCHang(int channel, int hang_ms);
SetTXAALCMaxGain(int channel, double max_gain);
SetTXAALCSt(int channel, int run);
SetTXALevelerAttack(int channel, int attack_ms);
SetTXALevelerDecay(int channel, int decay_ms);
SetTXALevelerHang(int channel, int hang_ms);
SetTXALevelerTop(int channel, double max_gain);
SetTXALevelerSt(int channel, int run);
SetTXAPanelGain1(int channel, double gain);  // operator mic gain
GetTXAMeter(int channel, int meter_type);    // for meter readouts
```

**Explicitly NOT cdef'd** (traps): `SetTXABandpassRun` (§15.23),
`SetTXAPanelSelect` (no call sites + default correct),
`SetTXAALCThresh` (doesn't exist).  Comment-anchor "DELIBERATELY
OMITTED" in the cdef list with §15.23 ref so future audits don't
add them.

### 5.3 TxChannel class (component 2)

```cpp
class TxChannel {
public:
    enum Mode { USB, LSB };  // others later

    TxChannel(int channelId);
    ~TxChannel();

    void open(int micRate, int dspRate, int outRate);
    void close();
    void start();   // SetChannelState(ch, 1, 0)
    void stop();    // SetChannelState(ch, 0, 1) - blocking

    // Operator-positive Hz.  Engine signs internally per mode.
    void setMode(Mode);
    void setBandpass(double opLowHz, double opHighHz);

    void setMicGainDb(double db);     // SetTXAPanelGain1
    void setAlcMaxGainDb(double db);  // SetTXAALCMaxGain
    void setLevelerOn(bool, double topDb = 5.0);  // SetTXALevelerSt
    void setPhrotOn(bool);            // SetTXAPHROTRun

    // Hot path.  mic_block is `n` mono samples (mic in I slot,
    // Q=0 — engine routes via panel inselect=2 default).  Output
    // is `n` complex<float> TX I/Q.  Returns WDSP error code.
    int process(const float* mic_block, int n,
                std::complex<float>* iq_out);

private:
    int channel_;
    Mode mode_ = USB;
    double opLow_ = 200.0, opHigh_ = 3100.0;

    // Centralized sign convention per §15.23 lesson.
    void pushBandpassLocked();
    std::pair<double, double> signedEdges(Mode, double low, double high);
};
```

Init sequence (in `open()`), per Thetis-grounded dossier B:
1. `OpenChannel(ch, in_size=512, dsp_size=4096, in=48k, dsp=96k,
   out=48k, type=1, state=0, tdelayup=0.010, tslewup=0.025,
   tdelaydown=0.000, tslewdown=0.010, block=1)`
2. `SetTXAPanelGain1(ch, 1.0)` — override WDSP default 4.0 (which
   adds +12 dB hot mic)
3. `SetTXAMode(ch, USB)`
4. `pushBandpassLocked()` — calls `SetTXABandpassFreqs(ch,
   signedLow, signedHigh)` per mode.  Does NOT call
   `SetTXABandpassRun` (the trap).
5. `SetTXAPHROTRun(ch, 1)` + `SetTXAPHROTCorner(ch, 338.0)` +
   `SetTXAPHROTNstages(ch, 8)`
6. `SetTXAALCAttack(1)` + `SetTXAALCDecay(10)` + `SetTXAALCHang(500)`
   + `SetTXAALCMaxGain(1.0)` + `SetTXAALCSt(1)`
7. `SetTXALeveler*` defaults + `St(0)` (off by default)
8. `SetChannelState(ch, 1, 0)` to start

### 5.4 Mic source abstraction (component 3)

```cpp
class MicSource {
public:
    virtual ~MicSource() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    // Producer-paced consumer registration.  Source pushes mic
    // samples (mono float32, normalized [-1,+1)) into the consumer
    // at whatever rate the source produces (HL2+: 48 kHz; PC SC:
    // operator-configured; TCI: per protocol).
    using Consumer = std::function<void(const float* samples, int n)>;
    virtual void setConsumer(Consumer) = 0;
    virtual int sampleRate() const = 0;
    virtual QString label() const = 0;
};

class Hl2Ep6MicSource : public MicSource;       // EP6 bytes 24-25
class PcSoundcardMicSource : public MicSource;  // Qt6 QAudioSource
class TciAudioMicSource : public MicSource;     // TCI audio_stream
```

**Three concretes, operator-selectable** per Q1 answer:

- **Hl2Ep6MicSource** — primary for HL2+ operators.  Registers a
  consumer on `HL2Stream` (new API mirroring Thetis's
  `Inbound(inid(1,0), ...)` pattern) that taps EP6 bytes 24-25 from
  each slot, scales `/2^31` to float32 [-1,+1), pushes to consumer in
  38-sample chunks (one per EP6 datagram at nddc=4: 19 slots × 2 USB
  frames per datagram).  Per §15.26 G1: AK4951 gateware emits
  populated mic slot in every EP6 frame unconditionally.
- **PcSoundcardMicSource** — Qt6 QAudioSource on operator-selected
  device.  Operator picks device + nominal rate; downsample-to-48k
  if device rate differs.  Required for std-HL2 operators (no codec)
  and HL2+ operators who prefer an external USB mic.
- **TciAudioMicSource** — listens on the TCI server's audio stream
  channel.  When a TCI client (WSJT-X / FLDigi / etc.) sends audio
  via TCI, this source emits it as the mic input.  For digital modes.

**Open Q for design phase**: does mode selection (DIGU/DIGL) auto-
switch to TCI source, or is the source picker fully manual?
Recommendation: **auto-switch with operator-overridable preference**.
Default: in DIGU/DIGL with a TCI client connected + streaming, use
TCI source; else use operator-selected default.  Operator can pin a
specific source per-mode if they want.

### 5.5 TX DSP worker (component 4)

Dedicated thread (NOT folded into the RX DSP worker, NOT inlined into
the EP2 writer thread).  Rationale per §15.28 lesson and §15.26 R2:

- Wire cadence (48 kHz, 126 samples per 2.6 ms datagram) does NOT
  match RX cadence (192 kHz block boundaries).  Folding causes
  cadence mismatch headaches.
- Inlining into EP2 writer thread loads a sensitive critical path
  (per §15.26 D-1/D-2/D-3 analysis on Python lyra).

The TX DSP worker:
- Owns the `TxChannel` instance.
- Receives mic samples from whichever `MicSource` is active (via
  consumer callback).  Pushes them into a lock-free SPSC ring.
- **Self-clocks at wire cadence** per §15.26 R2 + Thetis pattern
  (mandatory — HL2+ AK4951 mic content is codec audio that may be
  silence; TXA chain must produce I/Q regardless).  Specifically: at
  each EP2 frame boundary (signalled by a semaphore from the EP2
  writer), the worker checks if its mic ring has ≥126 samples; if
  yes, pop 126, call `TxChannel.process(126)`, push the 126 complex
  I/Q samples to the EP2 writer's TX I/Q ring.  If no, pop whatever
  is available + zero-pad to 126 (silence — TXA chain still runs).
- On the MOX-off→on edge, the worker applies the MoxEdgeFade
  envelope to the I/Q output (fade-in over 50 ms).
- On the MOX-on→off edge, the worker applies fade-out, signals
  `MoxEdgeFade.is_off()` true when done.  FSM keyup hook waits on
  this before clearing the wire MOX bit.

### 5.6 MoxEdgeFade (component 5)

50 ms cos² envelope, applied to TX I/Q at the EP2-writer side just
before packing (NOT inside TxChannel — keeps WDSP TXA chain pure
mic→IQ).  Shared between TUN and SSB paths.

**The open question** from §4.9 affects this: if WDSP's own uslew
applies to SSB keydown (5 ms cos² built in), MoxEdgeFade may only
need keyup responsibility.  Will resolve in the red-team round.

Worst case: both apply — that's fine, just slightly more aggressive
shaping at keydown.  The MoxEdgeFade 50 ms cos² subsumes the WDSP
5 ms cos² envelope (the longer envelope dominates).

### 5.7 EP2 packer extension (component 6)

Edit `hl2_stream.cpp:1622-1638`.  Today:

```cpp
const qint16 txI = emitTone ? kTuneCarrierI : qint16{0};
const qint16 txQ = 0;
```

Becomes (sketch):

```cpp
if (emitTone) {
    txI = kTuneCarrierI;  // TUN keeps the inline DC — unchanged
    txQ = 0;
} else if (txIq_have_block) {
    txI = txIqBlock[i].real * 32767;
    txQ = txIqBlock[i].imag * 32767;
} else {
    txI = 0;  // No block ready (TX DSP worker hasn't produced) — silence
    txQ = 0;
}
```

`txIqBlock` is drained from a lock-free SPSC ring filled by the TX
DSP worker.  TUN takes priority over SSB (TUN's whole point is "make
RF without modulation" — if both are armed somehow, TUN wins).

### 5.8 PTT/MOX FSM extension (HW-PTT, task #42)

Per task #42, fold the foot-switch HW-PTT forwarder into TX-1's FSM
work.  EP6 control bytes already decoded for telemetry; add a
`ptt_in` edge detector + parallel PttSource entry alongside the UI
MOX button.  **MANDATORY opt-in default-OFF gate** (per §15.26 RESOLVED-
CORRECTION lesson — N8SDR's HL2+ ptt_in reads NON-ZERO at RX rest):
Settings checkbox "Use HL2 hardware PTT input" (default OFF).  Edge-
detect not level-driven.  10 ms debounce in the forwarder.  Bench-
verify on operator's specific unit before any production wiring.

---

## 6. Open questions for operator before red-team round

| # | Q | Recommendation |
|---|---|---|
| **6.1** | Mic auto-switch on DIGU/DIGL to TCI source, or manual picker only? | **Auto-switch w/ operator override per-mode preference.**  Default: DIGU/DIGL + TCI client active → TCI source; else default-picker source. |
| **6.2** | Bandpass edges operator-tunable from day 1? | **Yes, range 50–8000 Hz, default +200..+3100, persisted per-mode.**  Operator's ESSB ambitions noted in CLAUDE.md §15.19. |
| **6.3** | Leveler ship in TX-1 or defer to v0.2.1? | **Ship wired but OFF by default.**  Operator opt-in toggle.  Marginal code cost, no risk if default OFF. |
| **6.4** | PHROT on by default (Thetis-faithful) or off? | **ON by default.**  Quiet 3-4 dB PEP-PAR win, no operator-visible side effect.  Mention in tooltip; no operator-visible knob in TX-1. |
| **6.5** | AK4951 codec I²C mic-route check on the operator's HL2+: required new EP2 surface or already routed? | **Bench-verify FIRST.**  Run current lyra-cpp with a mic plugged into HL2+, speak.  Capture EP6 bytes 24-25 at idle vs speech via the HL2 telemetry probe or Wireshark.  If samples track voice → codec is already routing mic→ADC (likely from prior Thetis configuration persisting).  If silence → I²C side-channel write needed (§3.9-class new EP2 surface; default-OFF gate + dossier). |
| **6.6** | HW-PTT foot switch fold into TX-1 (task #42)? | **Yes**, share the FSM design pass.  Default-OFF opt-in.  Debounce + edge-detect.  Bench-verify operator's HL2+ ptt_in rest-state first. |

---

## 7. Red-team round — preparation

Per §15.28 methodology, 2 independent senior agents review this
design + dossiers.  Specifically need:

**Agent A (concurrency / RT / lyra-cpp-thread-model lens):**
- Does the dedicated TX DSP worker model cleanly compose with the
  existing EP2 writer + RX loop?  Any cadence-mismatch hazards?
- Lock-free SPSC ring between TX worker and EP2 writer: producer/
  consumer pattern, ring sizing, underrun handling
- MoxEdgeFade.is_off() polling vs signal: what's the cheapest
  correct synchronization?
- Stop/restart safety: does the TX worker tear down cleanly without
  the §15.21-class bugs (socket-close-before-thread-join, etc.)?
- Q1: will this cause TX/RX hang/stutter under load (browser, logger
  open)?

**Agent B (safety / §15.23-trap / Thetis-faithfulness lens):**
- Audit every WDSP TXA setter call site against the §15.23 trap and
  the Thetis reference dossier file:line.
- Verify the SetTXABandpassRun trap is structurally prevented (no
  cdef for the symbol).
- Verify sign convention transform per mode.
- AK4951 I²C mic-route question (6.5) — bench-verify-first
  discipline, default-OFF gate if needed (§3.9).
- HW-PTT (6.6) — phantom-TX surge prevention.  Default-OFF opt-in
  + bench-verify before production.
- Q1: anything worse than HEAD (which is "no SSB voice at all today,
  only DC TUN")?  No-worse-than-HEAD == always OK (we're adding,
  not regressing).  But: does the TXA chain init misorder risk a
  click/splatter at first keydown?  Does the EP2 packer change risk
  regressing the TUN path (operator-validated 5 W into dummy)?
- Reference posture vs Thetis / pihpsdr / Quisk / linHPSDR: are we
  matching, ahead of, or behind the known-good?

Both agents file:line everything; both answer the charter §6
questions explicitly (will this hang TX/RX under full v0.2.x feature
load; how do the references handle this exact problem).

Convergence target: 2-round loop max (round 1 + round 2 if either
agent says LOOP).  If round 2 still LOOP → operator decides whether
to keep iterating or simplify the design.

---

## 8. Status

**DESIGN v1 LOCKED, awaiting operator answers to §6 Qs.**

Next steps:
1. Operator answers 6.1 - 6.6
2. (If 6.5 = bench-verify-required) operator runs the EP6 mic-slot
   capture
3. Spin 2 red-team agents (preferably in a fresh session for agent
   budget)
4. Reconcile
5. Operator decision
6. Implement
7. Tier-A unit bench
8. Hardware bench (§1)
9. Phase-3-EXIT kill-test (§1)
