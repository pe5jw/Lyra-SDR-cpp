# Wire threads and the TX cmbuffs mic producer

**Sources read 2026-06-04:**
- `ChannelMaster/cmasio.c` (404 lines, full read)
- `ChannelMaster/networkproto1.c` lines 380-420 (HL2 EP6 mic extraction)
- `ChannelMaster/networkproto1.c` lines 1204-1267 (`sendProtocol1Samples`)
- `ChannelMaster/netInterface.c` lines 61-83 (thread spawn)
- Cross-grep of `Inbound(` callers tree-wide

---

## Where the TX cmbuffs semaphore release comes from

**Answer:** from the HL2 EP6 receive path itself. The HL2+ AK4951 codec
samples mic audio at 48 kHz on the board, packs each mic sample into
every EP6 frame alongside the RX IQ data, and the host's wire-recv
thread (`MetisReadThreadMainLoop_HL2` in networkproto1.c) extracts the
mic byte pair per sample and calls `Inbound(inid(1, 0), N, mic_buf)`
once per EP6 frame's worth.

That call releases the TX cmbuffs semaphore. So **the TX cm_main
service thread is paced by the HL2's 48 kHz crystal**, via the wire.
No timer. No `Thread.Sleep(2)`. The HL2 hardware IS the clock.

`network.c:650-652` confirms the pattern for all three streams:

```c
Inbound(inid(0, 0), prn->rx[0].spp, prn->RxReadBufp);   // RX1
Inbound(inid(0, 1), prn->rx[1].spp, prn->RxReadBufp);   // RX2
Inbound(inid(1, 0), prn->mic.spp,   prn->TxReadBufp);   // TX (mic from EP6)
```

One wire-recv thread parses one EP6 frame and dispatches to all three
streams' cmbuffs. The three cm_main consumer threads each wake on
their own semaphore and process their own stream's data in their own
thread context.

## HL2 codec mic extraction (file:line)

`networkproto1.c:404-413` (inside the HL2 EP6 parse loop):

```c
prn->TxReadBufp[2 * mic_sample_count + 0] = const_1_div_2147483648_ *
    (double)(bptr[k + 0] << 24 |
             bptr[k + 1] << 16);
prn->TxReadBufp[2 * mic_sample_count + 1] = 0.0;

mic_sample_count++;
```

After the inner sample loop:

```c
Inbound(inid(1, 0), mic_sample_count, prn->TxReadBufp);
```

**Critical detail:** the mic samples are written as **I=mic, Q=0**
(line 407 explicitly zeros the Q half). This is the format the TX
cmbuffs receives; the format `cmdata()` drains into `pcm->in[stream]`;
the format `fexchange0` receives on the TX channel's input side.

(Mic samples are 16-bit signed values; the shift `bptr[k+0]<<24 |
bptr[k+1]<<16` packs them into the upper bytes of a 32-bit MSB-first
word, then divides by 2^31 — recovers a normalized double in
[-1.0, 1.0). The I=mic / Q=0 means the input is REAL-valued, not
complex. WDSP's TXA chain handles real-input correctly because the
NCO sign flip + SSB bandpass produce the correct sideband output.)

## What this means for the Lyra TCI path

When `use_tci_audio=1`, xcmaster's TX dispatch (cmaster.c:380-386)
calls `InboundTCITxAudio(pcm->xcm_insize[stream], pcm->in[stream])`
which **overwrites pcm->in[stream]** with TCI audio. For the
overwrite to be format-compatible with fexchange0, TCIServer.cs
must write the same I=mic, Q=0 pattern.

If TCIServer.cs writes I=Q=mono (both halves filled with the same
mono value), the WDSP TXA chain sees a *complex* input with energy
in both I and Q — that's a different mathematical signal than a
real-valued mic input.

**Task #67 framing reads "match Thetis I=Q=mono (not I=mono,
Q=0)" — this is contradicted by networkproto1.c:404-407 for the
HL2 codec mic path.** Either:
- TCIServer.cs uses a different convention than the HL2 codec mic
  path (in which case the framing is correct for the TCI side
  specifically, and the doc title is ambiguous), or
- Task #67 was wrong and Lyra's TCI path has a deviation from the
  reference's HL2 mic format.

**Resolution depends on reading TCIServer.cs's `RealHandleAudioStream`
/ `HandleTrx` (TCI TX-audio inbound) — pending in next reads.**
Flagging as Open Question #4 in the mapping doc.

## sendProtocol1Samples — the wire-send thread

`networkproto1.c:1204-1267`. Spawned at `netInterface.c:66`.

```c
DWORD WINAPI sendProtocol1Samples(LPVOID n)
{
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
    if (hTask != 0) AvSetMmThreadPriority(hTask, 2);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    ...
    pbuffs[0] = prn->outLRbufp;     // L+R audio (from global mixer Outbound)
    pbuffs[1] = prn->outIQbufp;     // TX I/Q (from TX channel xilv → OutboundTx)

    while (io_keep_running != 0)
    {
        WaitForMultipleObjects(2, prn->hsendEventHandles, TRUE, INFINITE);
        // ^ two-event AND: needs BOTH LR and IQ to advance one frame

        if (pcm->xmtr[0].peer->run && XmitBit)
        {
            memcpy(prn->outLRbufp, prn->outIQbufp + 256, sizeof(complex) * 126);
            // EER override: TX magnitude goes on LR for EER hardware
        }
        if (!XmitBit) memset(prn->outIQbufp, 0, sizeof(complex) * 126);
        // ^ MOX OFF → zero TX I/Q before quantize. Wire-side safety.

        ... (swap_audio_channels handling) ...

        for (i = 0; i < 2 * 63; i++)
            for (j = 0; j < 2; j++)
                for (k = 0; k < 2; k++)
                {
                    temp = round(pbuffs[j][i*2+k] * 32767.0);
                    if (cw_enable && j == 1)
                        // overwrite TX I bytes with CW state bits
                        if (HL2) temp = (cwx_ptt<<3 | dot<<2 | dash<<1 | cwx) & 0x0F;
                        else     temp = (dot<<2 | dash<<1 | cwx) & 0x07;
                    OutBufp[8*i + 4*j + 2*k + 0] = (temp >> 8) & 0xff;
                    OutBufp[8*i + 4*j + 2*k + 1] = temp & 0xff;
                }

        if (HL2) WriteMainLoop_HL2(prn->OutBufp);
        else     WriteMainLoop(prn->OutBufp);
    }
    return 0;
}
```

**Wake mechanism:** two-event AND wait. The thread does NOT poll, does
NOT sleep. It waits forever until both producers — the global audio
mixer's Outbound (LR audio) AND the TX channel's xilv→Outbound (TX
I/Q) — have signaled their event.

This pairs naturally with the cm_main per-stream architecture:
- Each cm_main thread, when it finishes xcmaster, ends with an
  `Outbound()` call (via xMixAudio for RX, via xilv for TX) that
  signals one of these two events.
- The TX cm_main signals the IQ event when xilv completes.
- The RX audio mixer (aamix) signals the LR event when its Outbound
  fires (paced by the RX cm_main threads through the audio mixer).
- The wire-send thread wakes when both have signaled, builds the
  next EP2 frame, sends it.

**Frame size:** 126 stereo samples per frame (line 1241 `2 * 63`),
8 bytes per LRIQ tuple. So one EP2 frame = 126 × 8 = 1008 bytes of
payload + 8 bytes header = 1016 bytes / USB block; two USB blocks
per UDP datagram = 504 + 504 + headers. At 48 kHz wire rate, 126
samples = 2.625 ms per frame, so the wire-send thread fires every
2.625 ms.

**The 2.625 ms cadence** is where the "every 2 ms" notion in earlier
Lyra docs MAY have come from — but it's NOT a `Thread.Sleep(2)`.
It's a producer-paced wait that happens to advance at ~2.625 ms
because that's the rate the HL2's 48 kHz clock produces 126-sample
chunks. Conflating the two is what produced the broken timer-paced
implementations in Lyra's prior TX work.

## CW state bits in TX I-LSBs (HL2-specific)

When CW is active, the **TX I-sample bytes are repurposed** to carry
CW key state:
- HL2 (4 bits): `cwx_ptt | dot | dash | cwx` packed into the low 4
  bits of the I-sample byte pair
- Non-HL2 (3 bits): `dot | dash | cwx`

This OVERWRITES the modulator's I output during CW transmit. Lyra's
CW modulator (v0.2.2 per the task list) must implement this exact
packing — see networkproto1.c:1247-1259.

## MOX gating on the wire

`networkproto1.c:1227`:
```c
if (!XmitBit) memset(prn->outIQbufp, 0, sizeof(complex) * 126);
```

**Single line, single test.** When MOX is off, the TX I/Q on the
wire is zeroed before quantize regardless of what xcmaster produced.
This is the wire-side safety net — even if some upstream layer (TCI
override, sidetone, etc.) leaves garbage in the buffer, the wire
never sees it. Lyra's EP2 packer must mirror this exactly.

## Thread inventory — closing pass

| Thread | File:line spawn | Priority | Wait | Cadence |
|---|---|---|---|---|
| cm_main (per stream — 1× RX1, 1× RX2, 1× TX) | cmbuffs.c:31 (start_cmthread) | MMCSS Pro Audio @ 2 | semaphore INFINITE | HL2 wire clock (48 kHz / 126 = ~2.625 ms producer-paced) |
| MetisReadThreadMain | netInterface.c:61 | (need to verify priority) | WSAWaitForMultipleEvents on `hDataEvent` (line 433-434) | EP6 datagram arrival |
| sendProtocol1Samples | netInterface.c:66 | **MMCSS Pro Audio @ 2** (line 1207-1208) | WaitForMultipleObjects on 2 events, AND, INFINITE | both LR + IQ producers signal |
| KeepAliveMain | netInterface.c:83 (P2 only on HL2 path? need verify) | TBD | TBD | TBD |

## Full picture

```
                  ┌──────────────────────────────────────────────────────┐
                  │  HL2 hardware: 48 kHz crystal, AK4951 codec, EP6+EP2 │
                  └────────────┬─────────────────────────────────────────┘
                               │ UDP EP6 datagrams (RX1 IQ + RX2 IQ + mic)
                               ▼
                  ┌──────────────────────────────────┐
                  │  MetisReadThreadMain (wire-recv) │
                  │  netInterface.c:61               │
                  └────────┬─────────┬──────────────┬┘
                  Inbound(RX1)  Inbound(RX2)  Inbound(TX) ← mic from EP6!
                           ▼         ▼               ▼
                  ┌─────────┐  ┌─────────┐  ┌─────────┐
                  │ RX1 cmb │  │ RX2 cmb │  │ TX cmb  │  (cmbuffs ring + semaphore)
                  └────┬────┘  └────┬────┘  └────┬────┘
                       │ wake       │ wake        │ wake
                       ▼            ▼             ▼
                  ┌─────────┐  ┌─────────┐  ┌─────────────┐
                  │ cm_main │  │ cm_main │  │  cm_main    │  (MMCSS Pro Audio)
                  │ RX1     │  │ RX2     │  │  TX         │
                  │ xcmaster│  │ xcmaster│  │  xcmaster   │
                  │ case 0  │  │ case 0  │  │  case 1     │
                  │ ↓       │  │ ↓       │  │  ↓ asioIN   │  (or InboundTCITxAudio)
                  │ RXA     │  │ RXA     │  │  ↓ TXA      │
                  │ ↓       │  │ ↓       │  │  ↓ Outbound │ ──┐
                  │ aamix   │  │ aamix   │  └─────────────┘   │
                  └────┬────┘  └────┬────┘                    │
                       │            │                         │
                       └────┬───────┘                         │
                            ▼                                 │
                  ┌───────────────────┐                       │
                  │  Global aamix     │                       │
                  │  Outbound→LR evt  │ signals               │
                  └───────────┬───────┘                       │
                              │                               │
                              ▼                               ▼
                  ┌──────────────────────────────────────────────┐
                  │  sendProtocol1Samples (wire-send)           │
                  │  WaitForMultipleObjects(LR, IQ, AND, ∞)     │
                  │  MOX-gate IQ to zero if !XmitBit            │
                  │  CW state bits in TX I-LSBs if cw_enable    │
                  │  Quantize float→int16                       │
                  │  WriteMainLoop_HL2(outbuf)                  │
                  └──────────────┬───────────────────────────────┘
                                 │ UDP EP2 datagram (L+R audio + TX I/Q)
                                 ▼
                  ┌────────────────────────┐
                  │  HL2 hardware: TX wire │
                  └────────────────────────┘
```

The entire architecture: **one HL2 hardware clock**, **one wire-recv
thread + N stream producers**, **N consumer cm_main threads**, **one
wire-send thread driven by AND of two events**. No polling. No
sleep. No timer. Just semaphores and Win32 events.

## What §3 of the mapping doc gets

The §3 update closes Open Questions #1 (TX semaphore producer = wire-
recv thread via mic-on-EP6) and #2 (sendProtocol1Samples cadence =
two-event AND wait, MMCSS Pro Audio). Open Question #3 (where does
`Thread.Sleep(2)` exist if anywhere) remains — possibly TCIServer.cs
CHRONO pull loop; verify in next read.

## What §8 of the mapping doc gets (EP2 wire path) — starting points

Captured in this read:
- EP2 frame structure: 2× USB blocks × 504 bytes payload = 63 LRIQ
  tuples × 8 bytes per block; per-tuple L (2B) + R (2B) + TX I (2B) +
  TX Q (2B), MSB-first int16
- Frame cadence: 126 samples per EP2 frame at 48 kHz = 2.625 ms
- MOX bit gating: `if (!XmitBit) memset(outIQbufp, 0, ...)` —
  single chokepoint, wire-side safety
- CW packing: overwrites TX I bytes when `cw_enable && j==1`
- Quantize: float → int16 round-to-nearest
- HL2 dispatch: `WriteMainLoop_HL2` (different from non-HL2)

Still to capture for §8:
- Full HL2 P1 wire-frame layout (`WriteMainLoop_HL2` internals,
  C&C round-robin)
- The PA-enable bit (frame-10 C2 bit 3 — already established from
  the HL2+ gateware RTL reads in CLAUDE.md §15.26)
- TX step-attenuator wire encoding (the `31-x` issue — already
  established from CLAUDE.md §15.26 / commit `73a459b`)
- TXNCO / frequency writes ordering on MOX edges (§15.25)
- The ForceCandCFrame priming sequence

These will come from reading `WriteMainLoop_HL2` next.

*File written 2026-06-04 during Phase 0 read.*
