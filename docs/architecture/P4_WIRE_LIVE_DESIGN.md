# P4 — Wire-LIVE switchover: grounded design (2026-06-12)

The final step of the direct-port arc: the verbatim obbuffs →
sendOutbound → sendProtocol1Samples → WriteMainLoop_HL2 chain
becomes THE wire path; the legacy hl2_stream EP2 writer and the
Step-14 OutboundRing/Ep2SendThread translations retire.

Reference reads completed for this design (file:line provenance):
StartAudio netInterface.c:36-94; StopAudio :96-114;
sendProtocol1Samples networkproto1.c:1204-1267; WriteMainLoop_HL2
tail :1180-1201; the EP6 mic feed :560-580; sendOutbound
network.c:1237-1341 (ported at P2.c); **asioOUT cmasio.c:137-146**
(the decisive find, below); UpdateRadioProtocolSampleSize
netInterface.c:1836-1858 (ported at P3).

---

## 1. The pacing model (verbatim)

EP2 cadence is paced by BOTH producers, locked to the HL2 crystal:

```
RX audio:  EP6 IQ -> WDSP RXA -> AAMix -> OutboundRx = OutBound(0,...)
           -> obbuffs ring 0 -> ob_main -> sendOutbound -> outLRbufp
           -> Release(hsendLRSem) -> Wait(hobbuffsRun[1])
TX I/Q:    EP6 mic -> Inbound(1,...) -> stream-1 cm pump -> xcmaster(1)
           -> fexchange0 (WDSP TXA) -> xilv -> OutBound(1,...)
           -> obbuffs ring 1 -> ob_main -> sendOutbound -> outIQbufp
           -> Release(hsendIQSem) -> Wait(hobbuffsRun[0])
Writer:    sendProtocol1Samples: WaitForMultipleObjects(BOTH, TRUE)
           -> eer overwrite if peer->run && XmitBit (never on HL2)
           -> !XmitBit => memset(outIQbufp) (the zero-on-no-MOX posture)
           -> swap_audio_channels -> 16-bit BE quantize + HL2 CW overlay
           -> WriteMainLoop_HL2(OutBufp) -> MetisWriteFrame
           -> Release(hobbuffsRun[0]) + Release(hobbuffsRun[1])
```

Both streams flow ALWAYS while the session runs (mic samples arrive
in every EP6 datagram per the HL2+ ak4951v4 gateware ground truth;
RX audio is produced from every IQ block).  There is NO separate
EP2 keepalive on P1 — if EP6 stops, EP2 stops, and the gateware
watchdog idles the radio (the reference posture; the legacy
silence-keepalive retires with the legacy writer).  Note the
legacy txWorkerLoop already converged on audio-production pacing
empirically — the model is bench-proven on this hardware.

## 2. The RX-side hybrid resolution (the B.6.b-delicate piece)

**The reference itself resolves it.**  asioOUT (cmasio.c:137-146)
is the registered RX Outbound consumer when audio goes to the PC
(ASIO) instead of the radio jack:

```c
void asioOUT(int id, int nsamples, double* buff)
{
	if (!pcma->run) return;
	xrmatchIN(pcma->rmatchOUT, buff);        // audio to the PC device
	if (pcma->protocol == 0)                 // Protocol 1
	{
		memset(buff, 0, nsamples * sizeof(complex));
		OutBound(0, nsamples, buff);         // ZEROED LR still paces the wire
	}
}
```

So Lyra's WdspEngine `dispatchAudioFrame` (the AAMix Outbound
consumer, the operator-approved hybrid site) becomes the Lyra
codec dispatcher implementing BOTH reference postures:

- **HL2-jack output selected** (HERMES posture): call
  `OutBound(0, n, audio)` with the REAL audio — the EP2 LR bytes
  ARE the jack audio.  The legacy hl2AudioPush EP2 ring retires.
- **PC-soundcard output selected** (ASIO posture): push audio to
  Lyra's native PC ring (kept), then zero the buffer and call
  `OutBound(0, n, zeroed)` — the wire stays paced, the jack gets
  silence, exactly as the reference does for ASIO.

Registration stays AT the WdspEngine site (the documented hybrid
divergence) — create_rnet's HERMES case remains empty with the
pointer comment.  This cannot re-create the B.6.b clobber: there
is no competing SetAAudioMixOutputPointer call anywhere.

## 3. The mic feed (verbatim)

Reference networkproto1.c:560-580: per EP6 datagram, decimate +
scale mic into `prn->TxReadBufp` (I = mic * 1/2^31, Q = 0), then
`Inbound(inid(1, 0), mic_sample_count, prn->TxReadBufp);`.
Lands in Lyra's LIVE EP6 dispatch (the ep6Thread_ path) verbatim.
This is what finally releases stream 1's Sem_BuffReady and runs
the TX pump (xcmaster(1) -> fexchange0 -> xilv) continuously.

## 4. TUN + MOX re-home

- The legacy DC-injection TUN (hl2_stream EP2 packer ~:2516) dies
  with the legacy packer.  TUN becomes the reference mechanism:
  WDSP TXA output-side tone generator (SetTXAPostGen* family) —
  **pre-cdef audit required** (§15.18 discipline: row-by-row
  signature harvest from the WDSP source + DLL export check)
  before the wdspcalls entries land.
- SSB voice needs no generator: mic flows via §3; MOX gates the
  wire via the verbatim `!XmitBit => memset(outIQbufp)`.
- FSM keydown/keyup arm/disarm the TXA channel per the §15.25
  ground truth: keydown ... -> SetChannelState(chid(1,0), 1, 0)
  LAST; keyup: SetChannelState(chid(1,0), 0, 1) (blocking flush)
  FIRST -> mox_delay -> clear MOX -> ptt_out_delay -> RX restart.
  The existing TX-0c FSM keeps its TR sequencing + ATT-on-TX; only
  its inject/tone actions re-home.

## 5. Control-plane mapping

The LIVE operator controls (drive %, PA enable, TX step-att, TX
freq, OC bits, MOX) currently write hl2_stream legacy state read
by its own composeCC.  The verbatim C&C composer (FrameComposer's
write_main_loop_hl2) reads `prn` fields + the Apollo globals.
P4.b includes the full mapping table (filled at implementation,
verified control-by-control):

| operator control | legacy state (hl2_stream) | verbatim home (prn/global) |
|---|---|---|
| TX drive %    | driveLevel_-class member | prn->tx[0].drive_level |
| PA enable     | paEnabled_ + composeCC bit | ApolloTuner/ApolloFilt globals |
| TX step-att   | ATT-on-TX policy writes | prn->adc[i].tx_step_attn |
| RX/TX freq    | freq members | prn->rx[0]/tx[0].frequency |
| MOX           | mox_/moxActive_ atomics | XmitBit global |
| OC bits       | ocPattern state | prn (FrameComposer case-0 source) |

Rule: each setter writes the verbatim home; the legacy member
stays only if UI reads it (single-writer discipline preserved).

### 5.1 RX-side rows — IMPLEMENTED in the RX-out gate (2026-06-13)

The operator-approved P4.b split (RX-out gate = pieces 1+2+3+7
first, TX = 4+5+6 second) initially under-scoped §5: the RX gate
shipped without ANY control-plane mapping, so the verbatim
write_main_loop_hl2 (now THE live C&C composer once composeCC's
txWorkerLoop launch retired) read empty `prn` → DDC0 at 0 Hz →
**dead RX, no audio, no signals — operator-confirmed on the bench
2026-06-13.**  The §5 line list always named "control-plane
mapping" as a core P4.b piece; the omission was the split, not the
plan.  Resolution (plan-faithful, NOT a patch): the **RX-relevant
rows** of the §5 table land in the RX-out gate; the TX-only rows
stay in commit 2 (TX-inert at RX, so the split holds).

| §5 row | RX-gate home write | site |
|---|---|---|
| RX freq | `set_rx_freq(0,hz)`+`set_rx_freq(1,hz)`+`set_tx_freq(hz)` | `setRx1FreqHz` |
| sample rate | `SampleRateIn2Bits = bits` | `setSampleRate` |
| LNA gain | `set_rx_step_attn_db(db+12, 0)` (case 11 !XmitBit; +12 = field-proven HPSDR P1 bias, byte-identical to retired composeCC) | `setLnaGainDb` + `applyLnaGainNoPersist` |
| OC bits | `prn->oc_output = pattern` (composer shifts `<<1 & 0xFE`) | `updateOcPattern` |

Plus an **at-open seed** in `open()` (before `ep6Thread_.start()`,
so the EP6 priming pass + first compose tune correctly) that
writes all four homes from the persisted operator state — without
it RX stays dead until the first dial gesture.  `prn` is non-null
there (the open() guard returns otherwise); per-setter sites carry
`if (prn)` for the pre-open window.  Writes go directly to `prn`
via the FrameComposer setters — matches the reference (Console
writes `prn->rx[]` from the control thread) + the shipped
`AttOnTxPolicy` precedent; no lock, no command queue.

**PureSignal-safe:** this only POPULATES freq/rate/gain/OC homes the
verbatim composer already reads; it changes no WDSP call, no buffer,
no `create_xmtr` PS surface.  TX-side rows (drive %, PA enable, TX
step-att, MOX → XmitBit) remain commit 2.  `nddc=4` needs no mapping
(correct by default global).  `prn->rx[0].preamp` left at default
(composeCC never drove it; no operator preamp control).  The dead
`composeCC` method + `ocC2_`/`sampleRateBits_` legacy members are a
§7 retirement (orphaned, harmless until then).

## 6. Threads + lifecycle (verbatim homes)

- Quartet creation + writer-thread start at open(), per StartAudio
  netInterface.c:65-71: `prn->hWriteThreadMain = _beginthreadex(
  ..., sendProtocol1Samples, ...)` then the 4
  `CreateSemaphore(NULL, 0, 1, NULL)`.  P2.b already declared the
  HANDLE quartet; `hWriteThreadMain` converts std::thread ->
  HANDLE (verbatim field type).
- `io_keep_running` global: verify present (network.h:411); the
  writer loop exits on it.  **Shutdown gap to close (the reference
  is process-exit-rough here):** after `io_keep_running = 0`,
  release BOTH hsend semaphores once so the writer wakes from
  WaitForMultipleObjects, observes the flag, and exits — then join
  + CloseHandle the quartet.  Verify the reference IOThreadStop
  body first; if the reference truly relies on process exit, this
  is a documented Lyra-lifecycle accommodation (Lyra must
  stop/start cleanly — the cb58bcb class).
- close() order: io_keep_running=0 -> wake+join writer ->
  destroy_obbuffs(0/1) (already P3) -> CloseHandle quartet ->
  socket close.  App-quit keeps destroy_obbuffs BEFORE
  destroy_xmtr (sendOutbound reads pcm->xmtr[0].peer->run).
- sendProtocol1Samples verbatim TU home: new
  `src/wire/NetworkProto1.cpp` (networkproto1.c PARTIAL), calling
  Lyra's write_main_loop_hl2 (the monolithic WriteMainLoop_HL2
  equivalent, #121/#122).

### 6.1 Teardown accommodation — VERIFIED + IMPLEMENTED + OPERATOR-APPROVED 2026-06-13

The §6 "Verify the reference IOThreadStop body first" caveat is now
resolved.  **Verification (2026-06-13):** reference `IOThreadStop`
(network.c:1434-1469) sets `io_keep_running=0`, `WaitForSingleObject`
on `hReadThreadMain` ONLY, then `CloseHandle`s `hWriteThreadMain` +
the 4 P1 sems — it never wakes or joins `sendProtocol1Samples`.  The
ONLY `ReleaseSemaphore(hsendIQSem/hsendLRSem)` sites in all of
ChannelMaster are inside `sendOutbound` (network.c:1311-1336, the
RUNTIME producer path); there is NO shutdown-path wake.  So the
reference deterministically orphans the writer parked in
`WaitForMultipleObjects(INFINITE)` — a `CloseHandle`-while-waiting
(documented Win32 UB; the MW0LGE :1456 comment shows they fought
crashes here) that relies on process-exit to reap the thread.

**The ONE P4.b deviation (operator-approved 2026-06-13):** Lyra must
stop/start cleanly (cb58bcb class; bench gate), so after
`io_keep_running=0` + `ep6Thread_.stop()` (producers quiesce, ob_main
parks on Sem_BuffReady), `HL2Stream::close()` releases both hsend sems
once → the writer wakes, observes the flag, runs one final benign
iteration (MOX force-cleared above ⇒ `!XmitBit` ⇒ zeroed TX I/Q),
exits → `WaitForSingleObject(hWriteThreadMain)` + `CloseHandle`.  This
also removes the reference's `CloseHandle`-while-waiting UB.

**PureSignal-safe:** runs ONLY at `close()`; changes no WDSP call, no
wire byte, no buffer (outLRbufp/outIQbufp/OutBufp), no
MetisOutBoundSeqNum, and does NOT touch `sendOutbound`'s runtime
sem-release path (verbatim P2.c).  It governs only WHEN the writer
THREAD stops — Lyra-native thread/process model (rule #2).
`create_xmtr`'s PS surface is not torn down here (app-quit
`destroy_xmtr`), so it survives stop/start intact.

**Ordering correction to the §6 sketch above:** the implemented order
is the VERIFIED reference order — `CloseHandle` quartet (IOThreadStop)
→ STOP send + socket close (StopReadThread) → `destroy_obbuffs(0/1)`
(StopAudio :112-113) — i.e. quartet-close BEFORE `destroy_obbuffs`,
the *reverse* of the §6 line-147 prose ("destroy → CloseHandle
quartet").  Safe because the writer-join above leaves ob_main parked
on Sem_BuffReady (no producer), so it never `ReleaseSemaphore`s a
closed hsend handle and `destroy_obbuffs` stops it last, exactly as
Thetis sequences StopAudio.

## 7. Retirements

- hl2_stream txWorkerLoop + buildEp2KeepaliveTemplate + the
  silence keepalive + the DC TUN injection + EP2 slew fill (the
  verbatim model has no underrun-fill: the writer only sends when
  both buffers are ready).
- wire/OutboundRing.{h,cpp} (the cv translation) + outbound_init.
- wire/Ep2SendThread.{h,cpp} (never started; superseded by the
  verbatim sendProtocol1Samples).
- FrameComposer write_main_loop_hl2 tail: the cv-based
  `outbound_notify_consumed_pair()` becomes the verbatim
  `ReleaseSemaphore(prn->hobbuffsRun[0/1], 1, 0)` pair (dormant
  flip — nothing calls write_main_loop_hl2 until the new chain).

## 8. Stage split + bench gate

- **P4.a (prep, wire-inert commit):** SetTXAPostGen* pre-cdef
  audit + wdspcalls entries; wire/NetworkProto1.cpp verbatim
  sendProtocol1Samples (dormant, thread not started); TxReadBufp +
  hWriteThreadMain verbatim field conversions; io_keep_running
  verify; FrameComposer tail flip (dormant); reference
  IOThreadStop read for the shutdown-gap decision.
- **P4.b (THE switchover — ONE commit, full operator HL2 bench):**
  quartet + writer-thread start at open(); dispatchAudioFrame
  asioOUT-pattern tee (both output modes); Inbound(1, mic) in the
  live EP6 path; FSM re-home (TXA channel arming + TUN postgen);
  control-plane mapping (§5 table); retirements (§7); teardown
  ordering (§6).

**P4.b bench gate (per the 2026-06-12 discipline: explicit TX
state lines):**
1. RX on HL2-jack output; RX on PC-soundcard output (the zeroed-LR
   pacing case); stop/start cycles; clean shutdown.
2. TX state: TUN carrier into dummy via the postgen path — power
   ~= the 5 W / PA ~1.76 A anchors; clean keydown (no relay
   chatter) + clean keyup (no broom).
3. **First VOICE through the direct-port chain**: SSB USB/LSB into
   the dummy — MIC/ALC/PWR meters live, external view clean.
4. PA-enable posture unchanged (opt-in gate still arms/disarms RF).
