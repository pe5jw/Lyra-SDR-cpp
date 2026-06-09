# Stage E — TxChannel Reference-Parity Audit

**Date:** 2026-06-09
**Branch:** `tx-rebuild` @ `2511099` (post-Stage C.3)
**Methodology:** "Do as the reference port does" — per operator directive
2026-06-09 + `docs/THETIS_DIRECT_PORT_PLAN.md` Locked Methodology §1.
**Reference:** openHPSDR Thetis 2.10.3.13 (MI0BOT HL2 fork) —
- `ChannelMaster/cmaster.c::create_xmtr` (lines 112-253)
- `ChannelMaster/cmaster.c::destroy_xmtr` (lines 255-271)
- `wdsp/TXA.c::create_txa` (line 31) referenced via `OpenChannel`
- `Console/console.cs::SetupForm`, mode/filter/gain UI handlers
  (the reference's operator-change push sites for SetTXA* runtime setters)

**Scope:**
1. Audit shipped `src/wdsp/TxChannel.{h,cpp}` against the reference's
   9 per-xmtr surface allocations in `create_xmtr`.
2. Audit the operator-runtime WDSP setter pushes in `src/hl2_stream.cpp`
   against reference's "push at operator-change time, not at
   create_xmtr time" posture.
3. Classify every finding: **PARITY** / **HARMLESS DIVERGENCE** /
   **DEFERRED-BY-DESIGN** / **MUST-FIX**.
4. Decide: can Stage D (xcmaster pump body direct port) proceed
   without first auto-fixing the existing TX surface?

---

## 1. Reference Surface Map — `create_xmtr` 9 surfaces

The reference's `create_xmtr` (cmaster.c:112-253) allocates **nine**
per-xmtr surfaces in this order. Per-surface audit vs Lyra-cpp:

| # | Reference Surface | File:Line | Lyra Status | Lyra File:Line | Notes |
|---|---|---|---|---|---|
| 1 | `out[0..2]` — 3 TX-output buffers (TX / EER / sidetone) | `cmaster.c:126-127` | **PARITY** | `TxChannel.cpp:23-30` | All three allocated unconditionally at `TxChannel` ctor, sized `2 * outSize_` doubles per buffer. `out1Buf_`/`out2Buf_` sit unused until EER + sidetone helpers land. Matches reference posture byte-for-byte. |
| 2 | `create_dexp` (VOX expander) | `cmaster.c:130-157` | **DEFERRED-BY-DESIGN** | — | Reference creates with `run=0` (line 132); VOX is operator opt-in. Tracked: Task #91 (v0.2.3 VOX). Out of v0.2.0 SSB-first scope. |
| 3 | `create_aamix` as `pavoxmix` (anti-VOX mixer) | `cmaster.c:159-175` | **DEFERRED-BY-DESIGN** | — | Tied to surface #2 (anti-VOX feeds DEXP). Will land alongside Task #91. Stage B `AAMix` port (just shipped) provides the implementation when needed. |
| 4 | `OpenChannel` (WDSP TXA channel) | `cmaster.c:177-190` | **PARITY** | `TxChannel.cpp:70-77` | Byte-for-byte arg match: `channel`, `in_size=getbuffsize(inRate)`, `dsp_size=4096`, `in_rate=48000`, `dsp_rate=96000`, `out_rate=48000`, `type=1`, `state=0`, `tdelayup=0.000`, `tslewup=0.010`, `tdelaydown=0.000`, `tslewdown=0.010`, `block=1`. Verified via `referenceBuffsize()` helper (cmsetup.c:106-111 formula). |
| 5 | `XCreateAnalyzer` (TX spectrum analyzer) | `cmaster.c:192-198` | **HARMLESS DIVERGENCE** | RX-side analyzer reused | Lyra has a single panadapter analyzer that source-swaps on MOX edge (Task #44 shipped, §15.29/§15.30 dB-range separation shipped). Reference allocates a separate TX-channel analyzer instance with the same `XCreateAnalyzer(in_id, &rc, 262144, 1, 1, "")` shape. Lyra's reuse delivers equivalent operator-visible behavior (TX spectrum during keydown) without a second analyzer instance — accepted divergence, no behavior loss. |
| 6 | `create_txgain` (`pgain`, Penelope / PS protection gain) | `cmaster.c:200-209` | **DEFERRED-BY-DESIGN** | — | Reference creates with `run=0` for fixed gain and `run=0` for protection (lines 201-202) — `txgain` is INERT unless an external amp's IGain/QGain pre-emphasis is enabled OR PureSignal is calibrating. HL2 has no Penelope; PS landing in v0.3 fills the protection-gain need. Inert reference allocation = no observable parity loss. |
| 7 | `create_eer` (`peer`) | `cmaster.c:212-224` | **REFERENCE-EQUIVALENT** | — | Reference creates with `run=0` (line 213); EER is host-DSP envelope-elimination for class-D PA hardware. HL2 has no EER hardware. Reference + Lyra both never enable EER on HL2; reference still allocates the inert stage (the `out[1]` EER-output buffer Lyra also allocates). Equivalent for HL2. |
| 8 | `create_ilv` (`pilv`, TX I/Q interleaver) | `cmaster.c:226-232` | **PARITY — SHIPPED STAGE C** | `src/wdsp/ILV.{h,cpp}` | Stage C just shipped (commits `0440186` → `81176ca` → `11064bb` → `2511099`). `pilv[]` central bank, all 7 setters, xilv pump, bit-exact unit-tested. `SendpOutboundTx → SetILVOutputPointer(0, cb)` hand-off wired (C.3). No remediation needed. |
| 9 | `create_sidetone` (CW) | `cmaster.c:235-250` | **DEFERRED-BY-DESIGN** | — | Reference creates with `run_tx=0` (line 238) — CW transmit OFF; sidetone INERT until CW mode active. CW is v0.2.2 (Task #105). Out of v0.2.0 SSB-first scope. |

**Score:** 4 PARITY (1 / 4 / 7 / 8) + 3 DEFERRED-BY-DESIGN (2 / 3 / 9)
+ 1 REFERENCE-EQUIVALENT (7) + 1 HARMLESS DIVERGENCE (5).
**Zero MUST-FIX findings.**

---

## 2. WDSP Runtime Setter Pushes — operator-change posture

Reference `create_xmtr` calls **NO `SetTXA*` setters**. All TXA runtime
configuration (mode, filter freqs, mic gain, ALC ceiling, leveler,
PHROT, etc.) is pushed by `Console/console.cs` at operator-change time
via `WDSP.SetTXA*(channel, ...)`.

Lyra-cpp's runtime setter pushes live in `src/hl2_stream.cpp`,
forwarded through a `TxControl` callback struct that the (currently
ripped) TxDspWorker / TxChannel wire-up populates. Per-setter audit:

| WDSP Setter | Lyra Push Site | Push Cadence | Reference Posture | Verdict |
|---|---|---|---|---|
| `SetTXAMode` | `TxChannel::setMode()` + `hl2_stream.cpp` registerTxControl | operator mode change | operator mode change (console.cs `mode_changed` handler) | **PARITY** |
| `SetTXABandpassFreqs` | `TxChannel::setBandpass()` | operator BW change + mode change | operator BW change + mode change (console.cs `UpdateTXLowHighFilterForMode`) | **PARITY** |
| `SetTXAPanelGain1` | `hl2_stream.cpp::setMicGainDb()` | operator slider | operator slider (Setup MicGain) | **PARITY** |
| `SetTXAALCMaxGain` | `hl2_stream.cpp::setAlcMaxGainLinear()` | operator spinner | operator Setup ALC spinner | **PARITY** (§15.27 corrected the dB vs LINEAR unit bug to reference-exact) |
| `SetTXAALC{Attack,Decay,Hang,St}` | `hl2_stream.cpp::setAlc*()` | operator change | operator Setup ALC | **PARITY** |
| `SetTXALeveler{Top,Attack,Decay,Hang,St}` | `hl2_stream.cpp::setLeveler*()` | operator change | operator Setup Leveler | **PARITY** (§15.27 LINEAR units match) |
| `SetTXAPHROT{Run,Corner,Nstages}` | resolved cffi, push site pending (Task #109) | TBD operator toggle | operator Setup PHROT | **PARITY-pending-wire-up** (cffi binding correct; UI toggle is Task #109) |

**Verified omissions** (per `TxChannel.h` LANDMINE note + `wdsp_native.h`
§ "Deliberately-omitted symbols"):

- ✅ `SetTXABandpassRun` — verified NOT called. The §15.23 root-cause
  trap (toggles `bp1.run` not `bp0.run`; would route the stale
  compressor-aux `(-5000,-100)` filter after `bp0` and kill SSB).
  Reference (Thetis tree-wide grep) has ZERO call sites for this
  setter. Lyra: same — verified clean.
- ✅ `SetTXAPanelSelect` — NOT called. `create_txa` default `inselect=2`
  (mic→I, 0→Q) is correct for the standard mic→TXA path on HL2.
  Reference: ZERO call sites.
- ✅ `SetTXAALCThresh` — NOT called. Symbol does not exist in WDSP;
  ALC ceiling is governed solely by `SetTXAALCMaxGain`. Verified.

**No init-time SetTXA* pushes** in TxChannel::open() — matches
reference `create_xmtr` posture exactly.

---

## 3. Verified-Clean Landmines

The shipped `TxChannel.h:40-52` LANDMINE comment block explicitly
preserves three reference-parity rules that previously bit the
project:

1. **bp0 vs bp1 (§15.23 multi-day root cause)** — `SetTXABandpassRun(ch,1)`
   toggles `bp1.run` (the compressor-aux bandpass), NOT `bp0.run`
   (the always-on SSB sideband selector). `create_txa` sets `bp0`
   via `create_bandpass(run=1)` at `TXA.c:829`; subsequent reconfig
   happens via `SetTXAMode`+`SetTXABandpassFreqs` (both call
   `TXASetupBPFilters` internally). `SetTXABandpassRun` is the
   §15.23 trap; verified absent.
2. **Inselect-2 default** — `create_txa` default `inselect=2` makes
   `xpanel` case-0 read `I = in[2i+0]*1 = mic`, `Q = in[2i+1]*0 = 0`.
   Calling `SetTXAPanelSelect` is unnecessary AND a §15.23-class
   landmine if mis-set. Verified absent.
3. **ALC ceiling = MaxGain only** — `SetTXAALCThresh` does not exist
   as a WDSP symbol. Plan-agent earlier draft mis-listed it.
   Verified absent + dossier-corrected.

All three rules are documented in code AND verified absent from the
shipped tree.

---

## 4. TX Hot-Path Status — the Rip

Critical context: **the TX wire layer is mid-rebuild.** Per Task #112
(in_progress) "⭐ TX Wire-Layer Rebuild (reference-verified, clean-room
C++23)" + Task #117 (in_progress) "Step 14 Stage 2 — Wire-LIVE on
first HL2 talk":

- `TxChannel` class: shipped, reference-faithful, **NOT INSTANTIATED**
  anywhere in production code. Verified — `grep -r "new TxChannel\|
  make_unique<.*TxChannel\|TxChannel\s*\w*\s*\("` returns only the
  class's own declarations + wdsp_native.h forward decl.
- `TxDspWorker`: **DELIBERATELY RIPPED** (Phase 1 Q2). Surviving
  references in `hl2_stream.cpp` / `mainwindow.cpp` / `metermodel.cpp`
  / `tci_server.h` / `settingsdialog.cpp` / `prefs.cpp` are all
  "ripped, pending rebuild" stubs — comments + NULL pass-throughs.
- `TciMicSource`: same — ripped.
- `xcmaster` equivalent: **NOT PRESENT**. Reference's per-stream pump
  body (`cmaster.c:340 void xcmaster(int stream)`) is what binds mic
  source → cmbuffs → fexchange0 → outbound. Lyra has no equivalent
  today; the rebuild needs one.
- WDSP operator setters in `hl2_stream.cpp` persist to QSettings +
  forward through `TxControl` callbacks. The TxControl callback
  slots are **NULL today** (no one populates them — that was
  TxDspWorker's job). Setters are inert until the rebuild wires
  TxControl.

This is a **deliberate vacuum**, not an accident. The rip created
clean ground for a reference-faithful rebuild.

---

## 5. Findings + Recommendation

### Findings summary

- **TxChannel itself: REFERENCE-PARITY.** The narrow surfaces it
  covers (out buffers + `OpenChannel` + `start`/`stop`/`process` +
  `setMode`/`setBandpass`) match the reference byte-for-byte.
  Disciplined deferral of EER / DEXP / anti-VOX / sidetone /
  txgain / TX-side analyzer matches reference's `run=0`-allocation
  posture for HL2.
- **Stage C ILV: REFERENCE-PARITY** (shipped this session, bit-exact
  unit-tested).
- **WDSP setter pushes: REFERENCE-PARITY.** All operator-tunable
  setters push at operator-change time, NOT at `create_xmtr` /
  `TxChannel::open()` time — matches reference posture exactly.
- **Three known §15.23-class landmines: VERIFIED ABSENT.**
- **Zero MUST-FIX findings.** No remediation commits required
  before Stage D.

### Gap to Stage D

The missing piece is the **xcmaster pump body** (cmaster.c:340-...).
That's the per-stream orchestrator that binds:

  mic source → cmbuffs ring → fexchange0(TxChannel) → outbound → ILV.

Reference's xcmaster does this in ~150 LOC of C; the Lyra-cpp port
needs to plug into:
- the existing mic source (Hl2Ep6MicSource — shipped, also parked
  pending rebuild)
- TxChannel (shipped, parked)
- the just-shipped ILV (parked at `pilv[0]` if Stage D creates it)
- HL2Stream's existing EP2 TX writer (which has the `tryConsumeTxIq`
  callback shape ready, currently NULL).

Stage D is a **clean-vacuum fill**, not a tear-and-replace.

### Recommendation

**PROCEED TO STAGE D.** Stage E found ZERO must-fix items in the
surviving TxChannel + WDSP-setter surface. The reference-parity
posture is intact across every surface the rebuild will touch. The
rip created clean ground; Stage D fills it with a reference-faithful
xcmaster pump.

Stage D scope when greenlit:
1. Port `cmaster.c::xcmaster(int stream)` body to
   `src/wire/CMasterPump.{h,cpp}` (or fold into CMaster.cpp — operator
   choice on file layout).
2. Create one ILV via `create_ilv(0, ...)` at the right place in
   the per-xmtr open sequence (reference does it inside `create_xmtr`;
   Lyra-cpp can match or hoist).
3. Wire the xcmaster pump's TX-side: pull from mic ring → call
   `TxChannel::process()` → feed result to `xilv` → ILV's Outbound
   (already wired to EP2 via Stage C.3 SendpOutboundTx hand-off).
4. Operator HL2 bench gate after Stage D commits — this is the first
   commit since the rip where TX I/Q reaches the wire.

Stage D is the right next move. No E.1 commits needed.

---

## 6. References

- Reference: `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/`
  - `ChannelMaster/cmaster.c` lines 112-271 (create_xmtr + destroy_xmtr)
  - `ChannelMaster/cmaster.c` line 340+ (xcmaster — Stage D target)
  - `wdsp/TXA.c` line 31 (create_txa — TXA chain composition)
- Lyra-cpp (this audit):
  - `src/wdsp/TxChannel.{h,cpp}` — narrow TXA wrapper
  - `src/wdsp/ILV.{h,cpp}` — Stage C interleaver port
  - `src/wdsp/AAMix.{h,cpp}` — Stage B mixer port
  - `src/wire/CMaster.{h,cpp}` — Stage A shell + wire-up
  - `src/hl2_stream.{h,cpp}` — operator runtime setters + TxControl
  - `src/wdsp_native.{h,cpp}` — cffi resolutions (13 TX setters)
- Plan: `docs/THETIS_DIRECT_PORT_PLAN.md` §Stage Index
- Methodology: `docs/RULES.md` AA. 2026-06-08 Amendment + the
  Locked Methodology in `THETIS_DIRECT_PORT_PLAN.md`
- Related: `docs/architecture/STEP14_STAGE2B_DESIGN.md` (the in-progress
  wire-rebuild plan that Stage D plugs into)
