# CM thread mechanism (per-stream)

**Sources read 2026-06-04:**
- `ChannelMaster/cmaster.c` — full file (595 lines)
- `ChannelMaster/cmbuffs.c` — full file (190 lines)
- Spot grep into `ChannelMaster/cmasio.c` (full read pending)
- Spot grep into `ChannelMaster/netInterface.c` (full read pending)

---

## Architecture in one line

The reference uses **N dedicated `cm_main` threads — one per stream —
each MMCSS "Pro Audio" priority 2, semaphore-blocked, producer-paced**.
NO polling, NO `Thread.Sleep(N)` busy loops on the DSP path.

## Per-thread setup

`cmbuffs.c:151-168 cm_main(void* pargs)`:

```c
void cm_main (void *pargs)
{
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
    if (hTask != 0) AvSetMmThreadPriority(hTask, 2);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    int id = (int)pargs;
    CMB a = pcm->pdbuff[id];

    while (_InterlockedAnd (&a->run, 1))
    {
        WaitForSingleObject(a->Sem_BuffReady, INFINITE);
        cmdata (id, pcm->in[id]);
        xcmaster(id);
    }
    _endthread();
}
```

KEY OBSERVATION: blocked on `WaitForSingleObject(..., INFINITE)` —
producer-paced, not timer-paced.

## Ring (cmbuffs)

`cmbuffs.c:35-58 create_cmbuffs()`:
- One `cmb` struct per stream
- Ring backing buffer: `double[r1_active_buffsize * 2]` (complex doubles)
- Two critical sections: `csIN` (producer side `Inbound()`), `csOUT`
  (consumer side `cmdata()`)
- Counting semaphore initialised to 0, max 1000

`cmbuffs.c:88-121 Inbound(id, nsamples, in)` — producer:
- Lock `csIN`
- memcpy samples into ring with wraparound
- When accumulated samples cross `r1_outsize`, release N semaphore
  counts (one per output-block-worth)
- Unlock

`cmbuffs.c:123-149 cmdata(id, out)` — consumer:
- Lock `csOUT`
- Check run flag; if cleared, exit thread (the destroy path)
- memcpy `r1_outsize` samples from ring into `out` with wraparound
- Unlock

## Per-stream DSP dispatch

`cmaster.c:339-405 xcmaster(int stream)`:
- Lock `pcm->update[stream]` — per-stream config-vs-DSP serialization
- switch on `stype(stream)`:

**Case 0 (RX):** xpipe → xanb (noise blanker) → xnob (NB2) →
Spectrum0 (panadapter feed) → fexchange0 (WDSP RXA) → xMixAudio
(audio mixer + anti-vox feeds)

**Case 1 (TX):** asioIN (ASIO mic in) → if `use_tci_audio`: call
`InboundTCITxAudio` callback **overwriting the buffer** → xpipe →
xdexp (VOX) → **fexchange0 (WDSP TXA — the actual DSP)** →
xsidetone → xpipe → xMixAudio (monitor) → xtxgain (Penelope gain +
amp protect) → xeer (EER transmission) → xilv (interleave EER, call
`Outbound()` for wire)

**Case 2:** special stitched upper panadapter (RX-only).

## TX channel OpenChannel parameters

`cmaster.c:177-190` create_xmtr:
- channel type = **1** (TX)
- input rate = `pcm->xcm_inrate[in_id]` (operator setting, typically 48 kHz)
- **DSP rate = 96000 Hz** (line 182) — TXA runs at 96 kHz internally
- output rate = `pcm->xmtr[i].ch_outrate` (HL2 wire rate = 48 kHz)
- `tdelayup = 0.000, tslewup = 0.010` → 10 ms cos² up-ramp on TX
  channel start (keydown)
- `tdelaydown = 0.000, tslewdown = 0.010` → 10 ms cos² down-ramp on
  TX channel stop (keyup)
- **block param = 1** (block until output available) — corrects the
  "non-blocking fexchange0 stale buffer" class of defect

## TCI hand-off mechanism

`cmaster.c:429-432`:
```c
PORT
void SendpInboundTCITxAudio (void (*Inbound)(int nsamples, double* buff))
{
    pcm->InboundTCITxAudio = Inbound;
}
```

`cmaster.c:441-444`:
```c
PORT
void SetTXTCIAudio (int txid, int active)
{
    _InterlockedExchange (&pcm->xmtr[txid].use_tci_audio, active);
}
```

So: TCIServer (C# side) registers an inbound callback via
`SendpInboundTCITxAudio`. When the operator's TCI client signals
`trx:0,true,tci`, the C# side toggles `use_tci_audio=1` via
`SetTXTCIAudio`. Then xcmaster's TX dispatch (case 1) sees the flag
and calls the registered callback INSTEAD of using the ASIO mic
samples — the callback writes mono TX audio (already L+R averaged
and rate-converted) into `pcm->in[stream]`.

This is fundamentally different from Lyra's prior approach where the
TCI inbound queue + drain timer fed the worker ring. The reference's
model is: TCI populates the in-flight buffer at the moment xcmaster
runs, via a synchronous callback.

## Stream creation pattern

`cmaster.c:273-320 create_cmaster()`:
- Loops `i = 0 to cmSTREAM`:
  - `create_cmbuffs(i, ...)` (which spawns the cm_main thread for
    stream i)
  - `pcm->in[i] = malloc0(...)` (the per-stream input buffer)
- Then `create_rcvr()` — sets up RXs with cmRCVR / cmSubRCVR loops
- Then `create_xmtr()` — sets up TXs (typically cmXMTR = 1 for HL2)
- Then the global audio mixer (`create_aamix` line 297) for mixing
  RX audio for output

So the reference has, for a typical HL2 setup with RX1+RX2+TX:
- 3 cm_main threads (one per stream)
- Each MMCSS Pro Audio priority 2
- Each semaphore-blocked on its own Sem_BuffReady
- Each runs its own slice of xcmaster

## Threads found so far (TX-path-relevant only)

| Thread | File:line spawn | Priority | Wait | Cadence |
|---|---|---|---|---|
| cm_main (per stream) | cmbuffs.c:31 | MMCSS Pro Audio @ 2 | semaphore INFINITE | producer-paced |
| MetisReadThreadMain | netInterface.c:61 | TBD | TBD | EP6 packet arrival |
| sendProtocol1Samples | netInterface.c:66 | TBD | TBD | TBD |
| KeepAliveMain | netInterface.c:83 | TBD | TBD | TBD |

(Console.cs Qt-main-equivalent UI thread + TCIServer.cs threads are
documented separately in their respective notes files when read.)

## Critical correction — what changed in Phase 0

Earlier work in this project (the now-superseded audit dossier and
the §111 patching arc) referenced the reference's "TX service loop
with `Thread.Sleep(2)`." **This is wrong.** The reference's TX
service thread is `cm_main`, blocked on
`WaitForSingleObject(..., INFINITE)`. It runs producer-paced, not
on a 2 ms timer. The mapping doc and any task descriptions citing
the old framing have been corrected.

The `Thread.Sleep(2)` reference may still exist somewhere
(TCIServer.cs CHRONO pull loop is a likely candidate) — flagged as
Open Question #3.

## Open questions parked for next reads

1. **What releases the TX cmbuffs semaphore?** cmasio.c is the most
   likely producer. Expected: the ASIO host callback receives mic
   chunks and calls `Inbound(tx_stream_id, n, samples)`. Verify by
   full read of cmasio.c.
2. **EP2 wire-send cadence.** `sendProtocol1Samples` mechanism —
   timer-paced? Semaphore-driven by audio mixer Outbound? Read
   `netInterface.c`'s `sendProtocol1Samples` function next.
3. **Where does the `Thread.Sleep(2)` reference live?** Possibly
   TCIServer.cs CHRONO pull; possibly nowhere. Investigate in §7
   read.

## Provenance trace

Every claim in this file traces to a specific source line:
- `cm_main`: cmbuffs.c:151-168
- `Sem_BuffReady` creation: cmbuffs.c:54
- `start_cmthread` / `_beginthread`: cmbuffs.c:29-33
- MMCSS Pro Audio priority: cmbuffs.c:154-156
- `Inbound()` producer: cmbuffs.c:88-121
- `cmdata()` consumer: cmbuffs.c:123-149
- TX OpenChannel params: cmaster.c:177-190
- xcmaster TX dispatch: cmaster.c:377-398
- TCI callback registration: cmaster.c:429-432
- `use_tci_audio` toggle: cmaster.c:441-444
- Stream creation loop: cmaster.c:273-320

*File written 2026-06-04 during Phase 0 read.*
