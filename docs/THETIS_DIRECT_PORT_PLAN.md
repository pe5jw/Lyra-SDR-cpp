# LYRA-CPP — THETIS DIRECT-PORT PLAN (TX)

**Authoritative doc for the current TX work direction.**

> [!] The older `EXECUTION_PLAN.md` (Step 14 wire-layer rebuild,
> Phase 3 etc.) describes a DIFFERENT approach that is now
> superseded for the TX work.  Do NOT mix the two plans — that
> doc tracks an earlier wire-rebuild pass which was partial and
> Lyra-native-leaning.  THIS doc is the active plan going
> forward.  Old doc remains in tree for historical reference
> only (some shipped pieces from it are still in production —
> noted per-component in the registry below).

**Branch:** `tx-rebuild` (the direct-port work lives here;
`origin/main` carries the earlier Step 14 / TX-1 line and is
deliberately untouched by the direct-port arc).

**Reference tree (the ONLY trusted TX reference):**
- `D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\`
  - `wdsp\*.c` — WDSP DSP engine (GPL v3+, port-with-attribution OK)
  - `ChannelMaster\*.c` — wire layer (GPL v3+, port-with-attribution OK)
  - `Console\*.cs` — C# UI/control glue (study ONLY; write Lyra-native equivalents)
- `Y:\Claude local\_hl2src\` — HL2+ ak4951v4 gateware RTL
  (`control.v` / `radio.v` / `usopenhpsdr1.v` etc. — the ground
  truth when host-side reference and gateware disagree)

**Provenance & attribution rule (operator-locked 2026-06-09, supersedes the 2026-05-31 "no-attribution-in-commits" tightening):**

Lyra-cpp is a derivative work of openHPSDR Thetis (ChannelMaster + WDSP modules), GPL v3+. The GitHub project description openly states this: *"TX baseline ported from openHPSDR Thetis (ChannelMaster) with Lyra-native DSP additions."* Open attribution is both legally required (GPL) and operator-intended (public project posture).

1. **License attribution at file head — REQUIRED.** Any file ported from WDSP or ChannelMaster carries a header block crediting upstream (NR0V for WDSP modules; openHPSDR Thetis for ChannelMaster modules) + the GPL v3+ notice. Existing `AAMix.{h,cpp}`, `CMaster.cpp`, `RadioNet.cpp` etc. already follow this; keep it.

2. **Code comments citing source — ENCOURAGED.** Citations may name the reference openly (`Reference: Thetis ChannelMaster cmaster.c:297-313`, `// Per openHPSDR networkproto1.c:1078`, `// HL2+ ak4951v4 gateware control.v:209-220`). The file:line is the primary technical content; the reference-app name is context that helps future readers find the source tree.

3. **Commit messages — REFERENCE-APP NAMES PERMITTED.** A commit like *"port aamix.c from Thetis ChannelMaster with Lyra-native idiom translations"* is fine — it matches the public GitHub posture and makes the history grep-able. The earlier `4cef88d` tightening (2026-05-31) that put commit messages on the no-attribution list is **rescinded**.

4. **Public docs (README, `docs/*.md` design docs, memory files, this plan) — OPEN ATTRIBUTION.** Same posture as the GitHub description.

5. **User-facing UI strings — KEEP OPERATOR-FOCUSED.** UI labels operators see in normal operation (panel titles, button text, error toasts) describe Lyra behavior, not provenance. *"AAMix output (HL2 jack)"* not *"Thetis-ported AAMix output"*. About dialog / credits screen / install docs MAY (and probably should) name openHPSDR Thetis + WDSP/NR0V openly — credit where due. This is the only category that stays "Lyra-native voice in operator-facing labels," because operators interact with Lyra, not its source provenance.

6. **The "Lyra-Native style governs surrounding architecture only" rule (Stage B-locked) STANDS unchanged.** Reference DSP API call patterns are byte-for-byte faithful; Qt/Vulkan/QML/process model is Lyra-native.

7. **Reference precedence STANDS unchanged.** Verify TX ONLY against Thetis 2.10.3.13 (`D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\...`) + the HL2+ ak4951v4 gateware RTL (`Y:\Claude local\_hl2src\`). **Old Python Lyra is NOT a trusted TX reference** (its TX never worked).

**Consequence of the 2026-06-09 amendment:** the three historical commits (`efdde02`, `604d87b`, `ed9f0cd`) that "predated the no-attribution rule" and contained "Thetis" in their messages are **no longer in violation** — they're now consistent with the rule. Task #73 ("Cleanup pre-existing 'Thetis' mentions in shipped code") is **OBSOLETED** by this amendment.

---

## LOCKED METHODOLOGY (Stage B-validated, do NOT relax)

These rules were earned through the Stage B aamix.c port arc
(2026-06-08) — see SESSION_LOG.md for the full trail.

1. **Smallest revertable empirical step -> operator HL2 bench ->
   next step.**  Each commit is one focused change with a
   falsifiable bench gate.  Multi-step "design then implement"
   is for stages that genuinely need it (operator approves);
   default is iterate-and-bench.

2. **"Do as Thetis does, Lyra-Native style"** (locked
   2026-05-30 after the `fexchange0` crash arc, re-validated
   tonight after the B.6.b shortcut attempt cost a revert
   cycle).  Three sub-rules:
   - Every WDSP API call matches the reference byte-for-byte
     (parameter values, order, the SET of setters fired, the
     lifecycle stage at which they fire).  If tempted to pick
     a value the reference doesn't use, STOP and find what the
     reference uses.  "I have a reason to pick differently"
     means I don't.  Reference has been right every time.
   - "Lyra-Native style" governs surrounding architecture ONLY
     (Qt + Vulkan + QML + process/thread model + facade APIs +
     IPC + UI shell).  The WDSP DSP-engine call surface is
     reference-bound.
   - Provenance only in code COMMENTS + design docs + memory.
     Never commit messages, never UI strings.

3. **Counters first when the symptom is "nothing happens".**
   The Stage B 2-stage instrumentation (chain counters in
   xMixAudio/mix_main + dispatch counters in
   dispatchAudioFrame) localized the silent-audio defect to a
   single line of code on the 2nd bench.  Pure speculation
   would have cost a multi-round revert/speculate cycle.
   Atomic counter + rate-limited qInfo = trivial overhead, very
   high diagnostic ROI when "I press play, hear nothing, log
   looks normal."

4. **Operator-empirical rule overrides agent inference.**
   When operator hardware data contradicts what an
   audit/red-team/Plan-agent concluded, the data wins.  Do not
   defend the audit — re-open, bisect, fix.  This rule has
   fired repeatedly through the broader project arc; the Stage
   B B.6.b-fix1 fix was an instance (chain counters healthy
   suggested AAMix was correct; the temptation was to revert
   the whole port; the empirical pattern instead pointed at
   the wire-up surface where the bug actually was).

5. **Read the reference FIRST, in shipped-code form, before
   designing.**  Every time we designed first and read second,
   we shipped a regression.  Every time we read first, we got
   the right answer in one pass.  The no-attribution rule
   applies to shipped code; it does NOT mean "don't read the
   reference."  Read it, dossier the file:line in the plan
   doc, then write Lyra-cpp.

6. **Session-start discipline:** `git fetch origin` is the
   FIRST action every session, before reading any local state.
   If divergence exists, surface it to the operator BEFORE
   writing code.  No exceptions.

7. **Push after every component ships, not at EOD.**  Each
   clean operator HL2 bench -> backup bundle + push immediately.
   Smaller in-flight delta = smaller surprise blast radius.

8. **Backup bundle naming:** `_backups/lyra-cpp-YYYY-MM-DD-<topic>.bundle`.
   `git bundle create … --all` (covers all refs).

---

## STATUS REGISTRY — what's done, in-progress, pending

> ⭐ **2026-06-13 — TX BRING-UP COMPLETE (P0 → P4.b) + SHIPPED.**
> The verbatim direct-port wire chain is LIVE and operator-HL2-bench-passed:
> SSB voice both sidebands, Phase-3-EXIT RF-safety kill-test, TUN
> Thetis-exact zero-beat, and **MSHV → TCI → FT8 real on-air QSOs**
> (incl. F4FLF ~4160 mi DX).  §7 dead-code retirements done.  Released as
> **v0.2.3** (`main` == `tx-rebuild` == `e8aafbc`; tags `v0.2.3` +
> `tx-working-2026-06-13`; GitHub Release + installer published).  Normal
> feature dev resumes off `main`.  Next milestones: PC/VAC mic-input paths
> (#102/#103/#104), then v0.3 PureSignal (the P0.d `_cmaster` struct
> already carries the full PS surface — no retrofit).

> ⭐ **2026-06-15 — VAC (#102/#158) SHIPPED as v0.3.0; post-P4 ordering LOCKED.**
> The next-milestone PC/VAC mic-input path shipped: the `ivac.c` →
> `wire/IVAC` device-layer rework (single full-duplex PortAudio stream,
> VAC/TCI crash fixes, RX-muted-on-TX) released as **v0.3.0** (`main` ==
> `tx-rebuild`).  **Open loose-end from that arc:** the TX MIC/ALC meters
> were re-homed onto `GetTXAMeter(chid(1,0)=1)` *inside* the VAC work
> ("#158 (post-DL) re-home" in `metermodel.cpp` / `wdsp_engine.cpp`) but
> never got their own bench gate — operator reports MIC/ALC read nothing.
> Treat as **UNVERIFIED, not done** (the WDSP-TX-chain leveler/ALC rows are
> `[WIP] "confirm reference-parity"`).  Closing it = a deep Thetis read of
> the TX-meter read/display path (which `txaMeterType`, which channel,
> where in the chain) + port + bench gate.  Code-level contributors found
> so far (candidates, NOT yet root-caused — do not patch blind): the
> secondary-readout line is still hard-stubbed `"—"` (`formatSecondaryText`,
> metermodel.cpp:577-582); and the AK4951 codec mic delivers zero into the
> TXA until a rate change because the at-open seed (hl2_stream.cpp:837)
> sets the rate bits but not `mic_decimation_factor` (defaults 0 = the
> `++count == factor` harvest gate never fires).
>
> **LOCKED post-P4 feature ordering (operator decision 2026-06-15):**
> 1. **TX MIC/ALC metering** — Thetis-reference read + port + bench
>    (closes out the VAC arc; verify-first, counters-first per the locked
>    methodology — do NOT patch the candidates above blind).
> 2. **Stage F — TX DSP chain** (EQ / compressor-Combinator / leveler /
>    ALC + their meters; broad TX value; feeds the in-flight #49 Profile
>    Manager).
> 3. **RX2 + Split** (#96–#101) — daily-use value, AND it builds the
>    `(mox, ps_armed, rx2_enabled)` DDC-dispatch state machine that PS
>    reuses (RX2 lives on DDC1; PS commandeers DDC0/DDC1 during MOX+PS).
> 4. **PureSignal (Stage G/H, v0.3)** — LAST.  Narrowest audience (needs
>    the HL2 hardware mod + PS gateware), deepest / most cross-cutting
>    surface; lands on top of the dispatch foundation RX2 builds, no wire
>    retrofit (the P0.d `_cmaster` struct already carries the PS surface).
>    Rationale: PS gets cheaper + safer AFTER RX2, not merely
>    lower-priority.  The plan's F-before-G was version cadence (v0.2.x
>    dynamics → v0.3 PS), NOT a hard compile dependency — so RX2 slots
>    between F and PS.

> ⭐ **2026-06-17 — VAC closeout v0.3.1 + Stage-F native TX DSP rack +
> Profiles + TX monitor SHIPPED (post-P4).**  All native (Lyra-side, NOT
> direct ports — see the SHIPPED-WORK LOG entry for hashes).  **v0.3.1**
> (`a3a517f`, 2026-06-15) closed the VAC arc: the enable/disable crash fix,
> Vol/Mute routed to VAC, AND the TX MIC/ALC/LEV metering the v0.3.0 banner
> above flagged UNVERIFIED is now wired Thetis-format (`9594bd5` #160) — so
> that loose-end is **resolved in code** (the WDSP leveler/ALC rows below are
> flipped to [DONE] for their cffi bindings + metering wiring).  NOTE: the
> #160 metering **bench close-out + Brent/Timmy v0.3.1 field-confirmation are
> still IN PROGRESS** — the meters are shipped, not yet field-validated.
> **Stage F TX DSP rack (native):**
> 8-band parametric EQ (#50), Speech processor / Auto-AGC leveller + de-esser
> (#88), X-Air-style Combinator (#51), Plate (#52), with digital-mode
> auto-bypass + digital/CW lamp dim (#163).  **Profiles #49** rack
> capture/apply (schema v3) + sideband/band-agnostic SSB profile + panel-lock
> fix + Speech-profile capture/lamp refresh (#162).  **TX audio monitor #90**
> — 3 reference-faithful routes (HL2 jack / VAC device / TCI RX-audio) + the
> inline Audio-panel "Out" picker & MON→MON-TX relabel (#164).  Released as
> **v0.3.1** (`main` == `tx-rebuild`); the native DSP/profile/monitor work
> rides on top (HEAD `dc45485`).  **NEXT (current open queue — one task):** an
> AM/DSB RX-DSP bug — AM and DSB produce only the upper sideband (lower
> missing); the WDSP passband for AM/DSB is likely set one-sided instead of
> symmetric -W..+W.  (GitHub / main-branch posture questions pending operator
> discussion.)

Format: per-file or per-component row.  Status tags (ASCII so they render identically in the PDF/DOCX siblings):
- [DONE] DONE + operator-bench-validated
- [WIP] IN-PROGRESS (committed but not yet bench-validated, or
  partially shipped)
- [PEND] PENDING (planned, not started)
- [N/A] N/A (Lyra-native equivalent already shipped; no direct
  port planned — see notes column)

### Wire layer (ChannelMaster — port directly w/ attribution)

| Source                                  | Target (lyra-cpp)                               | Status | Notes |
|-----------------------------------------|-------------------------------------------------|--------|-------|
| WDSP exports linkage (`OpenChannel`/`fexchange0`/analyzers/PS family) | `src/wire/wdspcalls.{h,cpp}` (P0.a)  | [DONE]     | **P0.a `e0c0f4a` 2026-06-09.** The single operator-approved linkage seam: fn-ptr table carrying the WDSP exports' exact names, resolved once from the loaded wdsp.dll. PureSignal entry family complete (verified vs dumpbin). RESAMPLE-typed + `flush_resample` added at P0.c. |
| `ChannelMaster\cmcomm.h` (family common header) | `src/wire/cmcomm.{h,cpp}` (P0.b -> P0.d)  | [DONE]     | `complex` typedef + PORT + verbatim malloc0 (wdsp/utilities.c:37-43) + PI + the reference include surface. P0.d added the opaque verbatim twin typedefs for deferred-subsystem types (ANB/NOB/VOX/TXGAIN/ANALYZERS + DELAY at P2.a); **EER completed at P2.a 2026-06-12** — full verbatim struct (wdsp/eer.h:30-49) + the umbrella-mapping note (.cpp files include explicit family headers — an include-list umbrella would be circular under `#pragma once`). |
| `ChannelMaster\ilv.c` (TX I/Q interleaver) | `src/wire/ILV.{h,cpp}` (P0.b)                | [DONE]     | **P0.b `1f49d98` 2026-06-11.** VERBATIM direct port (`ilv,*ILV` twin typedef, raw Outbound fn ptr, `pcm->xmtr[].pilv` setters, malloc0/_aligned_free). Mechanical diff vs ilv.{h,c} = whitespace-only. The earlier `src/wdsp/ILV` retrofit + its pilv[] bank are retired. scratch/test_ilv ALL PASS. |
| `ChannelMaster\cmbuffs.{h,c}` (CMB ring + cm_main pump) | `src/wire/CmBuffs.{h,cpp}` (P0.d)   | [DONE]     | **P0.d `afc7950` 2026-06-12.** VERBATIM rewrite (`cmb,*CMB`, `#define CMB_MULT (3)`, malloc0/_aligned_free; the Phase-C retrofit's calloc/intptr_t/guard deviations removed; `pcm->in[]` alloc moved back to create_cmaster per reference). Mechanical diff = whitespace-only. [DONE] **P0.d operator HL2 RX-regression bench PASSED 2026-06-12.** |
| `ChannelMaster\cmsetup.{h,c}` (radio structure + id helpers) | `src/wire/cmsetup.{h,cpp}` (P0.d, NEW) | [DONE]     | **P0.d `afc7950`.** VERBATIM: cmMAX* sizing macros (16/4/4/2/32), rxid/txid/sp0id/stype/chid/inid/mixinid/getbuffsize, SetRadioStructure + set_cmdefault_rates. main.cpp config: SetRadioStructure(2,1,1,1,0,…) -> chid(0,0)=0 RX1 / chid(1,0)=1 TX matches the live channel layout. [DONE] Bench PASSED 2026-06-12. |
| `ChannelMaster\cmaster.{h,c}` (FULL: struct + create/destroy + xcmaster + setters) | `src/wire/CMaster.{h,cpp}` (P0.d)   | [DONE]     | **P0.d `afc7950`.** VERBATIM rewrite: full `_cmaster` struct (cmaster.h:39-99 incl. the PS surface — out[3]/panalalloc/pgain/peer), `cmaster,*CMASTER`, raw TCI callback fn ptrs, `enum AudioCODEC`, `cm = {0}`. Verbatim no-arg create_xmtr/destroy_xmtr (out[0..2] malloc0 + OpenChannel(ch 1) + XCreateAnalyzer(TX disp 1) + create_ilv through the wdspcalls seam — **the TxChannel RAII carve-out is DELETED**, `src/wdsp/TxChannel.{h,cpp}` retired). create_cmaster/destroy_cmaster verbatim per-stream loops (update[] CS + cmbuffs + in[] for all cmSTREAM streams). xcmaster verbatim (update[] CS restored, real stype/txid/chid, TCI memset restored; fexchange0 + monitor xMixAudio + xilv live). All Sendp*/Set* setters ported. Deferred subsystem lines (dexp/anti-vox/txgain/sidetone/pipe/cmasio/analyzers.c/create_rcvr-body) carried IN PLACE as reference text with DEFERRED tags; **eer RESTORED VERBATIM at P2.a 2026-06-12** (create_eer run=0 / destroy_eer / xeer / pSetEER* via the wdspcalls seam — the object exists so the sendOutbound/sendProtocol1Samples `peer->run` derefs are valid, per cmaster.c:212-224). Sole code accommodation: `(char*)""` at XCreateAnalyzer. [DONE] **Operator HL2 RX-regression bench PASSED 2026-06-12** (RX working on the new per-stream pump/ring layout). |
| `ChannelMaster\obbuffs.c` (TX-out ring/seam) | `src/wire/ObBuffs.{h,cpp}` (P1, NEW)       | [DONE]     | **P1 `75754c2` 2026-06-12** — obbuffs.{h,c} verbatim (obb/*OBB twin typedef, file-scope obp four-alias bank, the reference's own calloc/free 2014-era idioms kept). Mechanical diff: .cpp IDENTICAL; .h sole delta = the include-guard define replaced by #pragma once (documented packaging). The `sendOutbound(id, a->out)` ob_main hand-off was restored at P2.c (no longer deferred). **Wire-LIVE since P4.b 2026-06-13** — create_obbuffs/ob_main are live in the direct-port TX chain (TU no longer dormant); was [WIP] SHIPPED while dormant, now DONE in the live chain. |
| `ChannelMaster\networkproto1.c` (HL2 wire writer) | `src/wire/NetworkProto1.cpp` (sendProtocol1Samples) + FrameComposer write_main_loop_hl2 (LIVE) | [DONE]     | **Wire-LIVE at P4.b 2026-06-13.** sendProtocol1Samples verbatim drives the EP2 writer thread; FrameComposer monolithic write_main_loop_hl2 is the WriteMainLoop_HL2 equivalent (#121/#122 fold). The old `hl2_stream.cpp` Step-14 mix retired at §7. |
| `ChannelMaster\network.c` (UDP + keepalive) | `src/wire/ObBuffs.{h,cpp}` ob_main + sendOutbound (LIVE) | [DONE]     | **Wire-LIVE at P4.b.** sendOutbound verbatim (P2.c); the Step-14 Lyra-native OutboundRing + Ep2SendThread translations RETIRED at §7 (`fe6a438`). |
| `ChannelMaster\netInterface.c::create_rnet` | `src/wire/RadioNet.cpp::create_rnet` (LIVE)     | [DONE]     | Direct port shipped earlier. The Stage-A SendpOutboundRx no-op stub at line 272 was DELETED 2026-06-08 (B.6.b-fix1, commit `8b8e0da`) because it clobbered the AAMix wire-up; future codec-id switching must NOT re-introduce a default registration here. P3 = the outbound registrations (SendpOutboundTx(OutBound) etc.) — MUST register AFTER create_xmtr (the verbatim setters have NO null guards, reference ordering). |

### WDSP RX chain (port directly w/ attribution)

| Source                       | Target (lyra-cpp)                  | Status | Notes |
|------------------------------|------------------------------------|--------|-------|
| `ChannelMaster\aamix.{c,h}`  | `src/wire/AAMix.{h,cpp}` (P0.c)    | [DONE]     | **P0.c `e0f2584` 2026-06-11 — VERBATIM re-port** (supersedes the 2026-06-08 Stage-B C++23-idiom translation, which the operator rejected as a deviation class). `aamix,*AAMIX` twin typedef + anonymous slew struct, raw Outbound fn ptr, paamix[] bank private to the .cpp, Win32 primitives direct. Mechanical diff vs aamix.{h,c} = whitespace-only. `src/wire/resample.h` = the public RESAMPLE ABI verbatim. WdspEngine RX wire-up: capturing lambda -> static member fn + TU-scope context ptr (the reference free-fn+global shape). [DONE] **Operator HL2 RX bench PASSED 2026-06-11.** |
| `wdsp\firmin.c` (filter cores) | (consumed via WDSP cffi)         | [N/A]     | Used internally by WDSP RXA chain via cdef + dlsym. No source-level port needed (the DLL ships with Lyra-cpp via `_native/` per the Python-project pattern; cdef + dlsym in `wdsp_native.cpp`). WISDOM cache fix shipped on Python side; lyra-cpp's RX chain inherits the same path. |
| `wdsp\wisdom.c`              | (consumed via WDSP cffi)           | [N/A]     | Same as firmin.c. |

### WDSP TX chain (per CLAUDE.md §4.1 port table)

| Source                           | Target (lyra-cpp)                              | Status | Notes |
|----------------------------------|------------------------------------------------|--------|-------|
| `wdsp\TXA.c` (channel scaffold)  | (consumed via `wire/wdspcalls` seam — P0.a/P0.d)| [DONE]     | **TxChannel class DELETED at P0.d `afc7950`** — the verbatim create_xmtr in `wire/CMaster.cpp` opens the WDSP TXA channel itself (OpenChannel/fexchange0/CloseChannel through the wdspcalls seam, cmaster.c:177-190 byte-exact), which removed the runtime-DLL RAII justification the class existed for. `wdsp_native.{h,cpp}` still carries the SetTXA*/GetTXAMeter cdefs for the operator-control surface. |
| `wdsp\bandpass.c` (bp0/bp1)      | (consumed via WDSP cffi — SetTXABandpassFreqs) | [N/A]     | The §15.23 trap (SetTXABandpassRun overrides bp1 to a stale state) is documented in CLAUDE.md + project_lyra_cpp_tx.md. cffi-only access is the correct posture. |
| `wdsp\compress.c` (TX compressor) | `src/wdsp/Compress.{h,cpp}` OR cffi binding   | [N/A]     | **Superseded by the native TX DSP rack (post-P4).** Operator chose the Lyra-native path: the Speech processor (#88, `902b814`/`93adb39`) + Combinator (#51) cover speech dynamics natively, pre-WDSP-TXA. No WDSP compress.c port/cffi binding shipped. |
| `wdsp\cfcomp.c` (5-band CFC)     | `src/wdsp/CFComp.{h,cpp}` OR cffi binding      | [N/A]     | **Superseded by the native Combinator #51** (`013bf12`/`61e16c4`/`b53ae35`) per the §15.19 plan — Lyra-native multiband compressor replaces the WDSP CFC. No CFC port shipped. |
| `wdsp\osctrl.c` (CESSB)          | cffi binding (SetTXAosctrlRun)                 | [PEND]     | Likely cffi-only; ~200 LOC port not justified unless operator UI need surfaces. |
| `wdsp\wcpagc.c` mode 5 (leveler) | cffi binding (SetTXALeveler*)                  | [DONE]     | cffi binding live in the wire-LIVE TXA chain. **TX LEV metering wired Thetis-format at #160 `9594bd5` (v0.3.1)** + #49 profile Leveler fields — addresses the v0.3.0 "UNVERIFIED" metering loose-end (shipped in code; #160 bench close-out / tester confirm still IN PROGRESS). |
| `wdsp\wcpagc.c` mode 5 (ALC)     | cffi binding (SetTXAALCMaxGain)                | [DONE]     | cffi present; SetTXAALCThresh does NOT exist (§15.23-class trap — only MaxGain governs ALC ceiling). **TX ALC metering wired Thetis-format at #160 `9594bd5` (v0.3.1)** — addresses the v0.3.0 "UNVERIFIED" flag (shipped in code; #160 bench close-out / tester confirm still IN PROGRESS). |
| `wdsp\eqp.c` (parametric EQ)     | `src/wdsp/EQ.{h,cpp}` OR cffi binding          | [N/A]     | **Superseded by the native EQ #50** (`edbaa52`/`17a28b7`/`eb92231`) — native RBJ-biquad 8-band parametric EQ (ParamEq engine + EESDR3-style panel), wired pre-WDSP-TXA. No WDSP eqp.c port shipped. |
| `wdsp\gen.c` (gen0/gen1 generators) | cffi binding (SetTXAPreGen*/SetTXAPostGen*) | [WIP]     | gen1 (postgen) live for TUN tone — wire-LIVE + bench-passed since P4.b 2026-06-13 (Thetis-exact zero-beat). gen0 (pregen) cdefs still deferred per §15.23 (bench-tooling nicety, not on the signal path) — hence row stays [WIP]. |

### TX PureSignal (per CLAUDE.md §4.1 v0.3)

| Source                       | Target (lyra-cpp)                              | Status | Notes |
|------------------------------|------------------------------------------------|--------|-------|
| `wdsp\iqc.{c,h}`             | `src/wdsp/Iqc.{h,cpp}` (~315 LOC)              | [PEND]     | v0.3. cffi math + Lyra-cpp wrapper for the 5-state PS lifecycle (RUN, BEGIN, SWAP, END, DONE). |
| `wdsp\calcc.{c,h}`           | `src/wdsp/Calcc.{h,cpp}` (~1164 LOC)           | [PEND]     | v0.3. cffi math + Lyra-cpp wrapper for the 8-state PS FSM + 3-state auto-attenuator FSM + PSDialog UI. |
| `wdsp\lmath.c::xbuilder`     | `src/wdsp/PsXBuilder.{h,cpp}` (~200 LOC)       | [PEND]     | v0.3. Cubic-spline coefficient builder, used by Lyra-cpp-side calcc orchestration. |
| `wdsp\delay.c`               | `src/wdsp/DelayLine.{h,cpp}` (~80 LOC)         | [PEND]     | v0.3. TX/feedback time-alignment. |

### Console (C# — study only, Lyra-native equivalents)

| Source                              | Target (lyra-cpp)                          | Status | Notes |
|-------------------------------------|--------------------------------------------|--------|-------|
| `Console\console.cs::chkMOX_CheckedChanged2` | `src/ptt.{h,cpp}` + `src/wdsp_engine.cpp` keydown/keyup | [DONE]     | Lyra-native FSM. Verified against the reference's keydown/keyup ordering invariants (RX-DSP stop on keydown -> wire MOX -> TX-DSP start; keyup TX-DSP off -> mox_delay -> clear MOX -> ptt_out_delay -> RX-DSP restart). **Live + operator-HL2-bench-passed at P4.b 2026-06-13** (SSB voice both sidebands + RF-safety kill-test exercise the FSM end-to-end). No direct port — C# is study-only by license. |
| `Console\console.cs` (TR delays)    | `src/tx/TrSequencing.{h,cpp}` (LIVE)       | [DONE]     | Lyra-native shipped with operator's Thetis-DB-verified defaults (mox=15/rf=50/space_mox=13/ptt_out=5/key_up=10 ms). |
| `Console\console.cs::m_bATTonTX`    | `src/hl2_stream.cpp` `fsmAdvance` + `compose_case_11` (LIVE wire) | [WIP]     | Lyra-native. Reference posture: keydown force step-att 31 (PS-A off OR CW); keyup restore. **WIRE path traced live + functional 2026-06-17:** `fsmAdvance` raises `setTxStepAttnDb(31)` on keydown (`hl2_stream.cpp:1651`) → `tx_step_attn = 31−31 = 0` → XmitBit-gated `compose_case_11` C4 = `0x40` → ak4951v4 cmd_addr 0x0a rx_gain = 0 = min LNA during TX. **BUT NOT [DONE]:** (a) the `src/tx/AttOnTxPolicy.{h,cpp}` class is **unused / wire-inert** (its own header says so) — the live mechanism is inline in hl2_stream.cpp, the earlier doc citation was wrong; (b) ~~operator-reported 2026-06-17: NO UI surface~~ → **UI surface BUILT 2026-06-17 (§15.31):** TxPanel "ATT" lamp in the Mic↔Tune gap (gray `ATT off` / orange `ATT 31` armed-RX / solid-red `ATT -31` engaged-TX, AUTO/TUN/MOX idiom) + Settings → TX → "ATT on TX (RX-ADC protection)" group (Enable + dB 0..31 spin); operator-gated, default ON/31, QSettings `tx/attOnTx*`; `fsmAdvance` now gates on the toggle + uses the value (replaces the hardcoded `kAttOnTxDb`). Operator-approved in review; **awaiting on-air bench confirm to flip [DONE].** The `AttOnTxPolicy` class stays unused (flag: delete-or-wire-up later). Broader **Task #114 still PENDING** (panadapter TX offset + PA-enable safety). |
| `Console\PSForm.cs`                 | `src/ui/PsDialog.{h,cpp}` (NEW, v0.3)      | [PEND]     | v0.3 PureSignal UI. C# study only; write Qt/QML native. |
| `Console\HPSDR\IoBoardHl2.cs`       | `src/hl2_stream.cpp` (LIVE)                | [DONE]     | HL2 I/O quirks implemented Lyra-native via the protocol-byte verification + gateware-RTL ground-truth pass (CLAUDE.md §15.26). Live + bench-passed across the RX + P4.b TX chain (incl. on-air FT8). |

---

## STAGE INDEX — ordered work plan

### P0 verbatim-rewrite arc (2026-06-09 -> current; the ACTIVE plan)

After the operator rejected the 2026-06-09 "rule-#8" retrofit
deviations ("I said PORT!"), every TX-ported file is being
rewritten VERBATIM (byte-faithful; only documented packaging
differences — `lyra::wire` namespace, PORT-expands-empty, the
wdspcalls seam for the statically-linked WDSP exports).  Each
step ships with a mechanical diff vs the reference + an operator
HL2 bench gate.

| Step | Status | What | Commit / gate |
|------|--------|------|---------------|
| **P0.a** | [DONE] DONE | `wire/wdspcalls.{h,cpp}` — the single approved WDSP linkage seam (exact export names, PS entry family complete) | `e0c0f4a` 2026-06-09 |
| **P0.b** | [DONE] DONE | ilv.{h,c} verbatim -> `wire/ILV.{h,cpp}` + new `wire/cmcomm.{h,cpp}` | `1f49d98` 2026-06-11; test_ilv ALL PASS |
| **P0.c** | [DONE] DONE | aamix.{h,c} verbatim -> `wire/AAMix.{h,cpp}` + `wire/resample.h` RESAMPLE ABI | `e0f2584`; [DONE] operator HL2 RX bench PASSED 2026-06-11 |
| **P0.d** | [DONE] DONE | cmbuffs/cmaster/cmsetup verbatim -> `wire/CmBuffs`/`wire/CMaster`/`wire/cmsetup`; full `_cmaster` struct (PS surface); TxChannel carve-out DELETED | `afc7950` 2026-06-12; [DONE] **operator HL2 RX-regression bench PASSED 2026-06-12** |
| **P1**  | [DONE] | obbuffs.c verbatim -> `wire/ObBuffs.{h,cpp}` (the TX-out seam; separate TU from cmbuffs; sendOutbound call restored at P2.c) | `75754c2` 2026-06-12; shipped dormant, then **wire-LIVE at P4.b 2026-06-13** (create_obbuffs/ob_main live in the direct-port TX chain); clean build 0 warnings; mechanical diff IDENTICAL |
| **P2**  | [DONE]      | sendOutbound / sendProtocol1Samples fidelity audit of the dormant wire layer — **COMPLETE 2026-06-12** (P2.a eer completion + P2.b prn outbound surface verbatim + P2.c wire/Network.cpp sendOutbound verbatim, mechanical diff IDENTICAL; the ObBuffs ob_main hand-off restored).  **P2.a SHIPPED 2026-06-12** — eer completion (verbatim struct + seam entries + the four CMaster restore sites); kills the null-`peer` landmine both reference functions deref.  NEXT: P2.b prn outbound surface verbatim (outLRbufp/outIQbufp/OutBufp -> reference pointer fields + the HANDLE semaphore quartet; callers bend) -> P2.c wire/Network.cpp sendOutbound verbatim (P1 branch live, ETH branch DEFERRED) + restore the ObBuffs ob_main call. | Audit finding: Lyra's Step-14 OutboundRing/Ep2SendThread are functionally-parallel idiom translations (vector buffers, bool+cv semaphores) predating the verbatim mandate — retired at P4 when the verbatim chain goes live. |
| **P3**  | [DONE]      | netInterface registrations + obbuffs ring lifecycle.  **SHIPPED + operator HL2 bench PASSED 2026-06-12** (RX fine; stop/start + shutdown clean) — create_rnet moved to once-per-process at the main.cpp QTimer AFTER create_xmtr (the reference C#-init order; fixes the per-open prn re-allocation leak); SendpOutboundTx(OutBound) restored at the reference site (netInterface.c:1761, tail of create_rnet); UpdateRadioProtocolSampleSize verbatim (netInterface.c:1836-1858, diff IDENTICAL) called per-open per StartAudio:45 — creates obbuffs rings 0/1 (2 new ob_main pump threads per session, idle: no producers until P4); destroy_obbuffs(0/1) at close() per StopAudio:112-113.  RX side deliberately NOT registered (WdspEngine hybrid owns RX dispatch until P4 — the B.6.b clobber class). | [DONE] Operator HL2 bench PASSED 2026-06-12. |
| **P4**  | [DONE]      | Wire-LIVE switchover — **COMPLETE + operator HL2 bench PASSED 2026-06-13.**  P4.a prep (2 wire-inert commits): sendProtocol1Samples verbatim in src/wire/NetworkProto1.cpp + io_keep_running + FrameComposer tail flipped to ReleaseSemaphore(hobbuffsRun) + SetTXAPostGen wdspcalls + TxReadBufp double* / hWriteThreadMain HANDLE.  **P4.b SHIPPED** in 3 commits: `fb9ec41` Wire-LIVE RX-out gate, `0b551b4` first modulated RF through the direct-port chain, `9db2c50` SSB sideband fix (USB transmits USB, 9100-verified).  Plus `84dbb12` TUN panadapter-spike fix (Thetis-exact zero-beat confirmed, display-only bug).  **§7 retirements DONE** (`fe6a438` OutboundRing+Ep2SendThread, `99fd24a` txWorkerLoop+composeCC+keepalive, `e711256` old HL2-jack EP2 ring).  **TCI digital TX re-home** `bd61d07` (TciTxBridge fills the §10.3 skeleton; on-air FT8 QSOs).  Released v0.2.3. | Bench gates PASSED: SSB voice both sidebands + RF-safety kill-test + MSHV→TCI→FT8 on-air. |

PureSignal is a committed feature — the P0.d `_cmaster` struct
carries the full PS surface so v0.3 lands on top without a
retrofit.

### Historical stage index (pre-P0 lettered stages — superseded)

The original lettered plan below is retained for history.  The
P0 arc absorbed/superseded it: Stage C (ilv) = P0.b; Stage D
(xcmaster) = P0.d; Stage E (TxChannel audit) = obsoleted by the
P0.d TxChannel deletion; Stage B's idiom-translated AAMix was
re-ported verbatim at P0.c.

| Stage | Status | What                                              | Notes |
|-------|--------|---------------------------------------------------|-------|
| **A** | [DONE] DONE | CMaster.cpp wire-up stubs (Send*Outbound* + SetTCIRun) | Superseded by the P0.d full verbatim CMaster port. |
| **B** | [DONE] DONE | aamix.c direct port + wire into RX path           | Shipped 2026-06-08 (idiom-translated); re-ported VERBATIM at P0.c. |
| **C** | [DONE] DONE (as P0.b) | ilv.c direct port (TX I/Q interleaver)   | Landed verbatim at `wire/ILV.{h,cpp}`. |
| **D** | [DONE] DONE (as P0.d) | xcmaster pump body direct port           | Landed verbatim in `wire/CMaster.cpp` (real stype/txid/chid + update[] CS). |
| **E** | [N/A] OBSOLETE | TxChannel reference-parity audit + reconciliation | TxChannel DELETED at P0.d — the verbatim create_xmtr replaces it. |
| **F** | [PEND] TBD | WDSP TXA chain components per CLAUDE.md §4.1 v0.2.1 | compress / cfcomp / eqp / wcpagc — operator-driven per-feature direct port vs cffi-only call. Order TBD. |
| **G** | [PEND] TBD | PureSignal direct ports (v0.3)                    | iqc / calcc / xbuilder / delay. Lands when v0.3 PS work starts. Needs operator HL2+ with the PS hardware mod. |
| **H** | [PEND] TBD | TX/PS auto-attenuator FSM port                    | C# study only (PSForm.cs); Lyra-native rewrite. |

**Ordering note:** the P0->P4 sequence above is the active linear
plan.  F–H land after P4 Wire-LIVE.  The methodology (smallest
revertable step + mechanical diff + operator bench gate) applies
within each step.

---

## OPEN QUESTIONS FOR OPERATOR

These are decisions that shape the plan but I cannot make
unilaterally — surface them when picking up tomorrow:

1. **Stage C (ilv.c) vs Stage D (xcmaster) vs Stage E
   (TxChannel audit) — which first?**  All three are
   defensible:
   - Stage C unlocks reference-faithful TX I/Q flow.
   - Stage D removes the Lyra-native pump (cleaner end-state).
   - Stage E formalizes what's already shipped (lowest risk,
     highest confidence-gain).

2. **Do we re-port networkproto1.c + network.c + the Step 14
   wire pieces direct from reference, OR leave them as the
   current mix of Lyra-native rewrites + original `hl2_stream.cpp`?**
   The Step 14 plan was midway through these when Stage B
   started.  Direct-port would replace the rewrites with
   reference-verbatim ports.  Operator call.

3. **WDSP TXA-chain components — direct port (~150-600 LOC
   each) vs cffi-only?**  The DLL exports work; cffi is faster
   to ship.  Direct port gives Lyra-cpp full source visibility
   + lets us patch behavior at the source level.  Per-component
   operator decision; default = cffi-only unless a real source-
   level need surfaces.

4. **PS work timing — start v0.3 (iqc/calcc/etc.) now in
   parallel with TX, or sequence after TX is fully done?**
   The CLAUDE.md plan defers PS to v0.3 (after v0.2 TX
   release).  Operator may want to start the ports early.

5. **The §15.27 "opt-in toggles that become defaults" cleanup
   from the Python project — does it have a lyra-cpp analog
   here?**  The Python project had a long backlog of legacy
   toggles to retire.  Lyra-cpp doesn't carry the same legacy,
   but new direct-port commits may obsolete some Settings
   surfaces operator-knows-about — track per-component.

---

## SHIPPED-WORK LOG

Newest at top.  Cross-references SESSION_LOG.md for full
arc details.

### 2026-06-14 -> 2026-06-17 — post-P4: VAC v0.3.0/v0.3.1, native TX DSP rack, Profiles, TX monitor (#90), Out picker

These are post-direct-port work: native (Lyra-side) TX-DSP /
host-audio / profile features that land ON TOP of the verbatim
P0–P4 chain.  They are NOT direct ports (no Thetis ChannelMaster/
WDSP source counterpart) — logged here as a post-P4 section so
the P0–P4 port-stage record above stays intact.

- **VAC / IVAC (#102/#158)** — PC/VAC mic-input + RX-to-PC path.
  `ivac.c` -> `wire/Ivac` direct port (Stage 0/1 `4831db7`/`52adff3`),
  #158 complete (RX decode + TX on-air + auto-start + profile VAC
  source) `6c3fecb`.  Released **v0.2.4** "Profiles + VAC" `c48c94d`;
  VAC mic mono-combine fix `b57bd09` -> **v0.2.5** `f55da2c`.
  Device-layer rework DL-0..DL-5 (PortAudio 19.7.0 vendored static,
  single full-duplex stream, Thetis-faithful host-API/device
  pickers, RX-muted-on-TX via SetIVACmox, old Qt two-stream
  `ivac_audio` retired): `11812f9`..`e8d0239`; plus two crash fixes
  — VAC-teardown UAF `f0c6497`, TCI dangling-owner `d508b8e`.
  TX MIC/ALC/LVL meters re-homed onto wire-live `GetTXAMeter`
  `993e10e`.  Released **v0.3.0** `accee55`.  Then **v0.3.1**
  `a3a517f`: VAC enable/disable crash fix + Vol/Mute routed to VAC
  `77dc55d` (#161) + Thetis-format TX MIC/ALC/LEV metering + #49
  profile Leveler/ALC fields `9594bd5` (#160).  v0.3.1 addresses the
  v0.3.0-banner "TX metering UNVERIFIED" loose-end in code (#160
  bench close-out / Brent+Timmy field-confirm still in progress).
  Interim VAC-UI
  fixes (`88bdab1` relabel, `b57bd09` combine, `a01d308`
  output-"(none)") shipped along the way.

- **Stage F — native TX DSP rack (pre-WDSP-TXA mic rack):**
  - **EQ #50** — native ParamEq (RBJ biquad cascade) `edbaa52`;
    EqModel + EESDR3-style panel `17a28b7`; wired into the live TX
    mic rack `eb92231`.
  - **Digital-mode auto-bypass** of the native mic DSP (DIGU/DIGL)
    `c8441a6`.
  - **Speech #88** — native SpeechProcessor (Auto-AGC leveller +
    de-esser) `902b814`; Speech panel + EQ analyzer + Reset-All
    `93adb39`.
  - **Combinator #51** — native engine `013bf12`; model+panel+dock
    `61e16c4`; wired into the live rack after EQ `b53ae35`.
  - **Plate #52** — native Schroeder–Moorer engine `ae9c5d3`;
    PlateModel + "Plating" panel `0cfd7e5`; wired after Combinator
    `e96a6e8`.
  - Rack-lamp dim in digital/CW + EQ analyzer recolor (#163)
    `9be1fc1` / `558d5d9`.

- **Profiles #49 (TX/RX profile manager):** rack
  saveState/loadState round-trip `e0f264e` (Stage 1); schema v3
  bundling the native rack blobs `b453320` (Stage 2); rack wired
  into profile capture/apply `29f9b92` (Stage 3).  Sideband- and
  band-agnostic SSB profile `296cafc`.  Panel-lock float-drag fix
  `a82eb77` (exclude TX rack panels from LOCK) + lock also blocks
  resize/custom-float `cf976c1`.  TX Speech profile capture +
  toggle-lamp refresh on recall (#162) `c17960c` / `46a428f`.

- **TX audio monitor #90** — Route 1 (MON button + Monitor slider,
  HL2 jack) `6e6a282`; Routes 2+3 (VAC device + TCI RX-audio)
  `e1fd186`.  Reference-verified against Thetis audio.cs.

- **#164** — inline Audio-panel "Out" output-device picker wired +
  MON->MON-TX relabel `740f769`.

- USER_GUIDE catch-up commits (minor): `66adc5e`, `266d8cd`,
  `dc45485`.

### 2026-06-13 — P4 Wire-LIVE switchover + v0.2.3 release ([DONE] HL2 bench PASSED)

- **P4.a prep** (wire-inert): sendProtocol1Samples verbatim
  `a4aa8c3` + SetTXAPostGen/TxReadBufp/hWriteThreadMain field types
  `baef866`.
- **P4.b SHIPPED** (3 commits, operator HL2 bench PASSED 2026-06-13):
  `fb9ec41` Wire-LIVE RX-out gate, `0b551b4` first modulated RF
  through the direct-port chain, `9db2c50` SSB sideband fix (USB
  transmits USB, 9100-verified).  Plus `84dbb12` TUN
  panadapter-spike fix (display-only, Thetis-exact zero-beat).
- **§7 retirements DONE:** `fe6a438` OutboundRing+Ep2SendThread,
  `99fd24a` txWorkerLoop+composeCC+keepalive, `e711256` old
  HL2-jack EP2 ring.
- **TCI digital TX re-home** `bd61d07` (first FT8 QSOs via MSHV/TCI
  through the verbatim chain) + space-bar PTT toggle `2824ebf`
  (#157).
- Released **v0.2.3** "Transmit (SSB + digital TCI) on air"
  `e8aafbc` (tags `v0.2.3` + `tx-working-2026-06-13`).

### 2026-06-12 — P0.d cmbuffs/cmaster/cmsetup VERBATIM port ([DONE] HL2 bench PASSED)

- **P0.d `afc7950`** on `tx-rebuild`: NEW `wire/cmsetup.{h,cpp}`
  (cmMAX* 16/4/4/2/32 + the 8 id helpers + SetRadioStructure +
  set_cmdefault_rates); `wire/CmBuffs.{h,cpp}` rewritten verbatim
  (`cmb,*CMB`, malloc0/_aligned_free; Phase-C retrofit deviations
  removed); `wire/CMaster.{h,cpp}` rewritten verbatim (FULL
  `_cmaster` struct incl. the PureSignal surface, `enum
  AudioCODEC`, raw TCI fn ptrs, verbatim create/destroy_cmaster +
  create/destroy_xmtr + xcmaster + all Sendp*/Set* setters).
- **TxChannel RAII carve-out DELETED** (`src/wdsp/TxChannel.{h,cpp}`
  retired) — the verbatim create_xmtr opens the WDSP TXA channel +
  TX analyzer (disp 1) + out[0..2] + ILV itself through the
  wdspcalls seam.
- main.cpp: SetRadioStructure(2,1,1,1,0,…) config before
  create_cmaster (chid(0,0)=0 RX1 / chid(1,0)=1 TX = the live
  layout); create_xmtr() post-resolve_wdsp_calls (documented
  DEFERRED-CALLSITE accommodation); handler-1.5 = gated
  destroy_xmtr().
- Verification: clean build ZERO warnings; test_ilv ALL PASS;
  mechanical diffs IDENTICAL (CmBuffs.{h,cpp} / cmsetup.h /
  CMaster.h / all 10 fully-verbatim cmaster.c functions) +
  live-line-subset PASS for the partially-deferred bodies (sole
  accommodation: `(char*)""` at XCreateAnalyzer).
- New at startup: TWO cm_main pump threads (streams 0+1; stream 0
  idles — no Inbound producer); TX stays wire-quiescent
  (OutboundTx null until P3).
- [DONE] **GATE PASSED 2026-06-12: operator HL2 RX-regression bench**
  ("RX still working") — P1 (obbuffs.c) is unblocked.

### 2026-06-09 -> 2026-06-11 — P0.a/P0.b/P0.c verbatim-rewrite arc

- Operator rejected the 2026-06-09 "rule-#8" retrofit deviations
  ("I said PORT!") -> every ported file rewritten VERBATIM.
- **P0.a `e0c0f4a`**: `wire/wdspcalls.{h,cpp}` — the single
  approved WDSP linkage seam (fn-ptr table, exact export names,
  PureSignal entry family complete + dumpbin-verified).
  `scratch/_typedef_fidelity_test.cpp` disproved the claimed
  "C++ collisions" (twin typedef / `complex[2]` / raw fn ptr all
  compile verbatim).
- **P0.b `1f49d98`**: ilv.{h,c} verbatim -> `wire/ILV.{h,cpp}`;
  new `wire/cmcomm.{h,cpp}` (complex/PORT/malloc0/PI); pilv[]
  bank gone; SendpOutboundTx raw fn ptr (register AFTER
  create_xmtr — no null guards, reference ordering).  Mechanical
  diff whitespace-only; test_ilv ALL PASS.
- **P0.c `e0f2584`**: aamix.{h,c} verbatim -> `wire/AAMix.{h,cpp}`
  (supersedes the Stage-B idiom translation); `wire/resample.h` =
  public RESAMPLE ABI; wdspcalls retyped + flush_resample;
  WdspEngine outbound lambda -> static fn + TU-scope context ptr.
  [DONE] **Operator HL2 RX bench PASSED 2026-06-11.**

### 2026-06-08 EOD — Stage B aamix.c port COMPLETE

- **Stage B (aamix.c)** [DONE] direct port shipped + bench-validated
- Commits `27ac2fa` -> `7a50b07` on `tx-rebuild`
- Operator HL2+ bench 19:27:49 -> 19:28:47: audible RX through
  both AK4951 jack and PC-Soundcard sinks; mute/unmute via
  dispatch; output-device swap; AGC mode changes; chain
  out=N tracks 1:1 with dispatch calls=N across both sinks
- B.6.b root cause: Stage-A SendpOutboundRx no-op stub at
  `RadioNet.cpp:272` clobbered `aaMix_->Outbound` ~1 ms after
  `WdspEngine::openRx1` wired the real lambda.  Fixed by
  deleting the stub (commit `8b8e0da`).
- 2-stage diagnostic instrumentation (chain @ `225518c` +
  dispatch @ `12369bc`) localized the defect to a single line
  of code on the 2nd bench — pure-speculation cost avoided.
- Methodology lessons locked into this doc § "LOCKED
  METHODOLOGY" above (counters-first when symptom is "nothing
  happens"; operator-empirical rule held; "do as Thetis does"
  continues to pay).
- Tasks #130 (parent) + #132 (Stage B.6.b) closed.
- Backup `_backups/lyra-cpp-2026-06-08-aamix-stage-B-COMPLETE-with-docs.bundle`.

### Earlier work

See `SESSION_LOG.md` for the full session-by-session log.
The Step 14 wire-layer arc (Stages 1, 1.5, 2a, 2b1, 2b2) is
covered in detail there + in `EXECUTION_PLAN.md`'s progress-
tracking sections.  TX-1 components 2-6 (`main` branch) are
covered in `project_lyra_cpp_tx.md` (memory file).

---

## RESUME POINTER (next session)

**First actions (locked discipline):**
1. `git fetch origin`
2. `git status` + `git log --oneline -5 tx-rebuild origin/main`
3. Read this doc + the "READ FIRST" block in
   `project_lyra_cpp_tx.md`
4. Read `SESSION_LOG.md` newest entry for arc detail

**Branch posture (RESOLVED 2026-06-17):** `main` is the working trunk again
— `main` == `origin/main` == `tx-rebuild` == the **v0.4.0** release commit.
Operator consolidated onto `main` (TX largely done) to stop forgetting to
fast-forward it. Do future work on `main`; `tx-rebuild` is a legacy ref.

**SHIPPED since the last revision of this pointer:** the AM/DSB one-sided
bug is **FIXED** (it was the SSB-only TX modulator, not an RX-DSP bug — RX
was correct; the one-sided signal was the operator's own transmit) →
**native AM/DSB/SAM/FM transmit + AM carrier level** shipped (#106/#93).
Plus operator-visible ATT-on-TX (§15.31) and a UI-readability batch.
**Released v0.4.0** (33 commits since v0.3.1; installer published to GitHub).

**CW transmit (#105) — ON AIR 2026-06-18 (CW-2/CW-3a/CW-3b/CW-4 SHIPPED).**
Design `docs/architecture/cw_tx_design.md`; software-keyer design
`docs/architecture/cw3_software_keyer_design.md`. HL2 CW is **100 %
gateware-keyed** (confirmed vs radio.v/dsopenhpsdr1.v) — no host DSP/WDSP
carrier; host sends key-state bits, the verbatim EP2 packer
(`NetworkProto1.cpp:99-108`) carries them, gated by `cw_enable` which also
enables the gateware CWX decode (`cmd_data[24]` = the 0x0f C1 bit).
- **CW-2 paddle** (gateware iambic) — SHIPPED + operator-confirmed;
  carrier-on-marker, QSK/Semi/Manual break-in, CW MON sidetone, foot-switch.
- **CW-3a/3b** host software keyer (CWX) + chip→floating CW console —
  SHIPPED `8904749`. `tx/CwMorse` (PARIS + operator weight) + `tx/CwKeyer`
  (dedicated timer thread) + HL2Stream `sendCw`/`abortCw`/`setCwx*` +
  `qml/CwConsolePanel`. Host drives `tx[0].cwx`/`cwx_ptt`; gateware keys.
- **CW-4** CW-over-TCI — SHIPPED `fcfcb6b`, verified vs the **EESDR TCI
  manual** (`docs/TCI Protocol.pdf` §3.2) + Thetis TCIServer.cs +
  SDRLogger+. `cw_macros` / `cw_msg`(+`$N`) / `cw_macros_speed` /
  `cw_macros_stop` → the CW-3 keyer; reserved-char un-escape; CW-mode-only.
- The earlier **0x0b register-collision** worry was a non-issue: `cw_enable`
  (0x0f C1) is what arms both sides; the step-att path is separate.
- **NEXT — CW-5 (#173):** RX CW decoder (faithful port of SDRLogger+'s
  Bayesian/AFC/Farnsworth decoder) into the console's reserved pane +
  macros field. Plus CW follow-ons (Semi/Manual host-MOX for CWX, F-key
  memories, CWFWKeyer toggle; TCI cw_terminal/cw_macros_delay/macro syntax).

Other open major arcs (unchanged): **RX2 + Split** (#96–#101) and
**PureSignal** (Stage G/H). Smaller items: VOX (#91), voice keyer (#89),
DSP buffer/latency options (#159), FM operator-knob refinement (#107),
parked #156 (restart-after-hard-kill, intermittent).

---

*Last updated: 2026-06-18 — **CW TX ON AIR (#105)**: CW-2 paddle (gateware
iambic) + CW-3a/3b host software keyer (CWX) & chip→floating CW console
(`8904749`) + CW-4 CW-over-TCI (`fcfcb6b`, EESDR-spec-verified) + USER_GUIDE
CW section (`2a09824`), all on `main`/`origin/main`.  NEXT = CW-5 (#173) RX
CW decoder.  Prior (2026-06-17): v0.4.0 RELEASED (AM/DSB/FM transmit + AM
carrier #106/#93, native TX DSP rack, Profiles #49, TX monitor #90,
ATT-on-TX §15.31).  RX2/Split + PureSignal remain the next major arcs.*
