# TX-1 SSB modulator arc — design document

Status: **DESIGN v2 LOCKED — 3-lens red-team converged + operator decisions in.
Ready for implementing-session.**
Date: 2026-05-29
Scope: SSB-only (USB/LSB).  CW/AM/FM/digital-modulator are later slices.
Reference: **Thetis 2.10.3.13 only** — operator directive 2026-05-29.  Old
Python lyra is NOT a reference for TX-DSP / WDSP-TXA / mic-input; it
remains relevant ONLY for (a) TX panadapter visual rule (red passband
rectangle on TX-active) and (b) 2RX + SUB/SPLIT UI design ideas.

**v2 amendment trail (this revision, 2026-05-29 EOD):**
- Three independent red-team lenses (concurrency / safety / scope) ran
  with file:line evidence against Thetis source.
- §4.9 uslew open question DEFINITIVELY ANSWERED via wdsp/uslew.c +
  wdsp/TXA.c read (BLOCKS-SHIP correction folded into §4.9 + §5.6).
- C&C modulus-19 audit confirmed against networkproto1.c:948-1176.
- §3.2 mic byte extraction CORRECTED — the original "bytes 24-25"
  layout is wrong; mic is interleaved with IQ per nddc-aware formula.
- §5.4 MicSource collapsed to a single class (Thetis-faithful).
- §6 open questions all answered + locked.
- Concurrency amendments applied to §5.5/§5.7 (mutex+cv not SPSC;
  producer-paced; teardown order; block-size pinned).
- Safety amendments applied (init order, TR delays, EP2 zero-on-no-MOX,
  panel-gain comment fix, DC/IQ-cal status note).
- v0.3 PureSignal sip1 tap added (§5.8).

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

**REWRITTEN 2026-05-29** — earlier "bytes 24-25" framing was wrong;
mic samples are interleaved with IQ per an nddc-aware formula.
Verified against `networkproto1.c:562-577` (`MetisReadThreadMainLoop
_HL2`).

- **Per-sample offset formula:**
  `k = 8 + nddc*6 + isamp * (2 + nddc*6)`
- **Per-frame mic-sample count:** `spr = 504 / (6*nddc + 2)`
- For HL2's typical nddc=1: stride=8 bytes, mic samples at offsets
  **14, 22, 30, …, 510** — 63 mic samples per 512-byte USB frame.
- For nddc=2: stride=14, 36 samples per frame.
- For nddc=4 (HL2 Lyra default per CLAUDE.md §3.1): stride=26, 19
  mic samples per frame.
- **Mic format:** 16-bit big-endian signed PCM
  (`bptr[k]<<8 | bptr[k+1]`); sign-extended via `<<24 | <<16` then
  divided by 2³¹ to normalize to [-1, +1).  L-channel only, R forced
  to 0.0.
- `networkproto1.c:562-577` extract sequence + scaling.
- `networkproto1.c:579`: `Inbound(inid(1,0), mic_sample_count,
  prn->TxReadBufp)` hands the (I=mic_scaled, Q=0) complex pairs to
  ChannelMaster.
- `cmbuffs.c:89-121`: `Inbound()` copies into ring `r1_baseptr`,
  signals `Sem_BuffReady`.
- `cmaster.c:112-253`: ChannelMaster TX DSP thread pumps the ring →
  routes through TX DSP → `prn->outIQbufp` for EP2 packing.

**Operator bench result (N8SDR HL2+, 2026-05-29):** reading bytes 24-25
of each "slot" produced SILENCE on his AK4951 even with a mic plugged
in.  That's because the slot framing is nddc-dependent — bytes 24-25
for nddc=1 land mid-IQ-sample (not a mic word at all).  Implementing
session MUST use the formula above + bench-gate the parser before
wiring TX (per §1).

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
- HL2 has **19 cases (0..18 inclusive)** per `WriteMainLoop_HL2`
  (`networkproto1.c:948-1176`).  Wheel advances via
  `if (out_control_idx < 18) out_control_idx++; else out_control_idx
   = 0;` at `networkproto1.c:1180-1183`.
- Each USB frame consumes one slot; ~760 USB frames/s; each register
  revisits at ~40 Hz.
- Lyra-cpp `hl2_stream.cpp:1585` `ccIdx_ = (ccIdx_ + 1) % 19` is
  **byte-exact** vs the Thetis HL2 wheel.  CONFIRMED 2026-05-29
  red-team audit.

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
  keydown — **gated to TUN / tone-gen paths ONLY, NOT SSB mic.**

**RESOLVED (red-team verification, 2026-05-29):** `wdsp/uslew.c` +
`wdsp/TXA.c` read end-to-end.  uslew **does NOT envelope-shape the
SSB mic stream**.  Proof:
- `wdsp/slew.c:90-92` (xuslew, first line):
  `if (!a->runmode && TXAUslewCheck(a->channel)) a->runmode = 1;` —
  uslew only arms when `TXAUslewCheck` returns true.
- `wdsp/TXA.c:819-825` (`TXAUslewCheck`): returns true **only if**
  `ammod.run || fmmod.run || gen0.run || gen1.run`.  SSB (USB/LSB)
  sets NONE of those (`SetTXAMode` switch at `TXA.c:762-785` leaves
  ammod/fmmod off for SSB modes; gen0/gen1 default off and are only
  enabled for input-signal-gen / TUN / two-tone).
- `wdsp/slew.c:153-154` (fall-through, !runmode): `memcpy(out, in,
  size * sizeof(complex))` — mic passes through untouched.

**Consequence for MoxEdgeFade (component 5):** MoxEdgeFade carries
**the FULL keydown AND keyup envelope responsibility for SSB voice TX**.
Fade-IN is **load-bearing**, not optional — without it every keydown
is a hard step at the EP2 packer = audible key click + adjacent-channel
splatter.  The "worst case both apply, longer wins" framing in earlier
drafts was wrong: WDSP's 5 ms cos² envelope does not run at all on
SSB, so MoxEdgeFade is the sole envelope.

---

## 5. Lyra-cpp TX-1 architecture (design v1)

### 5.1 Component list (6 components, all new for SSB voice)

| # | Component | New file | Lines (est.) |
|---|---|---|---|
| 1 | WDSP TXA cdefs + loader | extend `src/wdsp_native.{h,cpp}` | ~150 |
| 2 | TxChannel class | extend `src/wdsp_engine.{h,cpp}` | ~350 |
| 3 | Hl2Ep6MicSource (single class, no abstraction) | new `src/mic_source.{h,cpp}` | ~150 |
| 4 | TX DSP worker (dedicated thread) | new `src/tx_dsp_worker.{h,cpp}` | ~250 |
| 5 | MoxEdgeFade (cos² 50 ms) | new `src/mox_edge_fade.{h,cpp}` | ~120 |
| 6 | EP2 packer extension + sip1 tap | edit `src/hl2_stream.cpp:1622-1638` | ~70 |

Plus: Settings UI (TX panel: PHROT toggle, Leveler toggle, bandpass
sliders, mic boost / bias / mic-vs-line / line-in gain, HW-PTT toggle)
~150 lines in `settingsdialog.cpp`.

**Total estimate**: ~1240 lines (down from v1's ~1450 — MicSource
abstraction dropped per the §5.4 simplification).

**Per-component bench gates (§15.28 methodology, MANDATORY before
end-to-end Tier-A):**
1. WDSP loader: cdef dlsym audit — assert all symbols resolve from
   bundled libwdsp before any test runs.
2. TxChannel: `open()` setter-return-code audit; dump post-init bp0
   passband edges to log.
3. Hl2Ep6MicSource: loopback test piping a synthetic 1 kHz mic block
   through the consumer pattern with EP6 telemetry probe confirming
   bytes parse correctly via the §3.2 nddc-aware formula.  **This is
   also the Q6.5 mic-route bench** — verify AK4951 voice comes through
   with `line_in=0` + `mic_bias` as appropriate.
4. TX DSP worker: queue throughput at 48 kHz cadence with no
   consumer (just measures producer→worker→consumer plumbing).
5. MoxEdgeFade: cos² envelope unit test (fade-in monotonic, fade-out
   reaches exactly zero, abort-continuity on rapid keydown).
6. EP2 packer: TUN-vs-SSB-vs-silence dispatch unit test.

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

Init sequence (in `open()`), per Thetis-grounded dossier B + red-team
amendments 2026-05-29:
1. `OpenChannel(ch, in_size=126, dsp_size=2048, in=48k, dsp=96k,
   out=48k, type=1, state=0, tdelayup=0.010, tslewup=0.025,
   tdelaydown=0.000, tslewdown=0.010, block=1)`
   (in_size pinned to the per-datagram mic-sample count at nddc=4 —
   see §5.5; was `in_size=512` in v1, which would have starved
   fexchange0.)
2. `SetTXAPanelGain1(ch, 1.0)` — pin explicitly.  (WDSP `create_panel`
   default is gain1=1.0 per `wdsp/TXA.c:65-69`; the v1 doc claim of
   "default 4.0 (+12 dB hot mic)" was wrong — corrected here.  We
   pin it to centralize operator mic-gain control.)
3. **`SetTXABandpassFreqs(ch, signedLow, signedHigh)` FIRST** — per
   mode (operator-positive Hz, engine signs per §4.3).  This avoids a
   ~2.6 ms window where bp0 runs on stale `(-5000, -100)` defaults
   because `SetTXAMode` step 4 calls `TXASetupBPFilters` with whatever
   `f_low/f_high` it currently has (`TXA.c:786`).
4. `SetTXAMode(ch, USB)` (or LSB) — this re-calls `TXASetupBPFilters`
   with the bandpass edges already pushed in step 3.
5. `SetTXAPHROTRun(ch, 1)` + `SetTXAPHROTCorner(ch, 338.0)` +
   `SetTXAPHROTNstages(ch, 8)` (PHROT default ON per §6.4; operator
   Settings toggle exposed per §6.4)
6. `SetTXAALCAttack(1)` + `SetTXAALCDecay(10)` + `SetTXAALCHang(500)`
   + `SetTXAALCMaxGain(1.0)` + `SetTXAALCSt(1)` (always on; splatter
   protection, no operator opt-out)
7. `SetTXALeveler*` defaults + `St(0)` initially.  Operator Settings
   toggle (default OFF) per §6.3 flips `SetTXALevelerSt` live.
8. `SetChannelState(ch, 1, 0)` to start

Alternative if ordering of 3+4 is awkward in code: hold
`SetChannelState(ch, 1, 0)` until BOTH steps 3 and 4 have been pushed,
then start the channel.  Either approach is acceptable — what's NOT
acceptable is `SetTXAMode` first with no bandpass edges pushed.

### 5.4 Mic source (component 3) — single class, no abstraction

**REWRITTEN 2026-05-29** — collapsed from the v1 3-concrete
abstraction to a single class, per Thetis-faithful pattern verified
2026-05-29:

> Thetis HAS NO PC-soundcard mic input.  Operator mic is **always**
> the radio's onboard codec via EP6 at fixed 48 kHz
> (`networkproto1.c:562-577`).  The "PC audio for TX" case is
> **only** VAC1: PortAudio opens at user-selected `vac_rate` and
> WDSP `rmatchV` resamples to/from 48 kHz (`ChannelMaster/ivac.c:38-44,
> 343-352`).  Operator selects mic vs line input + boost via standard
> C&C bytes (cases 10 & 11), not via separate "mic source" objects.

Lyra-cpp therefore ships **one mic source class**:

```cpp
class Hl2Ep6MicSource {
public:
    // Consumer registers with HL2Stream's EP6 tap. The producer
    // (HL2Stream RX loop) extracts mic samples per the §3.2
    // nddc-aware formula and pushes to the consumer.
    using Consumer = std::function<void(const float* samples, int n)>;

    explicit Hl2Ep6MicSource(HL2Stream& stream);
    ~Hl2Ep6MicSource();

    void setConsumer(Consumer);
    int sampleRate() const { return 48000; }
};
```

No abstraction layer.  Only ONE concrete implementation justifies one
class (per locked rule "three similar lines beats premature
abstraction" — one concrete doesn't justify an ABC).

**Future PC-audio-for-TX (NOT in TX-1):** when wanted (e.g., bridge
WSJT-X via Virtual Audio Cable), ship a single VAC1-style feature
following the Thetis pattern: operator-toggled loopback ring buffer
between a PC audio device and the TX chain, with a real
rational-ratio resampler (WDSP RMATCH equivalent) between the device
rate and the radio's fixed 48 kHz TX input.  This is a future
milestone (likely v0.2.3 or v0.3 along with digital-mode workflows),
NOT another mic source class.  Q6.1 (auto-switch on DIGU/DIGL) is
deleted from TX-1 scope — Thetis doesn't auto-switch and we follow
suit.

**EP2 C&C cases 10 & 11 (mic routing — already in the round-robin
wheel):** Settings → TX panel exposes operator controls that compose
the bytes per `networkproto1.c:1076-1103`:

- Case 10 (C&C addr 0x09) C2 carries `mic_boost | (line_in<<1) | …`
- Case 11 (C&C addr 0x0a) C1 carries `mic_trs | mic_bias | mic_ptt`
  bits; C2 carries `line_in_gain`.

Settings exposes `mic_boost` (toggle), `line_in` (mic vs line radio
button, default mic = 0), `mic_bias` (electret toggle, default off),
`line_in_gain` (slider 0..31).  HL2 gateware reads these and routes
the AK4951 input accordingly — no AK4951 I²C side-channel needed
(Thetis-verified 2026-05-29: zero I²C transactions to the codec
anywhere in the Thetis tree; HL2 gateware initializes the AK4951
autonomously at bitstream init).

### 5.5 TX DSP worker (component 4) — AMENDED for concurrency

Dedicated thread (NOT folded into the RX DSP worker, NOT inlined into
the EP2 writer thread).  Rationale: wire cadence (48 kHz) and RX
cadence (192 kHz blocks) don't match; inlining loads a sensitive
critical path.

The TX DSP worker:
- Owns the `TxChannel` instance.
- Receives mic samples from `Hl2Ep6MicSource` via consumer callback
  on the HL2Stream RX thread.  Pushes them into a `std::deque<float>`
  protected by a `std::mutex` + `std::condition_variable` (the
  **same** primitive class the existing EP2 writer audio queue uses
  at `hl2_stream.cpp:1654-1687` — do NOT introduce a second
  sync-primitive type on the hot path).
- **Producer-paced clock (Lyra-native, matches the RX1 pattern at
  `hl2_stream.cpp:926-928`):** the worker wakes from the
  condition_variable when mic samples arrive (rather than from a
  separate EP2-writer semaphore).  This eliminates a round-trip,
  matches the existing RX worker model, and works for HL2+ AK4951
  where mic content is codec audio.
- **TXA channel block size pinned:** `OpenChannel` with `in_size=126`
  (the actual mic-sample count per EP2 datagram at nddc=4), `dsp_size
  =2048`.  The earlier `in_size=512` proposal would have starved
  fexchange0 (same gotcha class as the RX cleanup arc — pin the size
  to the producer, do not leave to integration).
- On the MOX-off→on edge, the worker applies the MoxEdgeFade
  envelope to the I/Q output (fade-IN over 50 ms — MANDATORY per
  §4.9/§5.6).
- On the MOX-on→off edge, the worker applies fade-OUT, then signals
  completion via a `std::condition_variable` (NOT a Qt-main-thread
  poll on `MoxEdgeFade.is_off()`).  FSM keyup hook waits on the
  cv before clearing the wire MOX bit.  Per CLAUDE.md §15.25 ground
  truth, the wire MOX bit must clear ONLY after the TX I/Q down-ramp
  fully completes (else key click + splatter on every keyup).
- **TUN priority interlock:** when TUN is armed, the worker skips
  `TxChannel.process()` entirely (saves CPU; the EP2 packer §5.7
  injects the TUN DC inline regardless).  No garbage I/Q queued.

**Teardown order (pinned, mirrors §15.21 discipline):**
1. `Hl2Ep6MicSource::stop()` (stop the mic-callback producer)
2. `TxDspWorker::request_stop() + join` (drain the consumer)
3. `TxChannel::stop()` — blocking `SetChannelState(ch, 0, 1)` per
   `wdsp/channel.c:259-297`
4. `TxChannel::close()` / `CloseChannel(ch)`

Skipping this order risks half-drained EP2 buffers + dangling WDSP
callbacks (the same bug class §15.21 fought through on the Python
side).  Documented here so the implementing session pins it on day
one.

**TR sequencing (Thetis-faithful defaults, per `console.cs:30350-30384,
19772, 19807`):**

| Delay | Default | Purpose |
|-------|---------|---------|
| `mox_delay`     | **10 ms** | After TX-channel-off (blocking flush) before clearing the wire MOX bit — lets in-flight TX I/Q drain past the gateware |
| `ptt_out_delay` | **20 ms** | After clearing MOX bit before restarting RX — hardware T/R settle |
| `rf_delay`      | **50 ms** | After MOX bit set, before enabling RF/TX-DSP-on — amp hot-switch protection (operator-tunable for non-amp use; Lyra v2 makes this configurable 1..75 ms with a default of 50) |

These defaults are non-zero by design.  The FSM keyup path
implements: `MoxEdgeFade.fade_out` → wait on cv for is_off →
`SetChannelState(ch, 0, 1)` (blocking) → `mox_delay` (10 ms) →
clear wire MOX bit → `ptt_out_delay` (20 ms) → restart RX DSP.
Keydown: set wire MOX bit → `rf_delay` → start TX DSP / un-mute path.
All delays implemented via Qt single-shot timers — NEVER
`std::this_thread::sleep_for` on the Qt main thread.

### 5.6 MoxEdgeFade (component 5) — LOAD-BEARING

50 ms cos² envelope, applied to TX I/Q at the EP2-writer side just
before packing (NOT inside TxChannel — keeps WDSP TXA chain pure
mic→IQ).  Shared between TUN and SSB paths.

**Per the §4.9 resolution:** WDSP uslew DOES NOT envelope-shape the
SSB mic stream (verified against wdsp/uslew.c + wdsp/TXA.c).
MoxEdgeFade is therefore the **SOLE** envelope for SSB voice TX, and
the fade-in is **MANDATORY**, not optional.  Skipping it ships a hard
step at the EP2 packer on every keydown = audible key click +
adjacent-channel splatter.  The §1 bench acceptance metric "Zero
clicks at keydown/keyup" depends on this fade-in being wired.

Both keydown (fade-in) and keyup (fade-out) responsibilities lie here.
TUN path also uses the same envelope (TUN's inline DC injection is
multiplied by the same cos² envelope coefficient).

### 5.7 EP2 packer extension (component 6) — AMENDED

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
} else if (moxBit) {
    txI = 0;  // No block ready while keyed — silence
    txQ = 0;
} else {
    txI = 0;  // MANDATORY zero-on-no-MOX (mirrors networkproto1.c:1227)
    txQ = 0;
}
```

`txIqBlock` is drained from the **same mutex+cv-protected queue**
used for audio (or a parallel queue with the same primitive — do NOT
introduce a different sync-primitive class on the hot path).  TUN
takes priority over SSB.

**EP2 zero-on-no-MOX is MANDATORY**, not "defence in depth".  Per
`networkproto1.c:1227`: `if (!XmitBit) memset(prn->outIQbufp, 0, …)`.
On HL2 community gateware this is benign (PA bias drops on MOX=0),
but on ANAN-class hardware (v0.4 scope) emitting TX I/Q with no MOX
could glitch an external linear.  Mandatory now to avoid a v0.4
regression hunt.

**Keyup ordering invariant (per §5.5 + CLAUDE.md §15.25):**
1. MoxEdgeFade fade-out reaches zero
2. Clear `inject_tx_iq` (the flag that gates this packer branch)
3. Clear the wire MOX bit

Implementing session: pin this order in the FSM keyup callback.

### 5.8 sip1 TX I/Q tap (v0.3 PureSignal forward-compat)

Per CLAUDE.md §7 (v0.2.0 line item: "§8.2 sip1 TX I/Q tap mandatory
in v0.2"): every TX I/Q sample written to the EP2 packer is also
written to a ring buffer (the `sip1` tap).  In v0.2.0 the ring has
no consumer — it's allocated and filled, that's it.  In v0.3
PureSignal's `calcc` thread reads from it for adaptive predistortion
calibration.

Wiring it now (~20 LOC) lets v0.3 land without re-validating every TX
sub-mode (USB/LSB/CW/AM/FM/digital).  Skipping it forces v0.3 to
introduce the tap retroactively across modes — explicit CLAUDE.md
§6.7 violation ("don't paint into a corner").

Ring size: 1 second @ 48 kHz @ 8 bytes/sample = 384 KB.  Lock-free
SPSC or mutex+cv — pick the same primitive as the audio queue (do
not proliferate primitives).

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

## 6. Operator decisions (LOCKED 2026-05-29)

| # | Q | LOCKED |
|---|---|--------|
| **6.1** | Mic auto-switch on DIGU/DIGL → TCI? | **NONE — DELETED from TX-1 scope.**  Thetis-faithful: mic is always-on, mode change never auto-switches audio routing (`console.cs:35370-35398` SetRX1Mode → `SetDigiMode(1, dmssTurnOffSettings)` touches DEXP/TXEQ/Leveler/etc.; zero VAC/routing touches).  Future PC-audio-for-TX is a VAC1-style feature, not a mic source — operator-toggled, mode-independent. |
| **6.2** | Bandpass edges operator-tunable? | **YES, default 0–10000 Hz** (operator-curated, supports ESSB).  Settings → TX exposes low/high sliders.  Tooltip notes "set ≥50 Hz to suppress 50/60 Hz mains coupling" but default stays 0 (operator's call).  Persisted per-mode. |
| **6.3** | Leveler ship in TX-1? | **YES, wired with a Settings UI on/off toggle, default OFF.**  Plumb the cdefs + setter; UI toggle in Settings → TX panel.  Testers can flip it; the toggle keeps it from being inert UI. |
| **6.4** | PHROT default ON? | **YES, default ON, with a Settings UI on/off toggle.**  Thetis-faithful adoption (~3-4 dB PEP/PAR win flattens speech peaks); operator can disable via Settings → TX. |
| **6.5** | AK4951 I²C mic-route side-channel needed? | **NO.**  Thetis does ZERO AK4951-specific I²C — the HL2 gateware initializes the codec autonomously at FPGA bitstream init.  Operator confirms his HL2+ has worked with Thetis + AK4951 mic for years.  Mic routing is via standard EP2 C&C cases 10 & 11 (`mic_boost`, `line_in`, `mic_bias`, `mic_ptt`, `line_in_gain`).  The earlier "no audio on bytes 24-25" bench result was a Lyra-cpp parser bug — bytes 24-25 are not the mic-sample offset at any nddc value (it's the §3.2 nddc-aware formula).  Implementing session fixes the parser + verifies the C&C bytes per §5.4. |
| **6.6** | HW-PTT foot-switch fold-in? | **YES, default OFF, operator opt-in toggle in Settings → TX.**  Edge-detect + 10 ms debounce in the forwarder.  Operator uses a foot switch on his station and will enable it; default-OFF protects everyone else from the `ff5f128` regression class (HW-PTT-in mis-read at RX rest → phantom-TX).  Bench-verify operator's HL2+ ptt_in rest-state before production wiring (still applies). |

**No remaining open questions for the implementing session.  Design
v2 is fully locked.**

---

## 7. Red-team round — completed 2026-05-29 (3 lenses, converged)

Three independent senior agents reviewed this design + dossiers with
file:line evidence against Thetis source.  All three converged
CONFIRM-WITH-AMENDMENTS in one round.  One BLOCKS-SHIP correctness
issue (§4.9 uslew dossier — now fixed); all other findings are
pin-the-mechanism amendments, all folded into v2 above.

**Verification asks both decisively answered:**
- §4.9 — uslew does NOT envelope SSB mic (only TUN/gen1).  Proof
  cited in §4.9.  MoxEdgeFade is the sole envelope, fade-in
  load-bearing.
- C&C round-robin modulus — confirmed 19 cases (0..18) at
  `networkproto1.c:948-1176`.  Lyra-cpp `% 19` is byte-exact.

**Agent A (concurrency)** — CONFIRM-WITH-AMENDMENTS: drop SPSC ring
in favour of the existing mutex+cv (single primitive class on the hot
path); producer-paced worker from mic-source callback (not
EP2-semaphore round-trip); pin `fexchange0` block size to `in_size
=126`; PTT teardown via condition_variable not main-thread poll; pin
explicit teardown order.

**Agent B (safety / §15.23-trap / wire-faithfulness)** —
CONFIRM-WITH-AMENDMENTS + 1 BLOCKS-SHIP: §4.9 uslew dossier inverted
(now corrected); init order push bandpass edges before
`SetTXAMode`; add `mox_delay=10ms` + `ptt_out_delay=20ms`; EP2
zero-on-no-MOX mandatory; fix the `SetTXAPanelGain1` default comment
(WDSP default is 1.0, not 4.0); add explicit "no DC/IQ calibration in
v0.2.0" status note.

**Agent C (scope / methodology / forward-compat)** —
CONFIRM-WITH-AMENDMENTS: drop `TciAudioMicSource` and
`PcSoundcardMicSource` from TX-1 (Thetis-faithful: mic is HL2 codec
only); add per-component bench gates (6); add the sip1 TX I/Q tap
for v0.3 PureSignal forward-compat; defer leveler to v0.2.1 was
operator-overridden (ship wired with UI toggle per §6.3).

---

## 8. Status

**DESIGN v2 LOCKED 2026-05-29.  Ready for the implementing session.**

3-lens red-team round complete (§7).  All amendments folded.  All §6
operator decisions answered.  No remaining design questions.

**Explicit status notes for the implementing session:**
- **No DC/IQ calibration in v0.2.0.**  Carrier suppression on the bench
  (§1 metric "Carrier (DC) ≤-60 dBFS") is hardware-limited by the HL2
  AD9866 DC trim, not software.  WDSP TXA's ALC + bp0 carrier
  suppression at create-time are ≥60 dB by design.  v0.3 ships
  `iqc.c`/`calcc.c` cffi for PureSignal-grade numbers.
- **HL2+ AK4951 mic = `Hl2Ep6MicSource` only** (single class).  Future
  PC-audio-for-TX is a VAC1-style feature, NOT another mic source.
- **Phase-3-EXIT kill-test (CLAUDE.md §15.20/§15.24-C class) BLOCKS
  any real-antenna keying.**  Bench: `taskkill /F` mid-TX into a
  dummy load, scope/confirm HL2 PA bias drops within the gateware
  watchdog window.  Until that bench passes, treat the watchdog as
  assumption-pending, NOT established fact.

**Implementing-session plan:**
1. Fix the EP6 mic byte parser per §3.2 (nddc-aware formula).
2. Verify EP2 C&C cases 10 & 11 byte composition per §5.4 (`line_in=0`
   default, expose mic_boost / mic_bias / line_in_gain to Settings).
3. **First bench gate**: with the parser fix + C&C correction, verify
   operator's AK4951 mic comes through EP6 (per-component bench gate
   #3 / §5.1).  This is the Q6.5 verification.
4. Implement components 1-6 per §5 with their per-component bench
   gates (§5.1).
5. Wire Settings → TX panel per §9 below.
6. End-to-end Tier-A unit bench (§1).
7. Hardware bench (§1).
8. Phase-3-EXIT kill-test (§1).
9. FAIL anywhere → revert that step, diagnose with captured data,
   no guess-fix.

---

## 9. Settings → TX panel UI spec

All wired-and-live (no-inert-UI per CLAUDE.md §15.13/14/15
discipline).  Each control plumbs to a real setter that takes effect
on apply.

| Control | Type | Default | Persists | Setter target |
|---------|------|---------|----------|---------------|
| **PHROT** | Toggle (on/off) | ON | per-mode? — TBD by impl | `SetTXAPHROTRun(ch, 0/1)` |
| **Leveler** | Toggle (on/off) | OFF | yes | `SetTXALevelerSt(ch, 0/1)` |
| **Bandpass low** | Slider 0..10000 Hz | 0 | per-mode | `TxChannel::setBandpass(low, high)` |
| **Bandpass high** | Slider 0..10000 Hz | 10000 | per-mode | `TxChannel::setBandpass(low, high)` |
| **Mic boost** | Toggle (on/off) | OFF | yes | EP2 C&C case 10 C2 bit 0 |
| **Mic input** | Radio (mic / line-in) | mic (`line_in=0`) | yes | EP2 C&C case 10 C2 bit 1 |
| **Mic bias** | Toggle (on/off) | OFF (electret operators turn on) | yes | EP2 C&C case 11 C1 bit (per `networkproto1.c:1091-1103`) |
| **Line-in gain** | Slider 0..31 | 0 | yes | EP2 C&C case 11 C2 |
| **HW-PTT input** | Toggle (on/off) | OFF (operator opt-in per §6.6) | yes | gate the EP6 `ptt_in` consumer |

Bandpass tooltip: "Set lower edge ≥50 Hz to suppress 50/60 Hz mains
coupling.  Higher edges above 4000 Hz enable ESSB audio width — verify
your TX BW conforms to your bandplan."

HW-PTT tooltip: "Forwards the HL2 EP6 foot-switch PTT input as a Lyra
PTT source.  Default OFF.  Some HL2 units (incl. the AK4951 variant
in some firmware revs) read non-zero at RX rest — enabling
unconditionally on those units causes spurious TX.  Bench-verify your
unit's ptt_in rest behavior before enabling."
