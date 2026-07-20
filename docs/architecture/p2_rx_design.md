# P2-2 — Brick / ANAN-G2 receive (HPSDR Protocol 2 RX)

**Status:** SCOPED, no code (2026-07-20). Design only. Implement
stage-by-stage against this doc, each stage gated on its own check.

**Goal:** take a `brick_p2` / `anan_p2` rig — already *discovered and
registered* by P2-1 — from "listed but un-openable" to **"open it and
hear it."** IQ from the radio's DDCs must land in the exact same DSP /
audio / panadapter path the HL2 feeds today. **RX only.** No transmit,
no PureSignal, no TX/DUC in this milestone.

Companion to the multi-rig work (`multi_rig_design.md`) and the P2
discovery already shipped (P2-1: commits `4257b42` + `f57aa88`).

---

## 1. Where this sits in the arc

| Stage | What | State |
|---|---|---|
| P2-1 | Dual-protocol **discovery** + RigRegistry identity | ✅ shipped |
| **P2-0** | `radioProtocol` **selector** driven to ETH on P2-rig open | folds into P2-2 stage 1 (see §3) |
| **P2-2** | **Brick RX** (this doc) | scoped |
| P2-3 | Command & control depth (per-band OC, filters, gain) | later |
| P2-4 | TX / DUC | later |

---

## 2. Confirmed architecture (source-read, not assumed)

- **The running app's RX path IS the `src/wire/` ChannelMaster port.**
  `hl2_stream.cpp` is the thin driver — it includes `wire/RadioNet.h`,
  `wire/MetisFrame.h`, `wire/FrameComposer.h`, `wire/Ep6RecvThread.h`,
  and pumps that engine. There is **no** separate "native HL2" RX path
  to fork; P2 extends the same engine.
- **The engine is already protocol-parameterized.** The global
  `radioProtocol` (`RadioNet.cpp:405`, `enum class RadioProtocol
  { USB=0, ETH=1 }`, default `USB`) drives per-protocol sizing that is
  **already computed for P2**:
  - `RadioNet.cpp` sets `rx.spp = 238`, `tx.spp = 240`, `audio.spp = 64`,
    `mic.spp = 64` when `radioProtocol == ETH` (vs 63/126/126/63 for P1).
- **Nothing ever assigns `RadioProtocol::ETH`.** Grep across `src/`
  finds only reads / branches, never a write. So the P2 branches are
  present-but-dormant reference text (house discipline: verbatim
  ChannelMaster port with `DEFERRED [Protocol 2 …]` tags), and the
  selector is scaffolding that nothing has switched on yet.

**Implication:** P2-2 is *un-defer the ETH branches + supply the P2-only
units, driven by the existing selector* — NOT a new engine and NOT a
`WireEngine` interface (locked in `multi_rig_design.md` / the P2 arc).

---

## 3. The one real divergence: P1 is stateless-ish, P2 is a held session

This is the crux of P2-2 and the piece with **no P1 analogue**.

- **P1 (Metis / HL2):** discover → send START → the radio streams EP6
  until STOP. Control rides the same data flow. One socket, simple.
- **P2 (openHPSDR):** a **held session** on one UDP socket:
  1. Send the **60-byte general packet** to `:1024` — this *claims the
     controller lease by our source IP* (the radio captures reply_addr).
  2. Send the **1444-byte high-priority packet** with `run=1`, and keep
     re-sending it on a timer (~100 ms) — this is the keepalive; the
     radio drops to idle ~1 s after the client stops (or dies).
  3. The radio starts streaming; the **first HP-status packet back**
     (radio source port `:1025`) is the "radio is active" proof.
  4. Send the **DDC-specific packet** to enable each DDC at a legal
     rate; IQ frames then arrive on radio source ports `:1035 + n`.
  5. On close: send `run=0` (more than once — it's UDP) to release the
     lease so another client (Thetis / deskHPSDR) can claim it.

**Stage 1 (P2-0 fold-in):** when opening a P2 rig, set
`radioProtocol = RadioProtocol::ETH` *before* the engine spins, and
restore `USB` on close. Small, but it flips every already-present ETH
branch live at once — so it lands first and everything after builds on it.

---

## 4. Reference stack (three-deep, distinct roles)

Study each, implement **Lyra-native** — no code copied, no reference name
in shipped code/comments/commits (provenance lives here + in memory only;
the WDSP ports are the sole attributed code).

| Reference | Path | Role |
|---|---|---|
| **deskHPSDR** (DL1BZ, GPL C) | `D:/sdrprojects/deskhpsdr` — `src/new_protocol.c`, `src/radio.c` | **PRIMARY Brick-behavior truth.** Readable C P2 RX; models the Brick as a first-class device (`HERMES_MODE_BRICK`, `radio.c:2862`) and carries **Brick-specific quirks the Saturn refs never surface** (see §6). |
| **Jerry / KD4YAL `P2Session`** | `Y:/Claude local/SDRProject/lyra-cpp-p2saturn` — `src/wire/P2Session.{h,cpp}` | **Bench-verified session/handshake** on a live ANAN-G2. Exact wire facts: ports, phase word, sequence-zero rule, watchdog bit, Alex-deaf trap. Hermes-class handshake = our Brick. |
| **ramdor / Thetis** | `D:/sdrprojects/ramdor-Thetis` — `ChannelMaster/network.c` | Canonical P2 client the whole ecosystem regresses against; the ported `src/wire/` structure already mirrors it. |

---

## 5. Stages (RX-only) + acceptance gates

Each stage is independently buildable and reversible. Do **not** advance
until the gate passes.

### Stage 1 — drive the selector
- On P2-rig open: `radioProtocol = ETH` before the wire engine starts;
  restore `USB` on close. Keep the P2-1 open/switch guards for now.
- **Gate:** opening an HL2 (P1) rig is byte-identical to today (P1
  regression null); the selector value is observable in a log line.

### Stage 2 — session bring-up (the P2 handshake)
- ETH-path socket bind (one socket, our source port) + general packet
  (claim lease) + HP `run=1` keepalive timer (~100 ms) + parse the
  incoming HP-status (`:1025`) → emit an "active" state = Lyra's
  equivalent of `P2Session::started`.
- **Gate:** on the link-local bench, opening the Brick produces a
  sustained HP-status stream (seq incrementing) and holds for minutes
  without dropping (proves the lease + keepalive + the Windows UDP
  flow-state resend, §6).

### Stage 3 — DDC enable + IQ receive
- Send DDC-specific packet (DDC0 at a legal rate: 48/96/192/384/768/1536,
  24-bit); receive `:1035+n` IQ frames in the `Ep6RecvThread` ETH branch;
  unpack **238 samples × 6 bytes** (I then Q, BE signed 24-bit, scaled
  `/2³¹`) → hand to the **same IQ sink HL2 feeds**.
- **DDS freq:** write the **phase word = round(Hz × 2³² / 122.88e6)**,
  NOT raw Hz (the radio hardcodes phase-word decode). Confirm the
  framework's P2 freq write already does this; if not, that's the fix.
- **Gate:** IQ counters advance with zero sequence errors; a synthetic /
  known signal appears at the right offset in the panadapter.

### Stage 4 — front-end routing (the deaf trap)
- Derive and send the **Alex RX filter word** from DDC0's dial frequency
  on every HP build (Jerry's `P2HardwareProfile::alexRxWord(hz)`
  pattern). All-zero front-end words = "connects but only hears its own
  noise floor" — bench-confirmed on Jerry's rig.
- **Gate:** real antenna signals (not just the noise floor) on a known
  band; band change re-filters automatically.

### Stage 5 — wire to the existing RX chain + un-guard
- Confirm IQ flows through WDSP / audio / panadapter unchanged (the DSP
  side is protocol-agnostic, so this should be ~free once IQ lands).
- Only now relax the P2-1 open/`switchRig` guards for RX so a P2 rig can
  be opened + made active. Set `maxReceivers` / audio-path defaults from
  discovery + `capabilitiesFor(BrickP2)`.
- **Gate (the P2-2 milestone):** open the Brick on the link-local NIC →
  real signals + audio on a known band, level/quality compared against a
  reference client (deskHPSDR / Thetis) on the same antenna.

---

## 6. Traps / risks (call out before code)

- **🔴 DDS phase word, not Hz.** `phase = Hz × 2³² / 122.88 MHz`. A raw-Hz
  write tunes wildly wrong. Verify the ETH freq path in `FrameComposer`.
- **🔴 Brick2 sample-rate gain quirk.** deskHPSDR `new_protocol.c:2580`:
  *"Brick2 P2: 48 kHz delivers higher IQ amplitude (~+29 dB vs ≥96 kHz)."*
  A Brick-specific level offset **Thetis and the Saturn `P2Session` do
  NOT model.** Miss it and Brick RX is ~29 dB hot at 48 k vs 96 k+. The
  Stage-3/5 gate must check **RX-level parity across rates**, not at one
  rate. Model it in the P2 capability/profile layer, not the DSP.
- **Alex-deaf front end.** All-zero RX/TX filter words = noise floor only.
  Stage 4 exists specifically for this.
- **Brick3 diversity DDC enable.** deskHPSDR `p2_diversity_brick3_mode_active`
  — the Brick's STM32 wants extra DDCs (DDC2/DDC3) enabled in some modes,
  unlike a stock ANAN. Out of scope for single-RX P2-2 but note it so a
  future diversity/RX2 pass doesn't rediscover it.
- **Windows UDP flow-state.** Jerry's `P2Session` keeps a periodic resend
  (~5 s) so Windows doesn't expire the flow state for the radio→PC `:1025`
  stream — bench-proven necessary (zero status packets arrive without it
  on a default-firewall host).
- **Sample layout / endianness.** P2 IQ = 238 × (3B I + 3B Q) BE signed
  24-bit — different from P1's EP6 slot layout. The unpack is new code.
- **Sequence rule.** Control packets (general, HP) send sequence **always
  zero**; only data streams increment. The radio must not dedupe control.

---

## 7. Bench safety (RX-only, non-negotiable)

- Transmit bit **never** set; drive level 0; PA enable 0 in the general
  packet. This is RX-only control traffic.
- Set the **hardware-watchdog bit** so the radio auto-drops to idle ~1 s
  after this client dies for any reason.
- The P2-1 TX-side guards stay in place; nothing in P2-2 keys RF.

---

## 8. Open questions (resolve on the bench, don't guess)

1. Does the ported `FrameComposer` ETH freq write already emit the phase
   word, or raw Hz? (Read first; fix if raw.)
2. Brick DDC count / legal rates as *this* unit reports them (discovery
   said 2 rx) vs what deskHPSDR enables for `HERMES_MODE_BRICK`.
3. The exact +29 dB@48k correction constant + which rates it applies to
   (deskHPSDR is the source; confirm against our unit's levels).
4. Which `src/wire/` ETH branches are complete vs stubbed `DEFERRED` —
   an inventory pass at Stage 2/3 start (Network.cpp, NetworkProto1.cpp,
   Ep6RecvThread.cpp, RadioNet.cpp).

---

## 9. Convergence note

P2-2 keeps the identity layer P2-1 established: one `RigRegistry`
(Rick's), not Jerry's parallel `HardwareCatalog`. The Brick opens as its
registered `brick_p2` rig; `capabilitiesFor(BrickP2)` supplies the family
defaults; the P2 quirks (rate gain, DDC map) live in the capability /
profile layer, never leaking into the protocol-agnostic DSP.
