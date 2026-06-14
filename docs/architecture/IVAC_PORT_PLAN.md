# IVAC / VAC Audio-Sources Port Plan (#158 → #102/#103)

Status: **IN PROGRESS.** Decisions signed off 2026-06-14 (incl. PureSignal
verified unaffected — `calcc.c`/`iqc.c` reference no audio device; PS
feedback is a rate-configured IQ channel `SetPSFeedbackRate` calcc.c:1065).
**Stage 0 DONE** (gate cleared — see §3 / R0). Stages 1-7 pending.
Reference studied: Thetis 2.10.3.13 `ChannelMaster/ivac.c` (907) + `ivac.h`
(133) + `Console/ivac.cs` (195) + `wdsp/rmatch.c` + `ChannelMaster/pipe.c`
+ `cmaster.c`, cross-referenced against the lyra-cpp ported `wire/` layer
and the existing Qt `QAudioSink` RX-out path.

This is the **host-audio foundation** (#158): the IVAC engine that bridges
PC audio devices to the radio's TX/RX chains. VAC1 (#102) and VAC2 (#103)
are two instances of it. HL2 Line-In (#104) is a separate codec path, not
covered here.

---

## 1. What IVAC does (two independent data flows)

**VAC OUTPUT — radio RX → PC** (so WSJT-X/MSHV/etc. *hear* the radio):
```
RX audio  ─┐
TX monitor ─┴► AAMix (2-in, synchronised)  ──► rmatchOUT ring (RX rate → VAC rate, drift-corrected)
                                                   └─► QAudioSink → PC input device
```
(raw-IQ mode: RX-IQ bypasses the mixer straight into rmatchOUT, + optional Q-swap.)

**VAC INPUT — PC → radio TX** (so the PC app *keys/feeds* the radio):
```
PC output device → QAudioSource ──► rmatchIN ring (VAC rate → mic rate, drift-corrected)
                                        └─► xvacIN → combine(L+R) + preamp → TX mic seam
```

The two directions are fully independent; each has its **own** rmatch ring
doing its own two-clock drift correction. That's why two separate Qt
devices (source + sink) is clean — we don't need one full-duplex stream.

---

## 2. Engine = the reference; device I/O + plumbing = Lyra-native

| ivac.c responsibility | Lyra-cpp approach |
|---|---|
| PortAudio device I/O (`StartAudioIVAC`, `CallbackIVAC`) | **Qt Multimedia.** VAC-out `QAudioSink` + QIODevice mirroring the existing `AudioRing` (`wdsp_engine.cpp:184-267`); VAC-in `QAudioSource` pushing into rmatchIN. Device enumeration/format/buffer-depth pattern already exists (`wdsp_engine.cpp:1303-1355`, `:2354-2403`). |
| Adaptive resampler `rmatchIN`/`rmatchOUT` | **WDSP DLL via `wdspcalls`** — `create_rmatchV` / `xrmatchIN` / `xrmatchOUT` / `destroy_rmatchV` / `forceRMatchVar` / `setRMatch*` / `getRMatchDiags`. NOT plain `RESAMPLE` (no drift loop). NOT a C re-port. **Gated on Stage 0 link-check.** |
| Async mixer (`create_aamix`/`xMixAudio`/`xvac_out`) | **Reuse ported `wire/AAMix`** (Stage B). VAC-out = a 2-input AAMix instance (RX audio + TX-monitor). |
| IN/OUT rings | These ARE the rmatchV-internal rings (sized from latency). No new ring class; do NOT reuse `CmBuffs` (different role). |
| VAC-in → TX seam (`xvacIN`) | **Mirror the TCI TX-in pattern** (`CMaster.cpp:413-418` drain + `:475-478` register), joining the SAME mic-source selector with mutual-exclusion vs TCI (`use_tci_audio`). |
| VAC-out tap (`xvacOUT`) | Hang off **`WdspEngine`'s RX audio/IQ path** (lyra has no `pipe.c`; RX dispatch is `WdspEngine::feedIq` — `CMaster.cpp:377-381`). TX-monitor tap off the TX path. |
| `SetIVAC*` setters/state | Port verbatim as a small `lyra::wire::Ivac` class, instance-parameterised by id (VAC1=0, VAC2=1), like `pvac[id]`. |

---

## 3. Staged build order (each stage builds clean + is revertible; bench-gate the audio-producing ones)

- **Stage 0 — rmatchV link-check + `wdspcalls` ABI expose. ✅ DONE.**
  `dumpbin /exports _native/wdsp.dll` confirmed all 20 needed symbols are
  exported (`create_rmatchV`/`destroy_rmatchV`/`xrmatchIN`/`xrmatchOUT`/
  `forceRMatchVar`/`getRMatchDiags`/`resetRMatchDiags` + 13 `setRMatch*`;
  `create_varsampV`/`xvarsampV` present underneath). Added the 20 cdefs +
  X-macro resolves to `wire/wdspcalls.{h,cpp}`; handle is opaque `void*`
  (no struct port). Build clean. **GATE CLEARED — no `rmatch.c`/
  `varsamp.c` re-port needed.**
- **Stage 1 — `wire/Ivac.{h,cpp}` engine, wire-INERT.** Port the `ivac`
  struct + `create_ivac`/`destroy_ivac` + the two rmatchV rings + the
  AAMix instance + all `SetIVAC*` setters. No Qt audio, no taps yet.
  scratch unit test (synthetic buffer → rmatch → out) like ILV/xcmaster.
- **Stage 2 — Qt device I/O.** `QAudioSource` (VAC-in) + `QAudioSink`+
  QIODevice (VAC-out) mirroring `AudioRing`; device enumeration; int16↔
  double + mono→stereo conversion. Standalone loopback (tone in → resample
  → out) before touching the radio.
- **Stage 3 — VAC-out LIVE (RX → PC).** Tap `WdspEngine` RX audio (+ TX
  monitor) → AAMix → rmatchOUT → QAudioSink. **Bench gate:** a digital app
  on the PC receives clean radio audio with no drift over minutes.
- **Stage 4 — VAC-in LIVE (PC → TX).** `QAudioSource` → rmatchIN → xvacIN
  → mic-source selector (mutual-exclusion vs TCI) → TX. **Bench gate:** PC
  app audio transmits cleanly; selecting VAC disables TCI-in and vice-versa.
- **Stage 5 — MOX / monitor routing.** `SetIVACmox`/`SetIVACmon` →
  AAMix mute/monitor; wire to the PTT FSM (mute RX into VAC on TX, route
  TX-monitor). + raw-IQ output mode (#IQ) + swapIQ.
- **Stage 6 — Settings UI (VAC1).** Settings → Audio: VAC1 enable, in/out
  device pickers, rate/latency, IQ mode, preamp; add **"PC Soundcard
  (VAC1)"** to the mic-source selector.
- **Stage 7 — VAC2 (#103).** Second `Ivac` instance + second device pair +
  `txvac` selector (which VAC drives TX). The engine is already
  id-parameterised, so this is mostly UI + wiring.

---

## 4. Locked decisions (pending sign-off)

1. **Qt Multimedia, not PortAudio.** Matches the existing RX `QAudioSink`
   path; no new native dependency; two-device clocks handled per-ring.
2. **rmatchV via the bundled DLL** (Stage 0 confirms) — not a C re-port,
   not plain RESAMPLE.
3. **Reuse the ported AAMix** for the VAC-out 2-input mixer.
4. **VAC-in joins the mic-source selector** (mutually exclusive with TCI),
   not a parallel TX feed.
5. **Foundation-first**: #158 engine + Qt I/O, then VAC1 (#102) end-to-end,
   then VAC2 (#103). HL2 Line-In (#104) separate.

---

## 5. Risks / open items

- **R0 (gate):** rmatchV not exposed in `wdspcalls` yet — Stage 0 must
  confirm the DLL exports before the Qt approach is viable as-planned.
- **Two-device clock model** vs Thetis's single full-duplex PA stream:
  `CallbackIVAC`'s `vac_size == frameCount` assumption won't hold; size is
  negotiated per Qt device. Each rmatch ring already absorbs the drift.
- **Format/rate:** Thetis forces `paFloat64`; Qt negotiates Int16
  (`wdsp_engine.cpp:1319`). Bridge converts int16↔double; rmatch in/out
  rates configured from the negotiated device rate, not a fixed 48k.
- **Mono→stereo** mic dup (`CallbackIVAC:212-248`) must be replicated.
- **PTT-override / `use_tci_audio` mutual-exclusion** with the existing TCI
  source must be respected at the selector (Task #33), not bypassed.
- **IQ-output mode** needs a separate RX-IQ tap off `feedIq` (not the audio
  tap).

---

## 6. Key reference anchors

- Thetis: `ChannelMaster/ivac.c` (`create_resamps:33`, `CallbackIVAC:196`,
  `xvacIN:129`, `xvacOUT:145`, `xvac_out:165`, `StartAudioIVAC:265`),
  `ivac.h:36-96`, `Console/ivac.cs:53-190`, `wdsp/rmatch.c` (rmatchV ABI),
  `pipe.c:181-235` (the xvacIN/xvacOUT call sites), `cmaster.c:479-575`
  (size/rate setters).
- lyra-cpp: `wdsp_engine.cpp` (AudioRing/QAudioSink 184-267, startAudio
  1303-1355, device enum 2354-2403), `wire/CMaster.cpp` (DEFERRED stubs
  537-538/552-556, TCI-TX seam 413-418/475-478, RX-dispatch note 377-381),
  `wire/AAMix.{h,cpp}`, `wire/wdspcalls.h` (RESAMPLE surface 85-90 — to be
  extended with rmatchV), `mic_source.h`, `tci/TciTxBridge.h`.
