# TCI TX-Audio Path — End-to-End Reference vs Lyra Comparison

**Date**: 2026-06-04
**Scope**: Every stage of the TCI TX-audio path, from the inbound
WebSocket binary frame to the EP2 wire I/Q output, side-by-side
between the working reference implementation and lyra-cpp.

**Origin**: operator-driven re-audit after the 2026-06-03 EOD audit
dossier (`THETIS_VS_LYRA_RECONCILED.md`) missed two load-bearing
divergences (L+R stereo decode and CHRONO tick rate) that produced
the FT8 zero-PSKReporter-spots symptom across multiple bench
cycles.  This document is the **complete** line-by-line comparison
of the TCI TX path that should have been done from the start.

**Use it before adding any TCI TX code**: every new feature touches
this path, and the §15.26 PS-entangled discipline says we don't
patch deviations one at a time on this surface.

---

## Stage 0 — Handshake / Negotiation

Operator-visible: a TCI client (MSHV / JTDX / WSJT-X) connects and
exchanges text commands declaring its audio-format expectations.

| Concern | Reference (file:line) | Lyra (file:line) | Match |
|---|---|---|---|
| Default `audio_stream_channels` | `TCIServer.cs:781` `= 2` | `tci_server.h:267` `requestChannels_ = 2` | ✓ post-fix |
| Default `audio_stream_samples` | `TCIServer.cs:780` `= 2048` | `tci_server.h:258` `requestSamples_ = 2048` | ✓ |
| Default `tx_stream_audio_buffering` | `TCIServer.cs:784` `= 50` ms | `tci_server.h:259` `bufferingMs_ = 50` | ✓ |
| Default `audio_samplerate` | `TCIServer.cs:783` `= 48000` | `tci_server.h:257` `requestRate_ = 48000` | ✓ |
| Parse inbound `audio_samplerate:N` | `TCIServer.cs:5037` | `tci_server.cpp:1216-1217` | ✓ |
| Parse inbound `audio_stream_samples:N` | `TCIServer.cs:5054` | `tci_server.cpp:1218-1219` | ✓ |
| Parse inbound `audio_stream_channels:N` | `TCIServer.cs:5037 case` + `:5935-5949` handleAudioStreamChannels | `tci_server.cpp:1220-1224` post-fix | ✓ post-fix |
| Parse inbound `audio_stream_sample_type:fmt` | `TCIServer.cs:5042 case` + `:5912-5933` (sets `m_seenModernTxAudioNegotiation`) | `tci_server.cpp:1225-1229` post-fix (sets `seenModernTxNeg_`) | ✓ post-fix |
| Parse inbound `tx_stream_audio_buffering:N` | `TCIServer.cs:5043 case` + `:5990-5998` | `tci_server.cpp:1230-1231` | ✓ |
| Clamp `audio_stream_channels` to {1, 2} | `TCIServer.cs:5942` | `tci_server.cpp:1222` post-fix | ✓ post-fix |
| Clamp `bufferingMs` to floor of 50 | `cmaster.cs:1297` | `tci_server.cpp:1231` `std::max(n, 50)` | ✓ |
| Echo modern-negotiation flag | `TCIServer.cs:5928 / 5946` | `tci_server.cpp:1224 / 1229` post-fix | ✓ post-fix |
| Echo back all five parameters | `:5045-5050 / 5919-5949 / 5990-5998` | `tci_server.cpp:1233` | ✓ |

**Before the 2026-06-04 fix**, Lyra didn't parse inbound
`audio_stream_channels` or `audio_stream_sample_type` at all.  A
client telling Lyra "I want channels=2" was silently ignored.

---

## Stage 1 — WebSocket Binary Frame In (Decode + L+R)

A TCI client sends a binary frame containing FT8/SSB audio samples.

| Concern | Reference (file:line) | Lyra (file:line) | Match |
|---|---|---|---|
| Frame header size | `TCIServer.cs:5618` `dataOffset = 64` | `tci_server.cpp:665` `dataOffset = 64` | ✓ |
| Sample-type byte | `TCIServer.cs:5609` `BitConverter.ToUInt32(payload, 8)` | `tci_server.cpp:659` `getU32(p + 8)` | ✓ |
| Length byte | `TCIServer.cs:5610` `BitConverter.ToInt32(payload, 20)` | `tci_server.cpp:660` `getU32(p + 20)` | ✓ |
| Channels byte | `TCIServer.cs:5612` `BitConverter.ToInt32(payload, 28)` | `tci_server.cpp:662` `getU32(p + 28)` | ✓ |
| Modern-header channels validation | `:5628` `headerChannels == 1 ∥ == 2` | `tci_server.cpp:677` same | ✓ |
| Legacy-channels inference (payload size ≥ 2 × length) | `TCIServer.cs:5643-5646` | `tci_server.cpp:678-680` | ✓ |
| Pair-align decode count when channels == 2 | `TCIServer.cs:5636 / 5651` | `tci_server.cpp:686` | ✓ |
| Cap decode count at min(length, actualScalars) | `TCIServer.cs:5633 / 5648` | `tci_server.cpp:685` | ✓ |
| NaN/Inf sanitize → 0 | `TCIServer.cs:5660-5664` | `tci_server.cpp:132-133` | ✓ |
| Clamp to ±4.0 | `TCIServer.cs:5665-5672` | `tci_server.cpp:134-136` | ✓ |
| Mono path (channels == 1) | duplicates `value` into both `complex[2*i]` and `[2*i+1]` (`TCIServer.cs:5347-5351 convertStreamSamplesToComplex`) → later `(left+right)*0.5 = value` | `tci_server.cpp:139-141` push directly as mono float | ✓ functionally |
| **Stereo path — L+R averaging** | `cmaster.cs:1412-1416 queueTCITxAudio` default `Both` mode: `mono[i] = (float)((left + right) * 0.5)` | `tci_server.cpp:142-153` post-R-FT8: `mono.push_back(sanitize(0.5f * (l + r)))` | ✓ **post-2026-06-04 fix** (pre-fix was L-only) |
| Queue cap (drop-oldest) | `TCIServer.cs:763 MAX_TX_AUDIO_QUEUE_BLOCKS=64` + `:764 MAX_TX_AUDIO_QUEUE_COMPLEX_SAMPLES=96000` | `tci_mic_source.h:160 kInboundCapacity = 96000` samples | ✓ equivalent |
| Drop-oldest direction | `TCIServer.cs:5689-5701` dequeues from head | `tci_mic_source.cpp:45-58` erases from begin | ✓ |

**Before the 2026-06-04 R-FT8 fix**, Lyra's stereo path took
ONLY the left channel.  If MSHV placed FT8 audio in R or
distributed it L+R, half the energy was silently dropped — the
SSB modulator output became near-carrier-only.

---

## Stage 2 — Resample to TXA Input Rate

If the inbound `sampleRate` differs from the TXA channel's input
rate (48 kHz on this build), resample.

| Concern | Reference (file:line) | Lyra (file:line) | Match |
|---|---|---|---|
| Resample to TXA input rate | `cmaster.cs:1420` calls `resampleTCITxSamples` | `tci_server.cpp:703-715` calls `resampleTxIn` | ✓ |
| Resampler algorithm | `cmaster.cs:1431-1473` WDSP polyphase float-vector | Lyra calls the same WDSP `xresample`-class API | ✓ |
| Fast-path when rate == target | `cmaster.cs:1436-1437` `return input` | `tci_server.cpp:703` `if (sampleRate != kTciTxTargetRate)` | ✓ |
| Resample-failure fallback | reference logs + drops | `tci_server.cpp:705-714` warn-once + return | ✓ |

---

## Stage 3 — Operator TX-in Gain (Task #108)

Lyra-only operator stopgap (the reference doesn't have a
per-client TX-in gain knob; ops adjust at the client side).

| Concern | Reference | Lyra (file:line) | Match |
|---|---|---|---|
| Operator TX-in gain stage | — | `tci_server.cpp:740-744` applies `txGainLinear_` once at audio-block ingress | Lyra-additive (declared deviation, behind operator default 0 dB) |

---

## Stage 4 — Inbound Queue → Worker Ring

Reference stores raw decoded samples in `m_txAudioQueue`, drains
+ resamples + averages-L+R at the 2 ms TX pump tick into
`m_tciTxSampleQueue`, which the DLL callback later pulls from.

Lyra stores already-mono-already-resampled samples in
`TciMicSource::inbound_` (Qt main thread), and a QTimer-paced
drain pushes them to `TxDspWorker::ring_` (worker thread).

| Concern | Reference (file:line) | Lyra (file:line) | Match |
|---|---|---|---|
| Stage architecture | 2 queues (`m_txAudioQueue` raw + `m_tciTxSampleQueue` post-resample mono), 2 ms tick services both | 1 queue (`inbound_` post-decode-post-resample-mono), 2 ms timer drains to SPSC ring | functionally equivalent |
| Cross-thread sync | DLL callback `OnTCITxAudioInSamples` invoked by native ChannelMaster thread | SPSC ring (lock-free) between Qt main and worker threads | architecture differs, both correct |
| **Drain timer cadence** | `cmaster.cs:1253` `Thread.Sleep(2)` = 2 ms | `tci_mic_source.h:166` `kDrainIntervalMs = 2` post-fix | ✓ **post-2026-06-04 fix** (pre-fix was 10 ms with default CoarseTimer drifting 10-20 ms) |
| **Drain timer precision** | native thread sleep, kernel-bound | `tci_mic_source.cpp:30` `setTimerType(Qt::PreciseTimer)` post-fix | ✓ **post-2026-06-04 fix** (pre-fix Qt default CoarseTimer) |
| Drain batch sizing | reference pulls whatever's there each tick | `tci_mic_source.h:177` `kDrainMaxSamples = 96` post-fix (= 48 kHz × 2 ms) | ✓ equivalent |
| Mono sample I/Q duplication into DSP buffer | `cmaster.cs:1791-1792` `data[2*n]=sample; data[2*n+1]=sample;` | `tx_channel.cpp:454-458` `inBuf_[2i+0]=s; inBuf_[2i+1]=s;` | ✓ |
| Silence-fill on starvation | `cmaster.cs:1806-1810` zero-fill the data buffer | `tx_dsp_worker.cpp:198-202` worker `popBlock` blocks until samples ready (EP2 packer zero-fills on TX-IQ ring underrun) | architecturally differs but both produce silence-on-the-wire on starvation |

---

## Stage 5 — CHRONO Outbound Pull Request

The TXA pump tick computes how far ahead the inbound queue is vs
the target buffer depth, and sends `requestsNeeded` CHRONO frames
to the client asking it to push more audio.

| Concern | Reference (file:line) | Lyra (file:line) | Match |
|---|---|---|---|
| **Tick cadence** | `cmaster.cs:1253` `Thread.Sleep(2)` = 2 ms | `tci_server.h:285` `kChronoIntervalMs = 2` post-fix | ✓ **post-2026-06-04 fix** (pre-fix was 50 ms — 25× slower) |
| **Tick timer precision** | native thread sleep | `tci_server.cpp:262` `setTimerType(Qt::PreciseTimer)` post-fix | ✓ **post-2026-06-04 fix** |
| Max outstanding | `cmaster.cs:481` `TCI_TX_MAX_OUTSTANDING = 64` | `tci_server.h:243` `kTciTxMaxOutstanding = 64` | ✓ |
| Outstanding-timeout reset | `cmaster.cs:1329` `max(250, bufferingMs * 4)` ms | `tci_server.cpp:781` `std::max(kChronoTimeoutMs, bufferingMs_ * 4)` | ✓ |
| Decrement outstanding on inbound frame | `cmaster.cs:1303-1304` | `tci_server.cpp:653` | ✓ |
| `txBlock` floor | `cmaster.cs:1313 GetBuffSize(targetRate)` fallback 720 | `tci_server.h:242` `kTciTxBlockSamples = 64` (post-R-H1, reference rule `64 * rate / 48000` at 48 kHz) | ✓ |
| `predictedPacketSamples` formula | `cmaster.cs:1317-1319` `max(txBlock, ceil(requestSamples * targetRate / requestRate))` | `tci_server.cpp:801-804` same | ✓ |
| `targetQueuedSamples` formula | `cmaster.cs:1320-1322` `max(txBlock*4, ceil((bufferingMs + EXTRA) * targetRate / 1000))` | `tci_server.cpp:809-812` same | ✓ |
| `EXTRA_BUFFER_MS` constant | `cmaster.cs:482` `= 50` | `tci_server.h:244` `kTciTxExtraBufferMs = 50` | ✓ |
| `futureSamples = queued + outstanding * predicted` | `cmaster.cs:1336` | `tci_server.cpp:820-822` | ✓ |
| `requestsNeeded = ceil(gap / predicted)` | `cmaster.cs:1337-1339` | `tci_server.cpp:825-830` | ✓ |
| Per-tick burst loop (bounded by max outstanding) | `cmaster.cs:1341-1359` | `tci_server.cpp:850-870` | ✓ |
| **CHRONO frame `channels` field** | `cmaster.cs:1357 SendTxChrono(receiver)` → `TCIServer.cs:5526 channels = m_audioStreamChannels` | `tci_server.cpp:866` post-fix uses `requestChannels_` | ✓ **post-2026-06-04 fix** (pre-fix hardcoded 1) |
| **CHRONO frame `length` field (modern semantics)** | `TCIServer.cs:5531` `requestLength = useModern ? samples * channels : samples` | `tci_server.cpp:861-864` post-fix mirrors | ✓ **post-2026-06-04 fix** (pre-fix `length = predictedPacketSamples` regardless) |

**Before the 2026-06-04 fixes**, Lyra's CHRONO pump ticked at
50 ms (20 Hz) instead of 2 ms (500 Hz) — 25× slower — AND
requested channels=1 frames even when the negotiated channels=2.
The combined effect: MSHV produced audio at a rate matching
Lyra's requests, which was insufficient to keep the TXA chain
fed at the wire's 48 kHz consumption rate, producing the
"tones then carrier then tones" alternating panadapter display.

---

## Stage 6 — TXA chain (WDSP)

Once samples reach the TXA channel's `fexchange0`, WDSP runs its
own internal SSB modulator chain (bp0 → ALC → leveler → output).
This is shared code between reference + Lyra (both call into the
same WDSP DLL).  No divergence at this layer.

| Concern | Reference (file:line) | Lyra (file:line) | Match |
|---|---|---|---|
| WDSP TXA in_size rule | `getbuffsize(rate) = 64 * rate / 48000`, yields 64 at 48 kHz | `tx_channel.cpp:65` `kInSize = 64` post-R-H1 | ✓ post-R-H1 |
| `kBlockSize` (worker drain) | n/a (reference uses pull-callback) | `tx_dsp_worker.h:97` `kBlockSize = 64` post-R-H1 | ✓ post-R-H1 |
| `kTciTxBlockSamples` (CHRONO floor) | `cmaster.cs:1313 GetBuffSize` | `tci_server.h:242` `kTciTxBlockSamples = 64` post-R-H1 | ✓ post-R-H1 |
| `OpenChannel(in_rate, dsp_rate, out_rate)` | typical `48000 / 96000 / 48000` for HL2 | `tx_channel.cpp:148-154` `(48000, 96000, 48000)` | ✓ |
| WDSP I/Q output buffer layout | interleaved doubles `[I,Q,I,Q,...]` | `tx_channel.cpp:474-478` same | ✓ |
| Mode/filter push order (`SetTXAMode` then `SetTXABandpassFreqs`) | `radio.cs:2692/2738` | `tx_channel.cpp` `_pushModeLocked` then `_pushBandpassLocked` | ✓ |

---

## Stage 7 — Output to EP2 wire

Worker assembles WDSP TXA output samples into EP2 frame chunks
(126 stereo samples / 504 bytes) and hands them off to the EP2
packer thread for transmission.

This stage is HL2-specific (the reference targets all HPSDR
hardware via ChannelMaster); architectural divergence here is
expected.  No FT8-relevant divergences known.

---

## Summary of divergences closed by the 2026-06-04 arc

1. **R-H1** — `kInSize` constants 128→64 (reference rule
   `64 * rate / 48000`) [commit `1002d32`]
2. **R-H2** — TCI keydown token-agnostic mic-source force
   (defensive; not the FT8 root) [commit `e902f31`]
3. **R-FT8** — Stereo decode L+R average (was L-only — THE
   first symptom-mover) [commit `8cddfa5`]
4. **CHRONO tick rate** — 50 ms → 2 ms with PreciseTimer
   (was 25× slower than reference) [commit `fe0ac2d`]
5. **Drain tick rate** — 10 ms → 2 ms with PreciseTimer
   (was 5× slower than chrono; could starve worker ring)
   [this commit]
6. **CHRONO request channels** — hardcoded 1 → negotiated
   (default 2, parsed from `audio_stream_channels:N`)
   [this commit]
7. **CHRONO request length** — modern length semantics
   (`samples * channels`) when client used modern handshake
   [this commit]
8. **`audio_stream_channels` parser** — added inbound handling
   so the negotiated value reaches CHRONO send [this commit]
9. **`audio_stream_sample_type` parser** — added inbound
   handling so the modern-negotiation flag flips [this commit]

## What this document is for going forward

Any new TCI TX feature work MUST verify against this document
first.  If a new code path touches any stage 1-5, the relevant
table row must be re-checked for reference parity BEFORE the code
ships.  No more piecemeal patching of newly-discovered
divergences.
