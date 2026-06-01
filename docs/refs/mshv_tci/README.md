# MSHV TCI client — ground truth for Task #33 wire-up

Operator-confirmed first-light client for Task #33's TCI mic-source
path: **MSHV** (LZ2HV — Multi-Stream HF Validator), installed at
`C:\MSHV\` on the operator's bench.  This README captures the
empirical findings off the actual install at the time #33 was
scoped (2026-06-01) so the implementation hits a known target
instead of guessing against the spec.

## MSHV-side TCI config (verbatim from `C:\MSHV\settings\ms_settings`)

```
default_device_alsa = TCI Client Input
default_out_dev     = TCI Client Output
default_rig_name    = TCI Client RX1
net_rig_srv_port    = ...#127.0.0.1;50001;1;2048;3;48000;50#...
```

`net_rig_srv_port` is a `#`-delimited list of per-rig configs; the
TCI entry decodes as:

| Field | Value | Meaning |
|---|---|---|
| 1 | `127.0.0.1` | Lyra host |
| 2 | `50001` | TCI port (matches Lyra's `TciServer::port_` default) |
| 3 | `1` | Receiver index (RX1; channel_count advertised by Lyra = 1) |
| 4 | `2048` | Block size (samples) |
| 5 | `3` | SampleType (per TCI v2 enum: `3 = FLOAT32`) |
| 6 | `48000` | Sample rate (Hz) |
| 7 | `50` | TX-stream buffering hint (ms) |

So MSHV is pre-configured to stream **FLOAT32 mono @ 48 kHz in 2048-
sample blocks** with a 50 ms TX buffering window.  Lyra's TCI binary
handler must decode that path; INT16 / INT24 / INT32 / 2-channel
remain spec'd but are not the operator's first-light surface.

A snapshot of `ms_settings` is filed alongside this README as
`ms_settings.txt` for archaeology.

## MSHV's TCI command surface (strings extracted from `MSHV_WIN64.exe`)

Confirmed token usage in the MSHV binary:

* `trx:` — TX/RX toggle (this is what fires Lyra's MOX path)
* `vfo:` — set VFO frequency
* `modulation:` — set mode (USB / DIGU / LSB / DIGL / CW / AM / FM)
* `split:` — split toggle
* `mute:` — mute control
* `drive` — set drive level
* `tx_enable` — per-trx TX enable
* `tx_freq` — TX frequency
* `audio_start` / `audio_stop` — audio stream lifecycle
* `tx_stream_audio_buffering` — set TX buffering timeout
* `TCI Client` / `TCI Server` — log strings (MSHV is the client,
  Lyra is the server)

MSHV sends commands in **lowercase**; Lyra's `TciServer::onTextMessage`
already uppercases at dispatch time (line 442 / 446), so the existing
UPPERCASE command handlers all match without changes.

## What Lyra's TCI server has today vs. what #33 adds

Already implemented (per `src/tci_server.cpp`):
* WebSocket server + handshake (`ready` / `start` / `stop` /
  `protocol;Lyra TCI Server,2,0;` / `device;Lyra TCI Server;`)
* `VFO:` / `IF:` / `DDS:` / `MODULATION:` / `RX_FILTER_BAND:` /
  `VOLUME:` / `MUTE:` / `CW_PITCH:` — all wired to live state
* `TRX:` / `TUNE:` / `RIT_ENABLE:` / `XIT_ENABLE:` / `SPLIT_ENABLE:`
  — parsed, acknowledged inactive (TX commands soft-store stub at
  line 578-583; Lyra-cpp is RX-only on the TCI surface today)
* `DRIVE:` / `TUNE_DRIVE:` / `MON_*` / `RIT/XIT_OFFSET:` /
  `KEYER:` / `CW_MSG:` / `CW_TERMINAL:` / `CW_KEYER_SPEED:` /
  `TX_STREAM_AUDIO_BUFFERING:` — soft-store at line 586-595
* `AUDIO_START` / `AUDIO_STOP` / `IQ_START` / `IQ_STOP` — wired to
  the outbound RX-audio / RX-IQ streamers
* `RX_SENSORS_ENABLE:` — wired (S-meter outbound)
* SPOT / SPOT_DELETE / SPOT_CLEAR — wired to SpotStore

Missing for the MSHV first-light path (this is what Task #33 adds):
* **Inbound binary frame handler.**  `QWebSocket::binaryMessageReceived`
  is NOT connected today (only `textMessageReceived`, line 247).
  Add `onBinaryMessage(QByteArray)` to parse the 64-byte Stream
  header + payload (TCI v2 spec §3.4).  Switch on `type`:
  * `TX_AUDIO_STREAM = 2` → decode payload by `format` × `channels`
    × `sample_rate`; resample to 48 kHz mono float32 if needed; push
    to TciMicSource's SPSC ring.  For DIGU/DIGL with channels=2,
    take the real part (Lyra TXA expects real audio).
  * Other types — ignore (RX_AUDIO_STREAM / IQ_STREAM are Lyra's
    outbound; TX_CHRONO / LINEOUT_STREAM aren't relevant inbound).
* **`TX_CHRONO` outbound emitter** (type=3) — periodic frame when
  `mic_source == tci` AND MOX is being requested.  Tells MSHV
  "send me N samples for the next window."  Cadence + length set by
  the operator's TX_STREAM_AUDIO_BUFFERING value (50 ms default
  per MSHV's config; Lyra-side default 150 ms per TCI spec).
* **TRX-to-MOX wire-up.**  Line 578-583 currently dispatches `TRX:`
  to a no-op stub.  Wire it to `radio.requestMox()` IFF
  `mic_source == tci` (safety: prevents a misconfigured client
  from keying when the operator has Mic In selected).
* **`TRX:0,true,tci` auto-source-swap.**  Per TCI v2 §3.3 the
  third arg is the mic-source token (`mic1` / `mic2` / `micpc` /
  `ecoder2` / `tci`).  When MSHV sends `trx:0,true,tci` it
  expects the radio to switch to the TCI audio source.  Lyra
  should honour that — automatically set `mic_source = tci`
  in Radio so the operator doesn't have to flip the Settings
  picker by hand.
* **Mic-source dispatcher** — `Radio::micSource()` / `setMicSource()`
  Q_PROPERTY; switching swaps which Source's Consumer is
  installed on TxDspWorker; QSettings `tx/mic_source` persist.
* **Settings UI picker** — Settings → TX gains a "Mic Source"
  dropdown (Mic In ✓ / TCI ✓ / Line In disabled / VAC1 disabled /
  VAC2 disabled).

## Safety posture

* Default `mic_source = mic1` (HL2 codec mic — current behavior).
* TCI client cannot key MOX unless operator has explicitly set
  `mic_source = tci` in Settings → TX **OR** MSHV sent
  `trx:0,true,tci` (which auto-flips the source).  In the second
  case the operator sees the Settings picker move and can revert
  if surprised.
* Per the project's §3.9 default-safe gating discipline: any
  EP6/EP2 byte CONSUMED (acted upon) must be either bench-
  verified at-rest-value OR behind an operator opt-in gate.
  TCI inbound TX audio is the latter — opt-in via Mic Source.

## Bench validation plan (first-light, ~30 min into a dummy load)

1. Build Lyra with Task #33 commits 1-4.
2. Configure MSHV (already done in operator's install — see
   `ms_settings.txt`).
3. Operator-side: Settings → TX → Mic Source = TCI.
4. Operator launches MSHV; MSHV connects to `127.0.0.1:50001`,
   Lyra TCI banner appears, MSHV control surface lights up.
5. MSHV → set band to 20m FT8 (14.074 USB), Lyra's VFO follows.
6. MSHV → "TX TUNE" or run an FT8 cycle; MSHV sends
   `trx:0,true,tci`, Lyra's MOX engages, MSHV streams
   `TX_AUDIO_STREAM` frames at 48 kHz / FLOAT32 / mono,
   Lyra's TX chain TXes via WDSP TXA.
7. Operator confirms on the Palstar AT2K (or external watt-
   meter) the carrier appears with FT8 tones.
8. Operator confirms no MOX-flap on TX_CHRONO timing, no
   ALC clip (commit 4 ships an ALC-saturated audio log
   line for forensic), no PA-current spike.
9. If all clean: bench gate green; commit 5 (User Guide
   walkthrough) lands.

## No-attribution rule observed

MSHV / LZ2HV / ExpertSDR / SunSDR names appear only in this
docs/refs/ tree.  Shipped code, comments, commits, and operator-
visible UI strings stay in first-principles "TCI" terms.
