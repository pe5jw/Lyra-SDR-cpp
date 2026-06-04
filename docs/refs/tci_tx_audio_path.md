# TCI TX-audio path

**Sources read 2026-06-04:**
- `Console/TCIServer.cs:5510-5703` — `SendTxChrono`, `handleBinaryFrame`,
  `TryDequeueTxAudio`, related TX-audio handlers
- `Console/TCIServer.cs:5339-5362` — `convertStreamSamplesToComplex`
- `Console/TCIServer.cs:1680-1707` — `flushOutboundFrames` (close Q#1)
- `Console/cmaster.cs:1131-1830` — TCI C# bridge (threads, callbacks,
  queues, chrono formula, format conversion)

---

## Architecture overview

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │  TCI Client (MSHV / JTDX / WSJT-X / etc.)                          │
 └──────────────────────────────┬──────────────────────────────────────┘
                                │ WebSocket
   ┌────────────────────────────┼──────────────────────────────┐
   │  text frames (handshake, controls)        binary frames    │
   │  audio_samplerate, audio_stream_channels,  (TX_AUDIO_STREAM:│
   │  audio_stream_samples,                      stereo float    │
   │  tx_stream_audio_buffering,                 samples)        │
   │  trx:0,true,tci  ← keydown via TCI         │                │
   │  TX_CHRONO ← pull request                  ▼                │
   └────────────────────────────┬───────────────────────────────┘
                                │
                                ▼
 ┌─────────────────────────────────────────────────────────────────────┐
 │  TCIServer.cs (C#, in-Thetis WebSocket handler)                    │
 │                                                                     │
 │  handleBinaryFrame:                                                 │
 │    parse header  →  convertStreamSamplesToComplex(samples, channels)│
 │                       (mono: I=Q=mono ; stereo: I=L, Q=R)           │
 │                                                                     │
 │  Enqueue into m_txAudioQueue                                        │
 │    capacity: MAX_TX_AUDIO_QUEUE_BLOCKS / MAX_TX_AUDIO_QUEUE_COMPLEX_ │
 │              SAMPLES                                                │
 │    overflow: drop-OLDEST (dequeue from front)                       │
 └─────────────────────────────────────────────┬───────────────────────┘
                                               │
                                               ▼
 ┌─────────────────────────────────────────────────────────────────────┐
 │  cmaster.cs — TCITxThreadProc (C# supervisory thread, Above Normal) │
 │                                                                     │
 │  while (m_runTCIStreamThreads) {                                    │
 │      ServiceTCITxProtocol();                                        │
 │      Thread.Sleep(2);   ← this is THE Thread.Sleep(2)               │
 │  }                                                                  │
 │                                                                     │
 │  ServiceTCITxProtocol:                                              │
 │    if (!MOX || !UsesActiveTCITxAudio): reset state, return          │
 │    dequeue from m_txAudioQueue (in TCIServer.cs)                    │
 │    for each: queueTCITxAudio(queued, targetRate, stereoMode):       │
 │      mono[i] = mode==Left?L : mode==Right?R : (L+R)*0.5             │
 │      resample mono → targetRate (WDSP xresampleFV)                  │
 │      enqueue into m_tciTxSampleQueue (mono, target-rate)            │
 │    compute predictedPacketSamples + targetQueuedSamples             │
 │    futureSamples = queuedSamples + outstanding * predicted          │
 │    requestsNeeded = ceil((target - future) / predicted)             │
 │    while (requestsNeeded-- && outstanding < TCI_TX_MAX_OUTSTANDING):│
 │      tciServer.SendTxChrono(receiver);  ← sends TX_CHRONO to client │
 │      outstanding++                                                   │
 └────────────────────┬────────────────────────────────────────────────┘
                      │ m_tciTxSampleQueue (mono float, target rate)
                      ▼
 ┌─────────────────────────────────────────────────────────────────────┐
 │  C++ ChannelMaster — cm_main TX thread (semaphore-blocked,          │
 │  MMCSS Pro Audio @ 2)                                               │
 │                                                                     │
 │  wakes on EP6 mic-on-cmbuffs Inbound()                              │
 │  drains TX cmbuffs → pcm->in[stream]                                │
 │  calls xcmaster(stream)                                             │
 │    asioIN (no-op for HL2)                                           │
 │    if (use_tci_audio):                                              │
 │      pcm->InboundTCITxAudio(insize, pcm->in[stream])                │
 │      ↓ (this is the C# delegate registered at cmaster.cs:1139)      │
 │      OnTCITxAudioInSamples(nsamples, data):                         │
 │        for each sample in m_tciTxSampleQueue (up to nsamples):      │
 │          data[2*i]   = sample      // I = mono                      │
 │          data[2*i+1] = sample      // Q = mono                      │
 │        if queue empty:                                              │
 │          data[2*i] = data[2*i+1] = 0.0   // zero-fill underrun      │
 │    xpipe → xdexp → fexchange0 (WDSP TXA) → ... → Outbound to wire   │
 └─────────────────────────────────────────────────────────────────────┘
```

## Q#1 — `Thread.Sleep(2)` LOCATION CLOSED

`cmaster.cs:1253` in `TCITxThreadProc`:

```csharp
private static void TCITxThreadProc()
{
    while (m_runTCIStreamThreads)
    {
        try { ServiceTCITxProtocol(); } catch { }
        Thread.Sleep(2);            // <-- THE 2 ms sleep
    }
}
```

This is a **C# supervisory thread** that runs the CHRONO pull state
machine at 500 Hz poll cadence. It is NOT the DSP path. The DSP path
is still `cm_main` semaphore-blocked, paced by HL2 EP6 mic-sample
arrivals (the HL2 wire clock).

The `Thread.Sleep(2)` controls how often Thetis *decides whether to
send CHRONO requests*. The actual CHRONO send rate is determined by
the formula (predicted/target/outstanding math), not by the 2 ms
tick. Most of the 2 ms ticks fire `ServiceTCITxProtocol`, find
nothing to do, and return.

There's also `Thread.Sleep(5)` at `TCIServer.cs:1705` in
`flushOutboundFrames` — a shutdown-only polling helper, irrelevant
to the hot path.

And `Thread.Sleep(5)` at `cmaster.cs:1208` in `TCIRxThreadProc` —
the C# RX supervisory loop (services TCI RX IQ + audio outbound).
RX is allowed a slower poll because outbound packetization is less
latency-sensitive.

**Conclusion:** the reference uses `Thread.Sleep(N)` ONLY on C#
supervisory threads that manage protocol/queue state. The audio
PATH is fully producer-paced (kernel semaphores + Win32 events,
INFINITE timeouts, no polling).

## Q#2 — TCI sample format CLOSED

`TCIServer.cs:5339-5362 convertStreamSamplesToComplex`:

```csharp
private static double[] convertStreamSamplesToComplex(float[] samples, int channels)
{
    if (channels < 1) channels = 1;
    int complexSamples = channels <= 1 ? samples.Length : samples.Length / channels;
    double[] complex = new double[complexSamples * 2];
    if (channels == 1) {
        for (int i = 0; i < complexSamples; i++) {
            double value = samples[i];
            complex[2 * i] = value;            // I = mono
            complex[2 * i + 1] = value;        // Q = mono (SAME)
        }
    } else {
        for (int i = 0, j = 0; i < complexSamples; i++, j += channels) {
            complex[2 * i] = samples[j];        // I = L
            complex[2 * i + 1] = samples[j + 1]; // Q = R
        }
    }
    return complex;
}
```

`cmaster.cs:1788-1793 OnTCITxAudioInSamples` (the callback that
delivers TCI audio to pcm->in[stream]):

```csharp
for (int i = 0; i < toCopy; i++) {
    double sample = block[m_tciTxSampleQueueOffset + i];
    data[2 * (copied + i)]     = sample;       // I = mono
    data[2 * (copied + i) + 1] = sample;       // Q = mono (SAME)
}
```

**Three input shapes, three layouts entering pcm->in[stream]:**

| Source | I | Q |
|---|---|---|
| HL2 codec mic (networkproto1.c:404-407) | mic value | **0.0** |
| TCI mono in (convertStreamSamplesToComplex + OnTCITxAudioInSamples) | mono value | **mono value (= I)** |
| TCI stereo in (convertStreamSamplesToComplex L→I, R→Q ; then queueTCITxAudio averages or picks → mono; then OnTCITxAudioInSamples writes I=Q=mono) | mono value (post-average/pick) | mono value (= I) |

**All three work because TXA's panel stage uses `inselect=2`
(use I for input)** — panel reads I, discards Q. So Q=0 vs Q=I
doesn't matter downstream.

**Lyra implication:** the TCI path must write **I=Q=mono** into the
TX channel input buffer (matches reference's `OnTCITxAudioInSamples`).
Task #67's framing was Thetis-faithful for the TCI side. The
HL2-codec-mic path writes I=mic, Q=0 — that's the protocol path,
not the TCI path. Both must coexist correctly; both feed
fexchange0; both produce identical TXA output because panel
discards Q.

This is a **§11 declared structural detail** — not a deviation,
but a non-obvious property that future code must not break.

## The CHRONO pull formula (cmaster.cs:1313-1359)

```csharp
int txBlock = GetBuffSize(targetRate);            // = 64 * targetRate / 48000
                                                   // (the WDSP block size at TX
                                                   //  input rate)
if (txBlock <= 0) txBlock = 720;

int predictedPacketSamples = Math.Max(
    txBlock,
    (int)Math.Ceiling((double)requestSamples * targetRate / Math.Max(1, requestRate)));

int targetQueuedSamples = Math.Max(
    txBlock * 4,
    (int)Math.Ceiling((bufferingMs + TCI_TX_EXTRA_BUFFER_MS) * targetRate / 1000.0));

int queuedSamples = m_tciTxQueuedSamples;
int outstanding  = m_tciTxChronoOutstanding;

// timeout: if no chrono response in (bufferingMs * 4) ms, assume lost
//          and clear outstanding so we don't deadlock
if (outstanding > 0 && nowMs - m_tciTxLastChronoTick > Math.Max(250, bufferingMs * 4))
    outstanding = 0;

int futureSamples = queuedSamples + outstanding * predictedPacketSamples;
int requestsNeeded = futureSamples < targetQueuedSamples
    ? (int)Math.Ceiling((double)(targetQueuedSamples - futureSamples) / predictedPacketSamples)
    : 0;

while (requestsNeeded > 0) {
    if (outstanding < TCI_TX_MAX_OUTSTANDING) {
        outstanding++;
        m_tciTxLastChronoTick = nowMs;
        tciServer.SendTxChrono(receiver);
        requestsNeeded--;
    } else break;
}
```

**The formula matches Lyra Task #64 exactly** — operator-faithful.
Lyra's task title was right; the prior implementation just lived in
a single-thread QTimer-paced drain instead of a proper supervisory
thread.

Defaults:
- `requestSamples = 480` (when client hasn't set `audio_stream_samples`)
- `requestRate = 48000` (when client hasn't set `audio_samplerate`)
- `bufferingMs = 50` (clamped to ≥50)
- `TCI_TX_EXTRA_BUFFER_MS` — additional safety buffer (TBD; need to
  read the constant definition)
- `TCI_TX_MAX_OUTSTANDING` — max concurrent unfulfilled chrono
  requests (TBD)
- `txBlock = 64 * targetRate / 48000` = WDSP's per-call block size

## The CHRONO request itself (TCIServer.cs:5515-5533)

```csharp
internal void SendTxChrono(int receiver)
{
    int sampleRate, samples, channels;
    TCISampleType sampleType;
    bool useModernLengthSemantics;
    lock (m_objStreamLock) {
        sampleRate                = m_audioSampleRate;
        samples                   = m_audioStreamSamples;
        channels                  = m_audioStreamChannels;
        sampleType                = m_audioSampleType;
        useModernLengthSemantics  = m_seenModernTxAudioNegotiation;
    }

    int requestLength = useModernLengthSemantics
        ? samples * Math.Max(1, channels)        // modern: samples * channels
        : samples;                                // legacy: just samples

    sendBinaryFrame(buildStreamPayload(
        receiver, sampleRate, sampleType,
        requestLength,
        TCIStreamType.TX_CHRONO,
        channels,
        Array.Empty<byte>()));
}
```

**Modern vs legacy length semantics:**
- Modern (client sent any audio_stream_* setup): `length = samples * channels`
- Legacy (JTDX old behavior): `length = samples`

`m_seenModernTxAudioNegotiation` flips to `true` on lines 5930, 5946,
5963, 5996 when the client sends any of the `audio_stream_*` setup
commands. (Need to read those handlers for full handshake — separate
read.)

## TXStereoInputMode handling (cmaster.cs:1391-1418)

`queueTCITxAudio` bridge function — converts TCIServer's complex[]
to cmaster's mono float[]:

```csharp
float[] mono = new float[complexSamples];
if (queuedAudio.Channels <= 1) {
    for (int i = 0; i < complexSamples; i++)
        mono[i] = (float)queuedAudio.Samples[2 * i];     // just take I (Q is the same anyway)
} else {
    for (int i = 0; i < complexSamples; i++) {
        double left  = queuedAudio.Samples[2 * i];        // I = L
        double right = queuedAudio.Samples[2 * i + 1];    // Q = R
        switch (stereoInputMode) {
            case TCITxStereoInputMode.Left:   mono[i] = (float)left;                  break;
            case TCITxStereoInputMode.Right:  mono[i] = (float)right;                 break;
            case TCITxStereoInputMode.Both:                       // DEFAULT
            default:                          mono[i] = (float)((left + right) * 0.5); break;
        }
    }
}

float[] output = resampleTCITxSamples(mono, queuedAudio.SampleRate, targetRate);
// resamples via WDSP xresampleFV from input rate to TXA input rate

lock (m_objTCITxStateLock) {
    m_tciTxSampleQueue.Enqueue(output);
    m_tciTxQueuedSamples += output.Length;
}
```

**The L+R averaging at `mono[i] = (left + right) * 0.5`** is exactly
Lyra's R-FT8 fix from earlier today, source-verified.

**TXStereoInputMode default = Both** (TCIServer.cs:6113:
`private TCITxStereoInputMode m_txStereoInputMode = TCITxStereoInputMode.Both;`),
operator-toggleable via `TXStereoInputMode` property at line 6249.

## OnTCITxAudioInSamples (cmaster.cs:1774-1812) — the consumer callback

This is what xcmaster's TX dispatch calls when `use_tci_audio==1`:

```csharp
private static unsafe void OnTCITxAudioInSamples(int nsamples, double* data)
{
    if (data == null || nsamples <= 0) return;

    lock (m_objTCITxStateLock) {
        int copied = 0;
        while (copied < nsamples && m_tciTxSampleQueue.Count > 0) {
            float[] block = m_tciTxSampleQueue.Peek();
            int available = block.Length - m_tciTxSampleQueueOffset;
            int toCopy = Math.Min(nsamples - copied, available);

            for (int i = 0; i < toCopy; i++) {
                double sample = block[m_tciTxSampleQueueOffset + i];
                data[2 * (copied + i)]     = sample;          // I = mono
                data[2 * (copied + i) + 1] = sample;          // Q = mono
            }

            copied += toCopy;
            m_tciTxSampleQueueOffset += toCopy;
            m_tciTxQueuedSamples -= toCopy;

            if (m_tciTxSampleQueueOffset >= block.Length) {
                m_tciTxSampleQueue.Dequeue();
                m_tciTxSampleQueueOffset = 0;
            }
        }

        // Underrun: zero-fill remaining samples
        for (int i = copied; i < nsamples; i++) {
            data[2 * i]     = 0.0;
            data[2 * i + 1] = 0.0;
        }
    }
}
```

**Underrun policy:** zero-fill. If the TCI client isn't keeping up
(network jitter, client-side starvation), the TXA chain processes
zeros for the missing samples. No retry, no block, no extrapolate
— just silence. The CHRONO pull formula's job is to keep the queue
ahead so this rarely happens.

**Lock:** single mutex `m_objTCITxStateLock` protects both the queue
and the chrono-outstanding state. Lock held briefly per call (not
across DSP processing). Lyra mirror: `std::mutex` with the same
discipline (lock briefly, never hold across DSP work).

## Two-queue model

| Queue | Owner | Format | Producer | Consumer | Overflow |
|---|---|---|---|---|---|
| `m_txAudioQueue` | TCIServer (per-listener) | `TCIQueuedTxAudio` with `Samples` as raw complex (I=Q=mono or I=L,Q=R) at client's sample rate | TCI listener's `handleBinaryFrame` (per TX_AUDIO_STREAM binary frame received) | `TCITxThreadProc` via `TryDequeueTxAudio` | drop-OLDEST when over MAX_TX_AUDIO_QUEUE_BLOCKS or MAX_TX_AUDIO_QUEUE_COMPLEX_SAMPLES |
| `m_tciTxSampleQueue` | cmaster.cs (global) | mono float[] at TXA target rate (= GetInputRate(1,0), typically 48000) | `queueTCITxAudio` (called from `TCITxThreadProc.ServiceTCITxProtocol`) | `OnTCITxAudioInSamples` (called from cm_main TX thread via `InboundTCITxAudio` cffi) | zero-fill on underrun (consumer side, not producer side) |

**The two-queue separation matters:** the first queue absorbs
client-side bursts (TCI clients send TX_AUDIO_STREAM in unaligned
chunks of variable size); the second queue is rate-converted and
ready-to-DSP. The supervisory thread bridges them; the DSP thread
just dequeues mono floats and writes complex.

## Defaults captured

| Field | Default | Where set |
|---|---|---|
| `m_audioSampleRate` | (negotiated; default likely 48000) | TCIServer.cs handshake |
| `m_audioStreamChannels` | (negotiated; default likely 2) | TCIServer.cs handshake |
| `m_audioStreamSamples` | (negotiated; default 480 if absent) | cmaster.cs:1296 fallback |
| `m_audioSampleType` | (negotiated; FLOAT32 typically) | TCIServer.cs handshake |
| `bufferingMs` | 50 minimum | cmaster.cs:1297 (`if (bufferingMs < 50) bufferingMs = 50;`) |
| `m_txStereoInputMode` | `TCITxStereoInputMode.Both` (L+R average) | TCIServer.cs:6113 |
| `m_seenModernTxAudioNegotiation` | false (until client sends any audio_stream_* setup) | TCIServer.cs:788 |

## Architecture mapping for Lyra

```
lyra::tci::Server                          // Qt6 QWebSocketServer wrapper
  per-client TciListener:
    m_rxAudioQueue / m_rxIQQueue           // (already exists / RX out of scope)
    m_txAudioQueue                         // RAW complex, drop-OLDEST, stereo-aware
    handleBinaryFrame()                    // decode TX_AUDIO_STREAM + enqueue
    SendTxChrono()                         // emit TX_CHRONO binary frame
    SendInit() / handshake                 // already exists

lyra::tci::TciTxSupervisorThread           // QThread, ~2 ms poll cadence
  while (running) {
    serviceTciTxProtocol();                // dequeue m_txAudioQueue,
                                            //   queueTciTxAudio (averaging + resample),
                                            //   compute chrono formula,
                                            //   send chronos
    QThread::msleep(2);
  }

lyra::tci::Bridge (singleton or member of TxChannel)
  m_tciTxSampleQueue                       // MONO float at TXA input rate
                                            // (deque<vector<float>>)
  queueTciTxAudio(complex, rate, stereoMode)  // L+R/L/R + resample
  OnTciTxAudioInSamples(nsamples, data)    // the cm_main callback equivalent —
                                            //   reads queue, writes I=Q=mono,
                                            //   zero-fills underrun

lyra::dsp::StreamThread (TX)               // the cm_main equivalent
  loop body:
    sem.acquire()                          // blocks on EP6 mic arrival
    drain stream input ring → in_buf
    if (use_tci_audio) Bridge::OnTciTxAudioInSamples(insize, in_buf)
                                            //   overrides in_buf with TCI samples
    TxChannel::process(in_buf, out_buf, &err)
```

The Lyra rip's TCI path is structurally these four objects.
**Critical:** the supervisory thread runs at 2 ms poll (matches
reference); the DSP thread runs semaphore-blocked (matches
reference). The 2 ms tick is supervisory-only, never gates audio.

## Open items remaining for §7

1. `m_seenModernTxAudioNegotiation` handshake — which `audio_stream_*`
   commands set it (lines 5930, 5946, 5963, 5996 — sub-reads needed).
2. `TCI_TX_EXTRA_BUFFER_MS` and `TCI_TX_MAX_OUTSTANDING` constants
   in cmaster.cs.
3. `MAX_TX_AUDIO_QUEUE_BLOCKS` and `MAX_TX_AUDIO_QUEUE_COMPLEX_SAMPLES`
   constants in TCIServer.cs.
4. The `Audio.MOX` gating at cmaster.cs:1260 — coupling between the
   C# MOX flag and the TCI supervisory thread.

*File written 2026-06-04 during Phase 0 read.*
