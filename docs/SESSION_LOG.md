# lyra-cpp — Session Log

Running EOD log. Newest entry on top. Short rough-outline format.

---

## 2026-06-12 (Friday PM-3 — P2 audit complete + P2.a eer completion SHIPPED)

**Branch:** `tx-rebuild`, on top of `2897756`.

### P2 fidelity audit (the deliverable that scopes P2.b/c/P4)
- **Reference TX-out chain (HL2 P1), fully read:** `ob_main` -> `sendOutbound` (network.c:1237; P1 branch :1285-1340) — id 1 memcpys 126 complex into `prn->outIQbufp` + Release(hsendIQSem) + Wait(hobbuffsRun[0]); id 0 -> `outLRbufp` + hsendLRSem + hobbuffsRun[1] -> `sendProtocol1Samples` thread (networkproto1.c:1204-1267): WaitForMultipleObjects(both, TRUE) -> eer overwrite if `peer->run && XmitBit` -> `!XmitBit => memset outIQbufp` -> swap_audio_channels -> 16-bit BE quantize + HL2 CW-bit overlay into `OutBufp` -> WriteMainLoop_HL2 -> MetisWriteFrame -> Release both hobbuffsRun.  Resources: bufs calloc'd netInterface.c:1606-1608; semaphore quartet created in StartAudio :68-71; obbuffs rings 0/1 created in UpdateRadioProtocolSampleSize :1856-1857; eer created run=0 per xmtr (cmaster.c:212-224).
- **Lyra dormant Step-14 surface:** functionally-parallel idiom translations predating the verbatim mandate — prn buffers are std::vector (RadioNet.h:552-554), the semaphore quartet became OutboundRing's bool+cv, Ep2SendThread is the sendProtocol1Samples rewrite.  `XmitBit` is already verbatim (extern int, RadioNet.h:733).  Disposition: re-port verbatim, retire the translations at P4 Wire-LIVE.
- **Landmine found:** BOTH reference functions deref `pcm->xmtr[0].peer->run`, but P0.d had deferred the eer creation — `peer` was NULL.  That made P2.a unambiguous and first.

### P2.a SHIPPED — eer completion (verbatim)
- `wire/cmcomm.h`: opaque `eer` typedef replaced by the FULL verbatim struct (wdsp/eer.h:30-49, mechanical diff IDENTICAL); opaque `DELAY` twin typedef added (wdsp/delay.h:32-59).
- `wire/wdspcalls.{h,cpp}`: +5 seam entries — create_eer / destroy_eer / xeer / pSetEERSize / pSetEERSamplerate (signatures verbatim from eer.h:51-75; all five verified present in the bundled wdsp.dll via PE export-table scan).
- `wire/CMaster.cpp`: all four deferred eer sites RESTORED verbatim — create_xmtr `create_eer(run=0, ...)` 12-arg block (cmaster.c:212-224); destroy_xmtr `destroy_eer` (:262, after destroy_ilv per reference order); xcmaster `xeer(pcm->xmtr[tx].peer)` (no-op at run=0 with in==out — verified in eer.c:86-122); SetXmtrChannelOutrate pSetEERSamplerate/pSetEERSize.  Restored-lines verbatim-subset check PASS (18 lines).
- Runtime impact: create_xmtr now also creates/destroys one run=0 eer object per xmtr (heap + CS only); xeer in the TX pump body never executes today (stream-1 pump has no producer).  RX path untouched.  Clean build, zero warnings.

### P2.b SHIPPED (same session) — prn outbound surface verbatim
- `outLRbufp`/`outIQbufp`/`OutBufp` std::vector -> the VERBATIM reference pointer fields (network.h:64-66) + the verbatim calloc set at create_rnet (netInterface.c:1606-1608, mechanical subset PASS).  Lifetime = process (create_rnet call-once) = reference.
- Verbatim HANDLE quartet declared on prn (hsendLRSem/hsendIQSem/hsendEventHandles[2]/hobbuffsRun[2], network.h:92-95) — DORMANT/nullptr until P4's StartAudio equivalent; the Step-14 cv+flags translation stays alongside until P4 retires its consumers.
- Callers bent: OutboundRing `.data()` ×6 -> raw; Ep2SendThread pack casts -> char; `write_main_loop_hl2` parameter -> `const char*` (reference type).  Wire-inert (only create_rnet's allocation runs live).  The lone C4456 in hl2_stream.cpp:776 is PRE-EXISTING (file untouched; surfaced by the header-triggered recompile) — flagged for a separate cleanup.

### P2.c SHIPPED (same session) — sendOutbound verbatim + ob_main restore
- NEW `src/wire/Network.cpp` — network.c PARTIAL direct port: `sendOutbound` verbatim (network.c:1237-1341).  P1 branch LIVE (the outLRbufp/outIQbufp memcpy + hsendLRSem/hsendIQSem release + hobbuffsRun wait handshake, incl. the EER de-interleave sub-branch with its function-local static ptr); ETH branch carried as DEFERRED reference text (Protocol 2/ANAN = v0.4; WriteUDPFrame/udpOUT unported).  Accommodations, each documented inline: the RadioNet.h enum-class mapping on the protocol gate; suppress 4101 (ETH-only locals) + 4456 (the reference's own loop-scope `i` shadowing its function-scope `i`, network.c:1239 vs :1298).  Mechanical diff vs network.c:1237-1341 = IDENTICAL (whitespace-normalized, deferred text restored).
- `wire/ObBuffs.cpp` ob_main: the DEFERRED `sendOutbound(id, a->out)` hand-off RESTORED verbatim — the P1 TU is now reference-complete.
- Still dormant end-to-end: no create_obbuffs caller until P3/P4; the semaphore quartet is created by P4's StartAudio equivalent before the pump can deliver, the same ordering the reference relies on.  Clean build, zero warnings.

### P3 SHIPPED (same session; survived + re-verified after an operator power outage mid-arc) — netInterface registrations + obbuffs ring lifecycle
- **create_rnet moved to once-per-process** at the main.cpp QTimer block AFTER create_xmtr — the reference's C#-init ordering (create_cmaster -> create_xmtr -> create_rnet -> StartAudio).  The old per-open call re-allocated prn on EVERY stop/start (leak + wire-state reset, contradicting the close() "prn stays alive for re-open" contract — a latent pre-existing defect the P3 reference-read surfaced).  open() now asserts the reference prn-non-null contract (netInterface.c:40); if wdsp.dll failed to load, open() refuses with a qCritical (no WDSP = no radio, the reference posture).
- **SendpOutboundTx(OutBound) restored at the reference site** (netInterface.c:1761, tail of create_rnet) — pcm->OutboundTx -> xmtr[0].pilv via the verbatim no-guard SetILVOutputPointer; ordering safe by construction.  The Stage C.3-fix "registration belongs at the xmtr-open site" note SUPERSEDED (it guarded a Step-14 stub against per-open clobber; under the direct port the reference site is correct).  RX side deliberately NOT registered — WdspEngine's openRx1 owns RX dispatch in the approved hybrid until P4 (registering RX here = the B.6.b clobber = dead RX audio).
- **UpdateRadioProtocolSampleSize ported verbatim** (netInterface.c:1836-1858, mechanical diff IDENTICAL with the documented kMax*/enum-class accommodations) — called per session-open from HL2Stream::open() at the StartAudio position (:45): per-protocol spp values (USB: mic 63 / rx 63 / tx 126 / audio 126) + create_obbuffs(0/1) — TWO new ob_main pump threads per session, both idle (ring 0 has no producer until P4's RX switchover; ring 1's producer is xilv in the stream-1 cm pump, quiescent until P4 feeds Inbound(1,...)).  destroy_obbuffs(0/1) at close() per StopAudio (:112-113).
- Power-outage recheck: all 4 edited files probe-verified intact, diff-vs-HEAD = exactly the P3 hunks, forced rebuild of the touched TUs clean (sole warning = the pre-existing hl2_stream C4456), mechanical diffs re-PASSED post-outage.
- **OPERATOR HL2 BENCH GATE (pending)**: RX works; multiple stop/start cycles clean (rings recreated/destroyed per session); clean app shutdown; watch the startup log for the new `[wire] P3: create_rnet()` line firing ONCE.

### P2 COMPLETE.  NEXT
1. **P3** — netInterface outbound registrations: `SendpOutboundRx(OutBound)` / `SendpOutboundTx(OutBound)` per netInterface.c:1749-1761 — MUST register AFTER create_xmtr (verbatim setters have NO null guards); plus the create_obbuffs(0/1) sites (netInterface.c:1856-1857) at the Lyra equivalent of UpdateRadioProtocolSampleSize.
2. **P4** — Wire-LIVE switchover (one commit, full HL2 bench gate): verbatim sendProtocol1Samples thread + the semaphore-quartet creation + WriteMainLoop_HL2 hand-off; retires the OutboundRing/Ep2SendThread cv translations.

---

## 2026-06-12 (Friday PM — P1 obbuffs.c verbatim direct port SHIPPED)

**Branch:** `tx-rebuild`, on top of `976062d` (the P0.d bench-PASS doc flip).

### Shipped (one commit)
- **NEW `src/wire/ObBuffs.{h,cpp}`** — reference obbuffs.{h,c} verbatim: `obb,*OBB` twin typedef + numRings/obMAXSIZE/OBB_MULT defines + the file-scope `_obpointers obp` four-alias bank + create/destroy/flush/OutBound/obdata/ob_main/SetOBRingOutsize.  The TX-OUT seam (WDSP output → ring → ob_main pump thread → sendOutbound → protocol packers); SEPARATE TU from cmbuffs (inbound-only).
- The reference's own **2014-era idioms kept as shipped** (deliberately NOT harmonized to the cmbuffs sibling): calloc/free (not malloc0/_aligned_free), destroy-time `obp.pcbuff[0]==NULL` guard, obdata WITHOUT the MW0LGE CS wrap, the csOUT enter/leave pair at the top of the ob_main loop, the `out` work buffer inside the struct.
- **Sole deferred line:** `sendOutbound(id, a->out);` in ob_main — the reference defines it at network.c:1237 (the P1 branch :1285-1340 is the outIQbufp/outLRbufp + hsendIQSem/hsendLRSem/hobbuffsRun handshake WriteMainLoop_HL2 consumes) = **P2 scope**.  Carried in place as reference text with a DEFERRED tag; decl in ObBuffs.h verbatim.
- Provenance check resolved before code: the "OutBound at network.c:1285-1340" citation in older RadioNet.cpp comments is the PROTOCOL_1 **branch of sendOutbound**, not a second OutBound — obbuffs.c's OutBound is the one registered via SendpOutboundRx/Tx (P3).
- Compiler accommodations: same set as CmBuffs.cpp (suppress 4312/4189/4311/4302 on the thread-arg casts + `(AVRT_PRIORITY) 2`), each marked inline.

### Verification
- Clean build, ZERO warnings.
- Mechanical diff vs reference: **ObBuffs.cpp IDENTICAL** (code lines, whitespace-normalized); ObBuffs.h sole delta = the reference's include-guard `#define _obbuffs_h`, replaced by `#pragma once` (documented packaging difference, same as CmBuffs.h).  Live-line-subset PASS on both.

### Wire impact
- **NONE — the TU is dormant.**  No Lyra caller of create_obbuffs exists until P3/P4 (the reference creates rings 0/1 in netInterface.c:1856-1857).  No new threads at startup, wire bytes unchanged.  An operator RX smoke at convenience is prudent (binary changed) but this touches no live path.

### NEXT
1. **P2 = sendOutbound / sendProtocol1Samples fidelity audit** of the dormant wire layer (reconcile network.c:1237-1341 + networkproto1.c sendProtocol1Samples vs Lyra's Step-14-era OutboundRing/MetisFrame/Ep2SendThread surface), then restore the ob_main hand-off verbatim.
2. P3 netInterface registration (SendpOutboundRx/Tx(OutBound) — AFTER create_xmtr, the verbatim setters have NO null guards) → P4 Wire-LIVE (one commit, full HL2 bench gate).

---

## 2026-06-12 (Friday — P0.d CmBuffs/CMaster/cmsetup verbatim direct port SHIPPED + ✅ HL2 RX-regression bench PASSED)

**Branch:** `tx-rebuild` HEAD = `afc7950`, pushed to `origin/tx-rebuild`.

**✅ OPERATOR HL2 BENCH (same day): RX-regression gate PASSED** ("RX still working") — clean RX on the new per-stream cmbuffs pump/ring layout (two cm_main threads, stream 0 idle by design).  P0.d is fully closed; **P1 (obbuffs.c verbatim port) is UNBLOCKED** and starts next session.

### Shipped (one commit, `afc7950`)
- **NEW `src/wire/cmsetup.{h,cpp}`** — reference cmsetup.{h,c} verbatim: cmMAX* sizing macros (16/4/4/2/32), rxid/txid/sp0id/stype/chid/inid/mixinid/getbuffsize, SetRadioStructure + set_cmdefault_rates.  CreateRadio/DestroyRadio carried with the unported pipe/sync calls commented.
- **`src/wire/CmBuffs.{h,cpp}` rewritten verbatim** — `cmb,*CMB` twin typedef, `#define CMB_MULT (3)`, malloc0/_aligned_free, no calloc/intptr_t/guard deviations; pcm->in[] allocation moved back to create_cmaster (reference shape).
- **`src/wire/CMaster.{h,cpp}` rewritten verbatim** — FULL `_cmaster` struct (cmaster.h:39-99, PS surface incl. out[3]/panalalloc/pgain/peer), `cmaster,*CMASTER`, raw TCI fn ptrs, `enum AudioCODEC`, `cm = {0}`.  **TxChannel RAII carve-out DELETED** (src/wdsp/TxChannel.{h,cpp} retired): verbatim no-arg create_xmtr opens the WDSP TXA channel (chid(1,0)=1) + TX analyzer (disp 1; pan analyzer = disp 0, no collision) + out[0..2] + create_ilv itself through the wdspcalls seam.  create_cmaster/destroy_cmaster verbatim per-stream loops (update[] CS + cmbuffs + in[] for all cmSTREAM streams; create_rcvr = deferred stub, RX hybrid).  xcmaster verbatim: update[] critical section restored, real stype/txid/chid, TCI-override memset restored; fexchange0 + monitor-mix xMixAudio (accept-gated, quiescent vs the 1-input WdspEngine mixer) + xilv live.  SetXcmInrate/SetCMAudioOutrate/SetRcvr-/SetXmtrChannelOutrate/SetRunPanadapter/SetAntiVOXSource* ported.  All deferred subsystem lines carried IN PLACE as reference text with DEFERRED tags.
- **`src/wire/cmcomm.h`** — opaque verbatim twin typedefs ANB/NOB/EER/VOX/TXGAIN/ANALYZERS (tags match reference headers; completing a type later is source-compatible) + the umbrella-mapping note (.cpp files include explicit family headers — an include-list umbrella would be circular under #pragma once because the family headers include cmcomm.h for the base surface).
- **main.cpp** — SetRadioStructure(2,1,1,1,0,…)/set_cmdefault_rates(48 k) config block BEFORE create_cmaster (derived ids match the live layout: chid(0,0)=0 = WdspEngine RX1 channel, chid(1,0)=1 = TX channel); create_xmtr() invoked in the QTimer block after resolve_wdsp_calls (the documented DEFERRED-CALLSITE accommodation); handler-1.5 = gated destroy_xmtr().  Consumer-side decls for the headerless PORT functions (test_ilv precedent).  RadioNet.cpp → enum AudioCODEC cases; scratch/test_ilv.cpp → verbatim `cmaster cm = {0};` globals.

### Verification
- Clean build, ZERO warnings (C4701 in getChannelOutputRate disabled function-wide with a documented verbatim-text-wins pragma — line-level suppress can't reach the code-gen-stage warning).
- scratch/test_ilv.exe ALL PASS against the new verbatim globals.
- Mechanical diff vs reference: CmBuffs.{h,cpp} / cmsetup.h / CMaster.h struct+decls+enum / all 10 fully-verbatim cmaster.c functions IDENTICAL (whitespace-normalized); cmsetup.cpp comment-only deltas; live-line-subset check PASS for the partially-deferred bodies.  Sole code accommodation: `(char *)""` at the XCreateAnalyzer call (C++ string-literal constness).

### Behavior changes at startup (for the bench)
- TWO cm_main pump threads now start (streams 0 + 1; stream 0 idles forever — no Inbound producer; the reference layout).  Stream rings sized from cmMAXInRate=384000 (in[] = 512 complex).
- TX stays wire-quiescent: no Inbound() producer yet, pcm->OutboundTx null until P3.
- destroy ordering: handler-1.5 destroy_xmtr() (gated) → handler-4 destroy_cmaster() = reference relative order.

### NEXT
1. ~~**Operator HL2 RX-regression bench** (the P0.d gate)~~ — ✅ **PASSED 2026-06-12** (RX working; gate cleared).
2. **P1 = obbuffs.c port** (TX-out seam, separate TU from cmbuffs) → P2 sendOutbound audit → P3 netInterface registration (outbounds AFTER create_xmtr) → P4 Wire-LIVE (one commit, HL2 bench gate).

---

## 2026-06-08 (Monday EOD — STAGE B aamix.c port COMPLETE + bench-validated)

**Branch:** `tx-rebuild` HEAD = `533b06b`. Pushed to `origin/tx-rebuild` (0/0 in sync). `origin/main` deliberately untouched (different arc). **Backup:** `_backups/lyra-cpp-2026-06-08-aamix-stage-B-COMPLETE.bundle` (16 MB, `git bundle --all`).

### Today's shipped commits (in order, all on `tx-rebuild`)
```
533b06b [aamix port -- Stage B.6.b-final]   strip diagnostic instrumentation
8b8e0da [aamix port -- Stage B.6.b-fix1]    remove Stage-A NO-OP SendpOutboundRx stub (THE FIX)
12369bc [aamix port -- Stage B.6.b-debug-sink] dispatchAudioFrame sink-side counters
225518c [aamix port -- Stage B.6.b-debug]   9-counter chain instrumentation
53d9e5a [aamix port -- Stage B.6.b-retry]   wire AAMix via reference-faithful path
6e773a5 Revert "[aamix port -- Stage B.6.b] wire AAMix(1-input passthrough) into RX path"
525c2f0 [aamix port -- Stage B.6.b]         wire AAMix(1-input passthrough) -- REVERTED at bench
27ac2fa [aamix port -- Stage B.6.a]         extract feedIq audio tail into dispatchAudioFrame
```

### Arc summary
- **B.0 → B.5:** AAMix.h/cpp port complete from earlier sessions (538-line header verbatim from `aamix.c` with GPL v3+ attribution to NR0V/WDSP, 1100+ lines of cpp; idiom translations from Win32 to C++23: `CRITICAL_SECTION→std::mutex`, `HANDLE semaphore→std::counting_semaphore`, `_beginthread→std::jthread`, `_Interlocked*→std::atomic` with fetch_or/fetch_and, `malloc0→std::vector` RAII).
- **B.6.a (`27ac2fa`):** Refactored `feedIq` audio tail (lines 2402-2456) into new `WdspEngine::dispatchAudioFrame(audio, nframes)` helper. Byte-identical relocation; bench-confirmed.
- **B.6.b first attempt (`525c2f0`):** Wired `aaMix_` into RX path with `create_aamix(active=0x01L)` shortcut. **Operator bench: NO audio.** Reverted (`6e773a5`).
- **B.6.b-retry (`53d9e5a`):** Reference-faithful per `cmaster.c:297-313`: `create_aamix(active=0L, ring_size=4096, slew=0/10/0/10ms)` → `SetAAudioMixOutputPointer(aaMix_, 0, outbound)` → `SetAAudioMixState(aaMix_, 0, 0, 1)` (activate via close_mixer/open_mixer slew atom). **Operator bench: STILL NO audio.**
- **B.6.b-debug (`225518c`):** Chain-side instrumentation — 9 atomic counters + 1 ENTRY beacon + producer/consumer 1-Hz log emits in `xMixAudio` / `mix_main`. **Operator bench result:** chain healthy end-to-end (`xmix=rel=wake=out=188/sec`, `nzOut`=99%, `mixmain_started=1`, `Outbound=set`) but operator confirmed STILL no audio. ⇒ defect is downstream of Outbound.
- **B.6.b-debug-sink (`12369bc`):** Dispatch-side instrumentation — 6 atomic counters in `dispatchAudioFrame` + 1-Hz log line tag `[disp-dbg]`. **Operator bench result:** `[disp-dbg]` lines NEVER fired in the entire 33-second capture. ⇒ `dispatchAudioFrame` is never entered. ⇒ the Outbound callable AAMix is calling is NOT the lambda WdspEngine wired.
- **B.6.b-fix1 (`8b8e0da`, THE FIX):** Root cause traced from bench timestamps:
  - `19:22:02.972` — `WdspEngine::openRx1` calls `create_aamix(id=0, ..., Outbound=dispatchAudioFrame_lambda)` ⇒ `paamix[0]->Outbound = real lambda` ✓
  - `19:22:02.973` — `HL2Stream::open() → create_rnet() → SendpOutboundRx(STUB)` ⇒ `pcm->OutboundRx = STUB`, then `SetAAudioMixOutputPointer(nullptr, 0, STUB)` resolves `nullptr → paamix[0]` ⇒ **`aaMix_->Outbound = STUB`, real lambda CLOBBERED** ✗
  - The Stage-A NO-OP stub at `RadioNet.cpp:272-283` was a placeholder ("Stage B aamix port wires it via SetAAudioMixOutputPointer") for the wire-up Stage B was supposed to replace. Stage B.6.b correctly wired `dispatchAudioFrame` in `openRx1` — but left the Stage-A stub registration in place, which then overwrote the wiring 1 ms later when `HL2Stream::open()` called `create_rnet()`.
  - **Fix:** delete the Stage-A stub registration. Comment block on the deleted case warns future hands that the wire-up needs the per-channel sink-routing context (`hl2Out_`, `hl2AudioPush_`, `audioRing_`) that the global `pcm` cannot reach — so the wire-up must live at `openRx1`, not `create_rnet`.
- **B.6.b-final (`533b06b`):** Strip diagnostic scaffolding. -134 lines. Brief comment in `AAMix.cpp` points future hands at commit history (`git show 225518c` for chain instrument, `git show 12369bc` for dispatch instrument).

### Operator HL2+ bench (final, 19:27:49 → 19:28:47 — 58-second run)
- HL2 jack startup: first `[disp-dbg]` post-unmute: `calls=188 muted=0 gain=0.0845 peak16=32767 hl2(push/null)=188/0 hl2pushSet=1 audioRingSet=0` — full-scale audio reaching AK4951.
- Operator-confirmed audible RX ("Yes we have audio now").
- Mute toggle via dispatch (muted counter 0→22→133→287).
- Output device switched to PC Soundcard at 19:28:17 → `hl2Out=0, audioRingSet=1, pc(push) 176→928/sec`.
- Switched back to HL2 jack at 19:28:23 → `hl2Out=1`, `hl2(push)` resumed climbing.
- AGC mode changes (med/slow/off/fast) clean.
- Chain `out=N` tracks 1:1 with dispatch `calls=N` across both sinks across the entire run — no dropped frames, no thread starvation.

### Tasks closed
- #130 [completed] Stage B.6 [DEFER] — migrate RX audio path to ported AAMix
- #132 [completed] Stage B.6.b — wire AAMix(1-input passthrough) into RX path
- Stages B.0–B.5 + B.6.a already closed in earlier sessions.

### Methodology lessons recorded (for future Stage X work)
- 2-stage instrumentation (chain + dispatch) localized the defect to a single line of code on the SECOND bench. No speculation cycle, no convergence theatre.
- Operator-empirical rule held: after first B.6.b failure I was tempted to revert the AAMix port itself; the chain counters proved AAMix was correct in isolation, redirecting the dig to the wire-up surface where the bug actually was.
- Hard rule for next port: counters first when the symptom is "nothing happens". Instrumentation cost is trivial vs the alternative (multi-round revert/speculate cycle).
- "Do as Thetis does, Lyra-Native style" continues to hold — the first B.6.b attempt's `active=0x01` shortcut cost a revert cycle; the reference's `active=0 then SetAAudioMixState(activate)` pattern worked first time on the retry.

### Memory + doc updates
- `MEMORY.md` index line for lyra-cpp-tx rewritten with "READ FIRST 2026-06-09 AM" header + branch/HEAD/backup/root-cause pointer.
- `project_lyra_cpp_tx.md` — new "▶ READ FIRST 2026-06-09 AM resume pointer" block at top, plus full "State (2026-06-08 EOD)" section. Earlier state preserved below.
- `EXECUTION_PLAN.md` — new "PROGRESS TRACKING — STAGE B (aamix.c port) [SIDE-TRACK]" section with full B.0 → B.6.b-final checklist; new row in VERIFICATION LOG; footer "Status as of 2026-06-08 EOD" updated.
- This SESSION_LOG entry.
- PDF/DOCX regenerated via `tools/sync_execution_plan.py`.

### Next session pointer
- **First action:** `git fetch origin` THEN `git status` + `git log --oneline -5 tx-rebuild origin/main` to surface divergence.
- **Next port target = OPERATOR'S CALL.** Stage B side-track done; candidates:
  - **Stage C:** `ilv.c` port (TX I/Q interleaver — pairs with the `SendpOutboundTx` stub already wired in `CMaster.cpp` Stage A)
  - **Stage D:** `xcmaster` pump body port
  - Or pivot back to Step 14 / Phase 3 wire-layer rebuild on `main` (previous-arc resume was TX-1 Component 7 — first end-to-end SSB voice TX)

### Wire status
RX audio flows live through the ported AAMix dispatcher on the `tx-rebuild` branch's `HL2Stream::open()` path. TX path unchanged from earlier `tx-rebuild` state.

---

## 2026-06-06 (Saturday LATE-AFTERNOON — §1-C FULLY COMPLETE + RE-AUDIT CLEAN)

**Branch:** `tx-rebuild` HEAD = `169f1c2`.  Build clean,
`lyra.exe` relinked at 11:10:09.  **16 commits shipped today**
+ TWO full multi-agent audit rounds.  §1-C arc COMPLETE
through Stage 4F.2 (FrameComposer dissolution).  Final
re-audit returned 37/37 PASS across 3 sections, zero remaining
§6-Q3/Q5-class candidates, zero forbidden tokens in src/wire/.

### What's reference-faithful now
- TX wire layer: 100% reference-faithful (all `_radionet`
  fields in RadioNet; all reference file-scope globals as
  TU-scope statics in correct .cpp; Router + OutboundRing
  + FrameComposer all dissolved into free functions matching
  reference; symmetric socket-binding via metis_wire_bind()
  in both Ep6RecvThread + Ep2SendThread).
- §1.1 verdict: 🔴 → ✅ PARITY (Stage 4E doc consolidation).

### What's NOT yet reference-faithful (tracked, NOT in §1-C)
- **Task #73** — pre-existing forbidden-token violations
  (Thetis / Console.cs / OpenHPSDR / PowerSDR) in 5 non-wire
  files: tci_server.{h,cpp}, mainwindow.cpp,
  hl2_stream.{h,cpp}, wdsp_native.h.  Doc-style cleanup,
  ~1 hour, no impact on shipped wire-layer paths.
- **Wire-INERT** — nothing in HL2Stream::open() calls the
  new wire-layer free functions yet (step 14 wire-up).
- **Task #114 deferrals** — EER mode, non-HL2
  WriteMainLoop_generic, PeakFwdPower/PeakRevPower helpers.
  Hardware-blocker (no ANAN hardware to bench-test).

### Step 14 scope (NOT trivial)
HL2Stream is 4203 lines total with its own monolithic
`rxWorkerLoop` + `txWorkerLoop` member methods at
`hl2_stream.cpp:921` + `:2437` (the OLD wire path).  Step 14
must REPLACE these (per Rule 7 "Delete TX, don't refactor")
with calls into the new wire-layer free functions:
1. `prn` global pointer assigned at session-open
2. `metis_wire_bind(socket, dest, dest_len)`
3. `outbound_init()` to size prn->outLRbufp/outIQbufp
4. `ForceCandC::prime(3, tx_freq, rx_freq)` synchronously
   per §7 temporal-separation contract
5. `Ep6RecvThread::start(socket_fd)` → spins RX thread
   reading from prn-> + WSAEventSelect
6. `Ep2SendThread::start(socket, dest, dest_len)` → spins
   TX thread; no more composer / ring params (Stage 4F.2)
7. Migrate HL2Stream's existing telemetry/mic/IQ callbacks
   to the new sink-based registration (Router::register_sink
   for IQ; Ep6RecvThread::set_*_sink for telemetry/mic/I2C)

**Plan-first commit (R6).**  Bench-critical (first time the
new wire layer actually emits datagrams to the radio).
Multi-hour, possibly multi-commit.  Wrong wire-up = broken
RX/TX on real HL2 hardware.

### Resume after step 14
Operator-bench verification of RX-side audio + telemetry on
real HL2+ hardware.  Then TX-side bench (CW/SSB + Palstar
power readings).  Then Task #73 cleanup (small, doc-only,
can happen anytime).

### Wire status
Wire-INERT.  All wire-layer components reference-faithful
but not connected to runtime.

---

## 2026-06-06 (Saturday AFTERNOON — §1.1 REVERT COMPLETE — historical)

**Branch:** `tx-rebuild` HEAD = `79fe4ec`.  Build clean,
`lyra.exe` relinked at 10:45:10.  **TWELVE commits shipped
today** + 6-agent comprehensive TX audit done.  ALL stages
of the §1-C sweep complete in code (§1.1 networking-
infrastructure exclusion fully reverted).  Only Stage 4E
(PARITY_CHECKPOINTS doc consolidation) remains before step 14
wire-up.

### Today's commits (newest first)
- `79fe4ec` §1-C Stage 4D — OutboundRing dissolves to free functions (§1.1 revert COMPLETE)
- `1d73bea` §1-C Stage 4C — Ep2SendThread reads from prn->OutBufp + g_fpga_write_bufp
- `f1031da` §1-C Stage 4B.1 — correct FPGAReadBufp scope (file-scope, not _radionet)
- `26acea3` §1-C Stage 4B — Ep6RecvThread reads from prn-> + WSAEventSelect
- `e157301` §1-C Stage 4A — add networking-infrastructure fields to RadioNet
- `d88ddd6` §1-C Stage 3 — OutboundRing wait-all via condition_variable
- `2b78d9e` §1-C Stage 2A — Router class → struct + free functions
- `0e2a375` §1-C Stage 1 — small reverts (wb_enable, push timeout, diag counters)
- `50b713b` §6-B nit — drop metis_write_frame null-guard
- `3bfd248` §6-B + §7 — TU-scope MetisFrame + ForceCandC populate

### §1.1 revert summary
All `_radionet` fields the original §1.1 exclusion (signed 🔴
2026-06-04) had punted to wire-layer class members now live in
RadioNet as members exactly per reference:
- Buffer pointers: `RxBuff`, `TxReadBufp`, `outLRbufp`,
  `outIQbufp`, `OutBufp` (+ `ReadBufp` declared P2-future-use)
- RX seq counters: `cc_seq_no`, `cc_seq_err` (P2-only;
  declared in RadioNet but HL2-inert)
- Thread handles: `hReadThreadMain`, `hWriteThreadMain`,
  `hKeepAliveThread` (std::thread types)
- Init semaphores: `hReadThreadInitSem`, `hWriteThreadInitSem`
  (std::counting_semaphore<1>)
- Outbound sync (cv-collapsed mirror of 4 reference HANDLE
  pairs): `cv_outbound` + `mu_outbound` + 4 bool flags +
  `outbound_stop`
- WSA event + waitable timer (Win32-only): `hDataEvent`,
  `wsaProcessEvents`, `hTimer`, `liDueTime`
- Networking config ports: `p2_custom_port_base`,
  `base_outbound_port`

Reference-FILE-SCOPE globals (NOT in `_radionet`) live as
TU-scope statics in their respective wire-layer .cpp files
per the §6-B precedent:
- `wire/MetisFrame.cpp`: `g_metis_out_seq_num` (was Ep2SendThread::out_seq_num_)
- `wire/Ep6RecvThread.cpp`: `g_metis_last_recv_seq`, `g_seq_error`, `g_seq_seen`, `g_control_bytes_in[5]`, `g_fpga_read_bufp`
- `wire/Ep2SendThread.cpp`: `g_fpga_write_bufp`

Wire-layer classes dissolved or reduced:
- `Router`: class → plain struct + free functions (Stage 2A)
- `OutboundRing`: class → namespace-scope free functions
  operating on `prn->...` (Stage 4D)
- `Ep6RecvThread`: still a class (thread orchestrator), but
  all data moved to `prn->...` or TU-scope statics
- `Ep2SendThread`: same — thread orchestrator, data moved out

### Remaining — Stage 4E (doc-only)
Consolidate the §1-C entry in `docs/architecture/PARITY_CHECKPOINTS.md`:
- Single §1-C entry covering Stages 1+2A+3+4A-D + the
  earlier §6-B + §7
- §1.1 verdict amended from 🔴 OPERATOR-APPROVED DEVIATION
  to ✅ PARITY (now genuinely byte/structure-faithful)
- Sign-off block + commit trailer

### Resume after Stage 4E
Phase 2 step 14 — wire `outbound_init()` + `metis_wire_bind()`
+ `ForceCandC::prime` + `Ep6RecvThread::start` + `Ep2SendThread::start`
into `HL2Stream::session_open()` in the correct temporal sequence.

### Wire status
Wire-INERT today.  All wire-layer components built but nothing
in `HL2Stream::session_open` calls them yet.

---

## 2026-06-06 (Saturday MID-DAY — STAGE 3 CHECKPOINT — historical)

**Branch:** `tx-rebuild` HEAD = `d88ddd6`.  Build clean, `lyra.exe`
relinked at 09:54:09.  Five commits shipped today + 6-agent
comprehensive TX audit done.  Stages 1+2A+3 of the §1-C sweep
DONE; Stages 2B+4 remain (bundled — §5.10 WSAEventSelect lives
in `_radionet` fields per reference, so it lands with §1.1
revert).

### Today's commits (newest first)
- `d88ddd6` §1-C Stage 3 — OutboundRing wait-all via condition_variable
- `2b78d9e` §1-C Stage 2A — Router class → struct + free functions
- `0e2a375` §1-C Stage 1 — small reverts (wb_enable, push timeout, diag counters)
- `50b713b` §6-B nit — drop metis_write_frame null-guard
- `3bfd248` §6-B + §7 — TU-scope MetisFrame + ForceCandC populate

### What got fixed to match reference (Stages 1+2A+3)
- §1.3: `std::atomic<long> wb_enable` → `volatile long` (matches `network.h:160`)
- §6.10: OutboundRing producer push — drop bounded 5s try_acquire_for + push_timeouts_*; unbounded `acquire()` mirroring reference Inbound/obbuffs
- §6.12: Ep2SendThread `datagrams_sent_`/`send_errors_` diagnostic counters dropped (no reference counterpart)
- §5.9: `class Router` → plain struct + free functions (`xrouter`/`register_sink`/`set_control_word`/`set_call_count`/`router_instance`) + file-scope `g_routers[]` array, mirroring reference `router.c` structure verbatim
- §6.9/6.11/6.14: OutboundRing wait-all via `std::condition_variable` + `bool` flags + single mutex (was 4 binary_semaphores + polling) — mirrors reference `WaitForMultipleObjects(2, hsendEventHandles, TRUE, INFINITE)` atomic wait-all, no polling, fully interruptible

### Remaining — Stage 4 (THE BIG ONE)
**§1.1 networking-infrastructure revert + §5.10 WSAEventSelect bundled.**

§1.1 (signed 🔴 2026-06-04) excludes from RadioNet a list of
fields that the reference puts INSIDE `_radionet`.  Under
strict "do as reference, period" §1.1 itself is a §6-Q3/Q5-
class candidate.  Stage 4 puts these fields back into RadioNet:
- Buffer pointers: `RxBuff`, `TxReadBufp`, `ReadBufp`, `OutBufp`, `outLRbufp`, `outIQbufp`
- Thread/sem/event handles: `hReadThreadMain`, `hsendEventHandles[2]`, `hobbuffsRun[2]`, etc.
- Waitable timer + WSA event: `liDueTime`, `hDataEvent`, `wsaProcessEvents`
- RX seq counters: `cc_seq_no`, `cc_seq_err`
- Networking ports: `p2_custom_port_base`, `base_outbound_port`

Then Ep6RecvThread + Ep2SendThread + OutboundRing read from
`prn->...` instead of their own members.  §5.10 (recv() →
WSAEventSelect) folds in because `hDataEvent` +
`wsaProcessEvents` are `_radionet` fields.

**Scope: multi-hour, multi-commit.**  Planned sub-stages:
- 4A: Add fields to RadioNet (additive, no behavior change)
- 4B: Ep6RecvThread refactor + WSAEventSelect mechanism
- 4C: Ep2SendThread refactor (reads buffers from RadioNet)
- 4D: OutboundRing — buffers + sync primitives move into
  RadioNet; class may dissolve into free functions
- 4E: PARITY_CHECKPOINTS §1-C entry finalized + §1.1 amendment

### Resume after Stage 4 completes
Phase 2 step 14 — wire `ForceCandC::prime` + `metis_wire_bind`
+ WSAEventSelect setup into `HL2Stream::session_open()` in the
correct temporal sequence.

### Wire status
Wire-INERT today.  All wire-layer components built but nothing
in `HL2Stream::session_open` calls them yet.

---

## 2026-06-06 (Saturday MID-DAY — AUDIT PAUSE BOOKMARK — historical)

**Branch:** `tx-rebuild` HEAD = `50b713b`.  Build clean, `lyra.exe`
relinked at 09:27:22.  All §6-B + §7 work shipped + audited
20/20 PASS this morning.

### Done this morning (2 commits, ALL like reference)
- `3bfd248` §6-B parity correction sweep + §7 ForceCandC populate
  - Hoisted `metis_write_frame` to `wire/MetisFrame.{h,cpp}` TU-scope
  - TU-scope `g_metis_out_seq_num` (was `Ep2SendThread::out_seq_num_`)
  - TU-scope socket/dest globals (were Ep2SendThread members)
  - DROPPED `Ep2SendThread::send_lock_` mutex (no reference counterpart)
  - §1.1 row split: RX seq stays / TX seq moves
  - `ForceCandC::prime` + `prime_pass` byte-for-byte from `networkproto1.c:106-139`
- `50b713b` §6-B nit — dropped `metis_write_frame` null-guard (no reference
  counterpart; audit-caught NOTE-D)
- Two parallel audits run: parity (20/10 PASS) + rule-compliance (10/10 PASS)

### Pause reason
Operator-requested **comprehensive TX audit** of ALL work since
the rebuild started (30 commits since `0348238` Phase 0 mapping
doc 2026-06-04).  Sections to audit against the reference:
- §1 RadioNet (`2171fb9`/`dcbf9d1`)
- §2 RbpFilter / RbpFilter2 (`228ffd8`)
- §3 HPSDR family + dispatch globals (`f3ccc51`/`55ae658`/`782e65d`)
- §4a/§4b-1/§4b-2/§4c FrameComposer 19-case dispatch
  (`93e1511`/`5fc4192`/`4446916`/`f06a796`)
- §5 / §5-A Ep6RecvThread + Router (`c8baa63`/`a6be425`/`b988177`/`6bbf449`)
- §6 / §6-A Ep2SendThread + OutboundRing (`4c580a1`/`89ab298`)
- §6-B + §7 already audited this morning (skipping re-audit)

### Resume point after audit
Resume here: **Phase 2 step 14 — wire-up `ForceCandC::prime` +
`metis_wire_bind` into `HL2Stream::session_open()`** in the
correct temporal sequence (bind → prime → Ep6 start → Ep2 start).

### Wire status
Wire-INERT today.  Nothing in `HL2Stream::session_open` calls
the new wire-layer components yet.  Step 14 (next commit after
audit closes) flips the wire on.

---

## 2026-06-05 (Friday EOD)

**Project:** lyra-cpp TX Wire-Layer Rebuild (Task #112) — branch `tx-rebuild`

### Done today
- **§5 EP6 Receive Path** — `Ep6RecvThread` + `Router` (`xrouter`/`twist`) populated (`c8baa63`)
- **§5-A 9-fix parity correction** (`a6be425`) including 🔴 CRITICAL RX-audio bug (IQ unpack scale 48 dB too quiet — caught before any consumer wired)
- **§5-A doc completeness** (`b988177`)
- **§6 EP2 Send Path** — `Ep2SendThread` + `OutboundRing` populated (`4c580a1`)
- **§6-A 6-fix parity correction** (`89ab298`) — MMCSS priority, per-family dispatch, CW bit-shift revert to verbatim, CW state per-sample reads, `binary_semaphore` UB guard, Rule 24 inline token
- 4 audit passes total (2 per §5 / §6) — all PASS after corrections

### Where we are
Branch `tx-rebuild` HEAD = `89ab298`. §5 + §6 of the wire-layer rebuild are **shipped + doubly-verified** (parity-audit + rule-audit + correction sweeps + re-audits, all clean). Wire-layer is still wire-inert — no Phase 1 code path instantiates the new components.

### Next up
**§7 — `ForceCandC` priming + ANY remaining wire-layer pieces.** Reference: `networkproto1.c::ForceCandCFrame` (lines 134-139) + `ForceCandCFrames` (lines 106-132). Small commit — just the 3-frame priming burst that runs at session start before the EP2 send loop begins.

After §7 completes the §1c-listed wire-layer skeletons, the next phase is **Phase 2 step 14 — wire-up**: instantiate Ep6RecvThread + Ep2SendThread + OutboundRing + Router + ForceCandC + FrameComposer into HL2Stream's session-open path.

### Pending / parked
- **Task #114** TX-policy plumbing — ATT-on-TX, panadapter offset, PA-enable safety, plus the §6 FIXMEs: `WriteMainLoop_generic` for non-HL2, EER mode (§6.4), `PeakFwdPower`/`PeakRevPower` helpers (§5 telemetry cases 0x08/0x10)
- **No hardware bench yet** — gateware-watchdog kill-test, real-antenna TX, foot-switch HW-PTT all parked until Phase 2 wire-up + verification
- **Don't push to GitHub** (standing rule)

### Commit summary (today)
```
89ab298 fix(wire): §6-A parity correction sweep — 6 fixes
4c580a1 feat(wire): populate §6 Ep2SendThread + OutboundRing
b988177 docs(parity): §5-A Lyra-native additions table — 2 audit-flagged rows
a6be425 fix(wire): §5-A parity correction sweep — 9 fixes
c8baa63 feat(wire): populate §5 Ep6RecvThread + Router
```

### Standing rules in effect
- Rule 24 "always verify against reference" — applied per commit, audit-verified
- 2026-06-05 directive "reference = make Lyra the same" — driving all Q-decisions
- §6 EER mode + non-HL2 dispatch + Peak power helpers all deferred to Task #114 with FIXMEs
- Operator hardware = HL2+/AK4951; no ANAN P1/P2 hardware available

---

*Session log started 2026-06-05.  Older entries (if any) below this line as they accumulate.*
