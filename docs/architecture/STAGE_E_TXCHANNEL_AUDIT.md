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
| 5 | `XCreateAnalyzer` (TX spectrum analyzer) | `cmaster.c:192-198` | **MUST-FIX (deferred to Stage E.1, post-Stage-D)** | `wdsp_engine.cpp:775-788` (one slot only, `kAnDisp`) | Lyra has a single panadapter analyzer reconfigured on every MOX edge via `SetAnalyzer(...)` (§15.29/§15.30 dB-range/avg separation work). Reference allocates a SEPARATE TX-channel analyzer instance with `XCreateAnalyzer(in_id, &rc, 262144, 1, 1, "")` — both analyzers run concurrently, UI picks which to display. **Reclassified MUST-FIX 2026-06-09 per operator + PS prerequisite:** PureSignal v0.3 `calcc.c` IMD measurement feeds from the TX analyzer; keeping the single-analyzer hack paints v0.3 PS into a structural corner (§15.23-class forward-reasoning trap that would have to be undone later at much higher cost). Tracked: Task #140 (Stage E.1, blocked-by Task #112). |
| 6 | `create_txgain` (`pgain`, Penelope / PS protection gain) | `cmaster.c:200-209` | **DEFERRED-BY-DESIGN** | — | Reference creates with `run=0` for fixed gain and `run=0` for protection (lines 201-202) — `txgain` is INERT unless an external amp's IGain/QGain pre-emphasis is enabled OR PureSignal is calibrating. HL2 has no Penelope; PS landing in v0.3 fills the protection-gain need. Inert reference allocation = no observable parity loss. |
| 7 | `create_eer` (`peer`) | `cmaster.c:212-224` | **REFERENCE-EQUIVALENT** | — | Reference creates with `run=0` (line 213); EER is host-DSP envelope-elimination for class-D PA hardware. HL2 has no EER hardware. Reference + Lyra both never enable EER on HL2; reference still allocates the inert stage (the `out[1]` EER-output buffer Lyra also allocates). Equivalent for HL2. |
| 8 | `create_ilv` (`pilv`, TX I/Q interleaver) | `cmaster.c:226-232` | **PARITY — SHIPPED STAGE C** | `src/wdsp/ILV.{h,cpp}` | Stage C just shipped (commits `0440186` → `81176ca` → `11064bb` → `2511099`). `pilv[]` central bank, all 7 setters, xilv pump, bit-exact unit-tested. `SendpOutboundTx → SetILVOutputPointer(0, cb)` hand-off wired (C.3). No remediation needed. |
| 9 | `create_sidetone` (CW) | `cmaster.c:235-250` | **DEFERRED-BY-DESIGN** | — | Reference creates with `run_tx=0` (line 238) — CW transmit OFF; sidetone INERT until CW mode active. CW is v0.2.2 (Task #105). Out of v0.2.0 SSB-first scope. |

**Score (corrected 2026-06-09 per operator + PS-prerequisite review):**
4 PARITY (1 / 4 / 7 / 8) + 3 DEFERRED-BY-DESIGN (2 / 3 / 9) +
1 REFERENCE-EQUIVALENT (7) + **1 MUST-FIX (5 — TX analyzer port,
deferred to Stage E.1 post-Stage-D per PS prerequisite — see §5.5)**.

The earlier classification of surface #5 as "HARMLESS DIVERGENCE"
was a downgrade I should not have made.  "Do as the reference port
does" is the locked methodology -- any divergence needs a justified
reason, not a comfort grade.  Surface #5 has neither.  Corrected.

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
  txgain matches reference's `run=0`-allocation posture for HL2.
- **Stage C ILV: REFERENCE-PARITY** (shipped this session, bit-exact
  unit-tested).
- **WDSP setter pushes: REFERENCE-PARITY.** All operator-tunable
  setters push at operator-change time, NOT at `create_xmtr` /
  `TxChannel::open()` time — matches reference posture exactly.
- **Three known §15.23-class landmines: VERIFIED ABSENT.**
- **ONE MUST-FIX finding (surface #5 — TX analyzer port).**
  Sequenced AFTER Stage D ships first-RF (Stage E.1, Task #140);
  load-bearing for v0.3 PureSignal — see §5.5 below.

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

**PROCEED TO STAGE D, THEN STAGE E.1.**  Locked sequence per
operator 2026-06-09:

  Stage C (DONE) → Stage D (xcmaster pump body, first-RF) →
  Stage E.1 (TX analyzer port to reference parity, PS prereq).

**Stage D scope** when greenlit:
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
5. Stage D ships using the CURRENT single-analyzer architecture
   (the §15.29/§15.30 MOX-edge `SetAnalyzer` reconfigure stays
   intact for first-RF).  E.1 lifts that hack post-bench.

**Stage E.1 scope** when greenlit (after Stage D bench passes):
1. **E.1.0** — Declare `kAnDispTx` analyzer slot ID; verify
   `XCreateAnalyzer` / `DestroyAnalyzer` / `SetAnalyzer` /
   `Spectrum0` cffi resolutions handle multi-slot (they do —
   already keyed by `disp` arg).
2. **E.1.1** — Allocate the TX analyzer at TX-channel-open with
   `XCreateAnalyzer(kAnDispTx, &rc, 262144, 1, 1, "")` — byte-exact
   args per cmaster.c:192-198.  Destroy at TX-channel-close
   (parity with `destroy_xmtr` cmaster.c:264 `DestroyAnalyzer(inid(1,i))`).
3. **E.1.2** — Retarget `TXASetSipDisplay(...)` → `kAnDispTx` so
   the TX siphon feeds the TX analyzer.  STOP the MOX-edge
   `SetAnalyzer` reconfigure of the RX slot (`wdsp_engine.cpp:443`
   and the TX-state branch at `:543`); RX analyzer stays in RX
   configuration permanently.
4. **E.1.3** — Panadapter UI: source-select read from `kAnDisp`
   vs `kAnDispTx` based on MOX state (initial UX = swap on MOX,
   matches current operator-visible behavior).  Eventually
   simultaneous display becomes possible (operator UX call —
   tracked separately).
5. Bench gate: RX panadapter spectrum **no longer freezes** during
   keydown (it keeps updating from the still-RX-configured
   `kAnDisp` analyzer; TX state is rendered from `kAnDispTx`).

§15.29/§15.30 dB-range/avg/FFT-size separation work stays useful —
becomes per-analyzer settings rather than per-MOX-state of a
single slot.  No throwaway code.

---

### §5.5 — Why TX analyzer is a v0.3 PureSignal prerequisite

The reference's PS calibration (`wdsp/calcc.c::calc()`,
PSForm.cs timer-driven FSM) feeds from the TX-side analyzer's
spectrum data to measure IMD products from the PA feedback DDC
samples.  Specifically:
- PS feedback DDCs (DDC2/DDC3 on HL2 per CLAUDE.md §3.8) deliver
  PA output samples to `calcc.c`'s ring buffer.
- `XCreateAnalyzer(inid(1, i), ...)` is the TX-channel analyzer
  the PS dialog reads to display IMD product levels + drive the
  auto-attenuator FSM decisions (recalibrate when `FeedbackLevel
  > 181 || (FeedbackLevel <= 128 && cur_att > -28)` per
  PSForm.cs:1142).
- If Lyra keeps the single-analyzer reuse architecture into v0.3,
  PS's calcc would need to either:
    (a) timeshare the analyzer with the operator panadapter
        (introducing analyzer-config thrash that calcc was never
        designed for + visible UI artifacts), OR
    (b) sidestep the WDSP analyzer entirely and reimplement the
        IMD measurement in Lyra-native (large new attack surface,
        reference-divergent v0.3 PS port).
  Both choices are §15.23-class forward-reasoning traps that would
  have to be undone later at much higher cost than fixing the
  TX-analyzer port now (post-Stage-D).

Adopting reference parity in E.1 (TX analyzer is its own slot,
created at TX-channel-open, destroyed at TX-channel-close) means
v0.3 PS's calcc plumbing reads from `kAnDispTx` exactly the way
reference's calcc reads from `inid(1, i)` — straight port, no
sidesteps.  This is the right kind of forward investment:
spending E.1 commits now to avoid a v0.3 PS structural wall.

(Logged in this audit + Task #140 description so the rationale
survives the next session compaction.)

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
