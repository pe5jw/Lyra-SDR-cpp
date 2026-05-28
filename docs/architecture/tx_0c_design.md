# TX-0c — Design read (plan, not code)

**Goal:** wire the TX-0b register state into the EP2 C&C round-robin
under MOX gating. First commit that can put RF on the air.

Reference reads (this session, 2026-05-28): Thetis 2.10.3.13
`networkproto1.c WriteMainLoop_HL2` + `console.cs chkMOX_CheckedChanged2`
+ `netInterface.c` Apollo defines + the ak4951v4 gateware RTL
(`hl2_rtl_control.v`, `hl2_rtl_dsopenhpsdr1.v`). No old-Lyra anywhere.

---

## 1. Reference behavior — how Thetis paces C&C on HL2

### 1.1 Round-robin: 19 slots, 1 per USB frame, no MOX-edge jump

`WriteMainLoop_HL2` (networkproto1.c:869-1191) emits **one C&C frame per
USB frame**, indexed by a single global `out_control_idx`:

- `for (txframe = 0..1)` → each datagram carries 2 USB frames → 2 C&C
  slots advanced per datagram.
- At ~380 Hz datagram rate that's **~760 C&C slots/sec / 19 slots
  ≈ 40 Hz per slot** (every TX register refreshed every ~25 ms).
- After the switch: `if (out_control_idx < 18) out_control_idx++; else
  out_control_idx = 0;` (lines 1180-1183).
- MOX-edge cycle jump (lines 886-891) is **`nddc == 2` only**
  (Hermes-II). HL2 (nddc=4) never jumps — relies purely on the
  continuously-refreshed cached state in the gateware.

### 1.2 The 19 slots (HL2 ground truth)

| `out_control_idx` | wire C0 (addr<<1) | what (`prn->*`) |
|---|---|---|
| 0 | `0x00` | general (rate, OC, atten/dither/ant, duplex bit, nddc-1) |
| 1 | `0x02` | TX VFO = `tx[0].frequency` |
| 2 | `0x04` | RX1 VFO (DDC0) |
| 3 | `0x06` | RX2 VFO (DDC1) |
| 4 | `0x1c` | ADC assignments + **`tx_step_attn & 0x1F`** (C3) |
| 5 | `0x08` | DDC2 = `tx[0].frequency` (HL2 mirror) |
| 6 | `0x0a` | DDC3 = `tx[0].frequency` (always) |
| 7-9 | `0x0c`/`0x0e`/`0x10` | DDC4-6 (unused on HL2 — Thetis still emits with rx[0].frequency placeholders) |
| 10 | `0x12` | **drive_level (C1), PA-enable bit (C2 bit 3 via ApolloTuner), BPF/LPF** |
| 11 | `0x14` | preamps + **C4 MOX-gated step-att** (TX=tx_step_attn, RX=rx_step_attn) |
| 12 | `0x16` | adc[1]/[2] step-att + CW keyer |
| 13 | `0x1e` | CW (cw_enable, sidetone) |
| 14-17 | `0x20`/`0x22`/`0x24`/`0x2e` | CW hang, EER PWM, BPF2, TX latency+PTT hang |
| 18 | `0x74` | reset_on_disconnect |

**MOX bit:** line 896, `C0 = (unsigned char)XmitBit;` — set on EVERY
emitted C&C frame regardless of slot. Then each case OR's the address
bits into C0 bits 7-1. So **MOX is a per-frame attribute, not
per-slot**.

### 1.3 Key insight for TX-0c

Thetis emits TX-bearing frames (slots 1, 4, 5, 6, 10, 11, …)
**continuously, at MOX=0 too**. The gateware sits on the cached state
until the MOX bit flips, then the already-cached TX freq/drive/PA/atten
is used. This is the safe, reference-faithful posture: never the case
that "MOX goes hot but the gateware has stale 0s." Don't try to be
clever and only emit TX frames when keyed — that's where the bugs
hide.

---

## 2. MOX-edge sequencing (Thetis keydown/keyup)

`chkMOX_CheckedChanged2` (console.cs:30058-30416). Full sequence for
the HL2 SSB path:

### 2.1 Keydown (RX → TX)

1. (TX power computed if not in TUN/2TONE.)
2. **Shutdown RX WDSP channel(s) — `SetChannelState(rx, 0, 1)`
   blocking flush.** Per-VFO policy (mute_rx1_on_vfob_tx etc.).
3. **`m_bATTonTX` policy** (line 30293-30327, HL2 branch):
   - `txAtt = getTXstepAttenuatorForBand(_tx_band)` (per-band table).
   - **Force `txAtt = 31`** when `(!chkFWCATUBypass && _forceATTwhenPSAoff)`
     OR `CW mode`.
   - `SetupForm.ATTOnTX = txAtt` → flows to the wire via
     `SetTxAttenData(31 - txatt)` HL2-only — i.e. stored in
     `adc[0].tx_step_attn` which the C&C round-robin then emits on
     slots 4 + 11.
4. `UpdateAAudioMixerStates` + `UpdateDDCs(rx2_enabled)`.
5. **`HdwMOXChanged(tx, freq)`** — flips the hardware MOX (the wire
   bit; XmitBit goes 1).
6. `Display.MOX = tx; psform.Mox = tx; cmaster.Mox = tx`.
7. If NOT CW: `Thread.Sleep(rf_delay)`, then `AudioMOXChanged(tx)`,
   then `SetChannelState(TX, 1, 0)` — turn TX DSP ON.
8. CW: just `AudioMOXChanged(tx)`.

### 2.2 Keyup (TX → RX)

1. `Thread.Sleep(space_mox_delay)` (default 0).
2. `_mox = false` (internal flag).
3. **`SetChannelState(TX, 0, 1)`** — turn TX DSP OFF (blocking flush
   of WDSP downslew).
4. CW: `key_up_delay` sleep; non-CW: **`mox_delay`** sleep
   (default 10 ms, "allows in-flight samples to clear").
5. `UpdateDDCs` / `UpdateAAudioMixerStates`.
6. `AudioMOXChanged(false)`, **`HdwMOXChanged(false)`** — clears
   the wire MOX bit.
7. `cmaster.Mox = false`.
8. **`Thread.Sleep(ptt_out_delay)`** (default 20 ms, "time for HW to
   switch", wcp 2018-12-24).
9. **`SetChannelState(RX, 1, 0)`** — RX channels back ON.
10. HL2: `AutoTuningHL2(Idle)`.
11. **`m_bATTonTX` restore** (line 30391-30410): `updateAttNudsCombos()`
    + `UpdatePreamps()` — restores per-band RX step-att.

### 2.3 TR-delay defaults (N8SDR Thetis DB, ground truth)

From `Thetis_database_export_…xml`:

| knob | Thetis DB | Lyra-cpp target |
|---|---|---|
| `mox_delay` | 15 ms | adopt 15 (operator's working value) |
| `rf_delay` | 50 ms | adopt 50 (1 kW linear hot-switch safety) |
| `space_mox_delay` | 13 ms | adopt 13 |
| `ptt_out_delay` (`udGenPTTOutDelay`) | 5 ms | adopt 5 |
| `udPTTHang` | 10 ms | (CW only, defer) |

All operator-adjustable in Settings → TX later; the defaults match
N8SDR's working rig.

---

## 3. ATT-on-TX policy (per N8SDR's Thetis DB)

Operator's DB values:

- `chkATTOnTX = True`
- `udATTOnTX = 31` (positive 31)
- `chkForceATTwhenPSAoff = True`

So on this rig, the keydown atten value is **always 31** (PS is off
at the protocol level, no per-band variation enabled). TX-0c can
**hardcode `tx_step_attn → 31` on MOX=1**, restore `tx_step_attn → 0`
on MOX=0. Per-band table + Settings UI is a later refinement.

Wire path is already correct (`706babb` TX-0b):
- `setTxStepAttnDb(db)` stores 0..31 operator dB.
- Future emission composer encodes:
  - **Slot 4 (`0x1C`) C3**: `(31 − tx_step_attn_db) & 0x1F`.
  - **Slot 11 (`0x14`) C4** (MOX-gated): if MOX=1 emit
    `(31 − tx_step_attn_db) & 0x3F | 0x40`; if MOX=0 emit
    `0x40 | ((lna_gain_db + 12) & 0x3F)` (current RX branch — already
    shipped in `hl2_stream.cpp:927-931`).

So at MOX=0, frame-11 C4 is unchanged (still RX gain-sense). At MOX=1
it becomes the TX-protective step-att. Operator-perceptible: panadapter
ADC overload chip on TX should drop instead of pegging.

---

## 4. lyra-cpp implementation design

### 4.1 C&C emitter rewrite — extend the 2-slot cycle to 19

Current `sendDatagram` (`hl2_stream.cpp:910-960`) hardcodes:
- USB frame 1 [offset 8]: `frame-0` general config (every datagram).
- USB frame 2 [offset 520]: alternating RX1-freq / LNA (19:1).

Replace with **Thetis-style round-robin**: one `cc_idx_` (0..18)
advances per USB frame; each USB frame writes the C&C bytes for that
slot. Frame-0 (general config / OC / rate / duplex) becomes one of
the 19 slots emitted at ~40 Hz — same as Thetis.

**Concerned about duplex-bit / rate cadence?** Don't be. Thetis ships
this on HL2 with frame-0 emitted ~40 Hz and the gateware caches it.
CLAUDE.md §3.2's "duplex bit set on every main-loop frame-0 emission"
means "every time you emit frame-0, set the bit" — not "you must emit
frame-0 in every datagram." Confirmed by Thetis's working behavior.

**Slot composers (one C++ function each)** — keep them local, atomic
reads only. For example:

```cpp
// slot 1: TX VFO
c0  = 0x02;
freq = txFreqHz_.load();
c1 = (freq >> 24) & 0xFF; c2 = ...; c3 = ...; c4 = (freq) & 0xFF;
```

…and so on for slots 4, 5, 6, 10, 11. Slots 7-9 (DDC4-6 unused on
HL2) and slots 13-17 (CW/EER/BPF2/TX-latency) and 18 (reset-on-disc)
can either be implemented as no-op placeholders (`c0=addr`, C1-C4=0)
or properly composed from operator state. For TX-0c minimum:

- **Implement composers for**: 0 (general), 1 (TX VFO), 2 (RX1 freq),
  4 (TX step-att slot 0x1C), 5+6 (DDC2/3 = TX freq mirror), 10 (drive
  + PA), 11 (LNA + MOX-gated step-att).
- **Placeholders (emit but inert)**: 3 (RX2 freq — same as today's
  default 0), 7-9 (DDC4-6 unused, send rx1 freq or zero), 12, 13-17,
  18.

Total: 19 slots cycling at ~40 Hz/slot.

**MOX bit emission**: in `sendDatagram` the per-USB-frame C0 base is
`(unsigned char)mox_.load()`. Each composer OR's its addr bits into
C0. This matches Thetis line 896.

### 4.2 PTT/MOX state machine (this is also new in lyra-cpp)

lyra-cpp has the `setMox` atomic but **no FSM** today. TX-0c needs a
minimal MOX-edge sequencer (separate file, `lyra::tx::Ptt` or wired
into HL2Stream). For TX-0c-SSB the sequence is:

**Keydown:**
1. Set `mox_ = true` (atomic). This makes the next ~2.6 ms's C&C
   frames carry C0 bit 0 = 1. Slot 11 C4 starts emitting
   `tx_step_attn`-based protection.
2. (No RX-DSP stop yet — lyra-cpp's RX DSP path is separate, will
   wire later. Defer.)
3. Apply ATT-on-TX: `setTxStepAttnDb(31)`. The new value lands on
   slots 4 + 11 within ~25 ms.
4. Sleep `rf_delay` (50 ms operator default).
5. (No TX DSP start yet — TX modulation chain is TX-2.)

**Keyup:**
1. (No TX DSP stop — N/A for TX-0c.)
2. Sleep `mox_delay` (15 ms).
3. Set `mox_ = false`.
4. Sleep `ptt_out_delay` (5 ms).
5. (No RX-DSP restart — N/A.)
6. Restore ATT-on-TX: `setTxStepAttnDb(0)`.

**Critically:** TX-0c keydown produces NO RF on its own — `paOn_`
defaults False (PA disabled) AND there's no TX DSP yet (TX I/Q stays
0 in the EP2 datagram). What it does prove:
- MOX bit goes 1 on the wire.
- RX-protective step-att (31 → wire 0) gets applied on slot 11.
- Gateware accepts the keydown without errors.
- Telemetry continues to flow.
- Keyup clears MOX cleanly.

### 4.3 PA-enable wiring

The `_pa_on` atomic flows into slot 10 (`0x12`) C2 bit 3 (`0x08`).
Default-OFF (TX-0b ships it as `false`).

**TX-0c does NOT add a Settings UI for PA-enable** — leave it
default-False. Once TX-0c proves keydown is clean, a later commit
adds the operator toggle. Until then `setPaEnabled(true)` can be
called from a unit test / debug console for verification, but the
operator-facing path is OFF.

---

## 5. Wire-byte changes vs today's lyra-cpp (RX=0)

Today (lyra-cpp HEAD `2d8c68d`): each datagram emits frame-0 in slot 1
+ alternating RX1-freq/LNA in slot 2. Two C&C addresses on the wire
per datagram.

After TX-0c: each datagram cycles through 19 C&C addresses (1 per USB
frame); the gateware sees frame-0 once per ~25 ms instead of every
2.6 ms, but ALSO sees TX-VFO, drive, step-att, etc. on the same
cadence. **RX behavior at MOX=0 is unchanged** (the gateware caches
state; nothing TX-relevant happens without MOX=1 + PA-on + nonzero
EP2 TX I/Q). The wire bytes ARE different — by design — but the
radio's audible/visible behavior at MOX=0 is unchanged.

**This is the right reinterpretation of TX-0b's "byte-identical to RX"
constraint**: it meant "TX-0b doesn't touch RX behavior." TX-0c is
where TX bytes start appearing on the wire — that's intrinsic to the
task. The check is "RX still works" (not "the wire bits haven't
moved").

---

## 6. Bench gate (Phase-3-EXIT for lyra-cpp)

**Mandatory before any real-antenna keying.** Per §15.20/§15.24-C
posture.

1. **Static-only smoke** (no key):
   - Build, launch, confirm RX still works on antenna (no audio
     regression, no telemetry regression, no "Stream errors").
   - Confirm telemetry banner: `T ≈ 27 °C`, `V ≈ 12.25 V`,
     `PA ≈ 0 A`. None change from today.
   - **STOP here for operator A/B vs current build.** This validates
     the 19-slot cycle doesn't break RX.
2. **Dummy-load keydown** (PA default OFF):
   - Connect dummy load. PA toggle stays OFF.
   - Click MOX (or trigger via a debug action). Expect:
     - MOX bit goes 1 on wire (debug log).
     - `PA = 0` (PA disabled, no bias).
     - ADC overload chip does NOT peg (ATT-on-TX = 31 dB protection
       working).
     - Keyup → MOX bit goes 0 cleanly.
     - No relay chatter, no audio glitches on RX return.
3. **Dummy-load PA-enable keydown** (the real first RF):
   - Enable PA via debug toggle. Key MOX. Expect:
     - `PA ≈ 0.2 A` idle bias (gateware cuts bias only at full-
       drive; at zero drive the PA is biased but not actively
       transmitting).
     - Temp banner stable.
     - Keyup → `PA → 0`. Clean.
4. **Kill-test** (§15.20 watchdog verify, TX-UNVERIFIED for lyra-cpp):
   - During step-3 keydown, `taskkill /F /PID <lyra>`.
   - Operator watches PA current — must drop to 0 within a few
     seconds (gateware EP2-keepalive watchdog clearing PA bias when
     EP2 stops).
   - This is the SAFETY gate; must pass before any real-antenna keying.

NOTE: TX-0c does NOT produce a modulated carrier (no TX DSP yet). Even
with PA on + MOX on, EP2 TX I/Q is zero, so the radio is keyed but
silent. Idle PA current only. Step-4's purpose is to verify the
gateware safety, not to make on-air RF.

---

## 7. Out of scope for TX-0c (later phases)

- **TX modulation chain** — mic → SSB modulator → EP2 TX I/Q bytes.
  This is TX-2.
- **RX-DSP stop/restart** on MOX edges (the §15.26 bristle-broom
  saga). lyra-cpp's RX path is on the WDSP engine but the start/stop
  hooks aren't wired into the FSM yet. Add when TX modulation lands.
- **TUN tune-carrier**. WDSP TXA gen1 — needs TX channel open.
- **Per-band ATT-on-TX**, **operator Settings UI for PA enable** /
  TR delays / safety timeout / HW-PTT-in opt-in. Settings → TX tab
  bundled later.
- **Apollo-I²C side-channel** — confirmed unnecessary (operator's
  HL2+ keys PA purely from C&C 0x09 / frame-10 C2 bit3, per
  `control.v:213`). No I²C surface needed.

---

## 8. Commit slicing for TX-0c

Three commits, each independently revertable:

1. **TX-0c-emit**: rewrite `sendDatagram` to round-robin all 19 C&C
   slots. RX-only build (no FSM yet) — verify RX unchanged (bench
   gate §6.1). PA stays default-OFF, mox_ stays False, so wire is
   "Thetis-style RX cadence" but TX state still zero.
2. **TX-0c-fsm**: add the minimal MOX-edge sequencer. setMox(true) ↔
   ATT-on-TX 31 → mox_ on the wire → rf_delay → ready. Operator-
   triggerable from a debug menu item or QML button (no Settings
   yet). Bench gate §6.2.
3. **TX-0c-pa-debug**: add a debug-only "Enable PA" route (NOT in
   operator UI yet). Verify dummy-load PA-bias + kill-test. Bench
   gate §6.3+§6.4.

After all three pass: operator-facing UI (Settings → TX) + per-band
ATT-on-TX + safety timeout etc. land as TX-0d polish.

---

*Authoritative sources: this file's `tx_research.md` sibling §2.1 /
§4 + Thetis 2.10.3.13 (`WriteMainLoop_HL2`, `chkMOX_CheckedChanged2`,
`SetTxAttenData`) + ak4951v4 gateware RTL (`control.v`,
`dsopenhpsdr1.v`).*
