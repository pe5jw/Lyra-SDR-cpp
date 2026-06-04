# MOX/PTT FSM + HL2 EP2 wire frame

**Sources read 2026-06-04:**
- `Console/console.cs:30058-30420` — `chkMOX_CheckedChanged2` (the FSM)
- `Console/console.cs:19140-19200` — `m_bATTonTX` setter
- `ChannelMaster/networkproto1.c:869-1100` — `WriteMainLoop_HL2`
  (case 0-10 captured; case 11+ continuation pending)

---

## §5.1-5.6 — The MOX FSM

`chkMOX_CheckedChanged2(object sender, EventArgs e)` is the single
funnel through which every MOX transition flows. Source: SW button,
HW PTT, CAT, TCI all set `chkMOX.Checked`, which fires this handler.

### Keydown sequence (RX → TX), in exact order

`console.cs:30058-30348`:

1. `console.cs:30060-30067` — early-abort if `_ganymede_pa_issue`
   (Ganymede PA fault); flip checkbox back off, return.

2. `console.cs:30071` — fire `MoxPreChangeHandlers` (subscriber
   callbacks BEFORE the state change).

3. `console.cs:30073` — **`NetworkIO.SendHighPriority(1)`**.
   Immediate / out-of-band C&C frame push so the radio sees the
   new MOX state ASAP, before the next round-robin cycle.

4. `console.cs:30074-30078` — if `_rx_only` set: bail.

5. `console.cs:30081-30098` — VAC bypass routing.

6. `console.cs:30109` — `if (tx) _mox = tx;` — internal `_mox`
   flag set EARLY (before validation/freq calc/etc). Used by
   downstream guards.

7. `console.cs:30112-30240` — compute the TX freq:
   - FM TX offset / split / VFO selection (lines 30135-30143)
   - XIT offset if `chkXIT.Checked` (line 30142-30143)
   - Out-of-band guards (lines 30148-30229; chkMOX.Checked=false
     and return on fail)
   - **CW pitch offset** applied to freq: `CWL: +cw_pitch * 1e-6;
     CWU: -cw_pitch * 1e-6` (lines 30231-30239)

8. `console.cs:30267` — `_pause_DisplayThread = true;` — pause
   panadapter so the keydown transition doesn't flash artifacts.

9. `console.cs:30274-30291` — **STOP RX DSP channels with BLOCKING
   flush.** Conditional on `full_duplex` AND on which RX(s) need to
   shut down (per `chkVFOATX` / `chkVFOBTX` / `mute_rx1_on_vfob_tx`
   / `mute_rx2_on_vfoa_tx`):

   ```cs
   WDSP.SetChannelState(WDSP.id(0, 1), 0, 0);   // sub-RX dmode=0
   WDSP.SetChannelState(WDSP.id(0, 0), 0, 1);   // RX1 dmode=1 = BLOCKING
   WDSP.SetChannelState(WDSP.id(2, 0), 0, 1);   // RX2 dmode=1 = BLOCKING
   ```

   `dmode=1` = blocking down-slew flush; the call doesn't return
   until WDSP has fully drained the channel. **This is the cure
   for the "bristle broom sweep" Lyra hit repeatedly** — RX
   filters must NOT keep grinding the keyed-period IQ.

10. `console.cs:30293-30327` — **ATT-on-TX policy**:

    ```cs
    if (m_bATTonTX) {
        int txAtt = getTXstepAttenuatorForBand(_tx_band);
        if ((!chkFWCATUBypass.Checked && _forceATTwhenPSAoff) ||
            CW mode)
            txAtt = 31;                          // force min RX gain
        SetupForm.ATTOnRX1 = getRX1stepAttenuatorForBand(rx1_band);
        SetupForm.ATTOnTX = txAtt;
        updateAttNudsCombos();
    } else {
        NetworkIO.SetTxAttenData(0);
        Display.TXAttenuatorOffset = 0;
    }
    ```

    See §5.5 below for the wire encoding.

11. `console.cs:30329` — `UpdateAAudioMixerStates()` — flip the
    AAmixer routing to TX-active.

12. `console.cs:30330` — `UpdateDDCs(rx2_enabled)` — flip DDC
    routing (the (MOX, PS_armed) state-product).

13. `console.cs:30332` — **`HdwMOXChanged(tx, freq)`** — flips
    the HARDWARE: writes the wire MOX bit + TX freq C&C registers
    (via `NetworkIO`). After this call, the radio sees TX on the
    wire.

14. `console.cs:30333-30335`:
    - `Display.MOX = tx`
    - `psform.Mox = tx`
    - `cmaster.Mox = tx` — loads the router bit in cmaster

15. **For non-CW modes** (`console.cs:30339-30346`):
    ```cs
    if (rf_delay > 0)
        Thread.Sleep(rf_delay);              // settle delay
    AudioMOXChanged(tx);                     // flip audio.cs MOX
    WDSP.SetChannelState(WDSP.id(1, 0), 1, 0);  // START TX channel
    ```
    TX channel starts with `dmode=0` (non-blocking, WDSP cos²
    up-ramps internally per the `tslewup=0.010` set at OpenChannel).

16. **For CW modes** (`console.cs:30348`):
    ```cs
    AudioMOXChanged(tx);
    ```
    CW handles its own keying via key events (different path).

### Keyup sequence (TX → RX), in exact order

`console.cs:30350-30407`:

1. `console.cs:30352-30353` — `if (space_mox_delay > 0)
   Thread.Sleep(space_mox_delay)`. **`space_mox_delay`** delay,
   default 0.

2. `console.cs:30355` — `_mox = tx` (= false).

3. `console.cs:30356` — `psform.Mox = tx`.

4. `console.cs:30357` — **`WDSP.SetChannelState(WDSP.id(1, 0),
   0, 1)`** — **STOP TX channel with BLOCKING flush**
   (dmode=1). This is the cos² down-ramp; the call doesn't
   return until WDSP has fully flushed the TX downslew. **The
   wire MOX bit is STILL ON during this flush** — the faded
   TX I/Q tail goes out on the wire.

5. **For CW modes** (`console.cs:30359-30364`):
   ```cs
   if (!cw_fw_keyer && key_up_delay > 0)
       Thread.Sleep(key_up_delay);
   ```

6. **For non-CW modes** (`console.cs:30365-30369`):
   ```cs
   if (mox_delay > 0)
       Thread.Sleep(mox_delay);    // default 10ms
   ```
   Comment: "default 10, allows in-flight samples to clear".
   This is the buffer-drain window between TX DSP stop and the
   wire MOX bit being cleared.

7. `console.cs:30370` — `UpdateDDCs(rx2_enabled)`.

8. `console.cs:30371` — `UpdateAAudioMixerStates()`.

9. `console.cs:30373` — `AudioMOXChanged(tx)` — flip audio.cs
   to RX.

10. `console.cs:30374` — **`HdwMOXChanged(tx, freq)`** — flips
    the hardware: **clears the wire MOX bit + restores RX freq**.

11. `console.cs:30375-30376`:
    - `Display.MOX = tx`
    - `cmaster.Mox = tx` — router bit cleared

12. `console.cs:30377-30378` — **`if (ptt_out_delay > 0)
    Thread.Sleep(ptt_out_delay)`** — default 20 ms; comment:
    "added wcp 2018-12-24, time for HW to switch". Window
    between MOX-bit-cleared and RX DSP restart so the T/R
    relay can physically switch.

13. `console.cs:30379-30383` — **START RX DSP channels**:
    ```cs
    WDSP.SetChannelState(WDSP.id(0, 0), 1, 0);    // RX1
    if (RX2Enabled)
        WDSP.SetChannelState(WDSP.id(2, 0), 1, 0); // RX2
    if (radio.GetDSPRX(0, 1).Active)
        WDSP.SetChannelState(WDSP.id(0, 1), 1, 0); // sub-RX
    ```

14. `console.cs:30385` — `Audio.RX1BlankDisplayTX =
    blank_rx1_on_vfob_tx`.

15. `console.cs:30387-30389` — **HL2-specific**:
    `if (HardwareSpecific.Model == HPSDRModel.HERMESLITE)
    AutoTuningHL2(ProtocolEvent.Idle);` — stop the auto-tune.

16. `console.cs:30391-30407` — **ATT-on-TX restore**:
    `updateAttNudsCombos()` restores RX1+RX2 step-attn from the
    saved values (saved during keydown step 10).

### Keyup-ordering invariants — the §15.25 truth source

The empirical findings in CLAUDE.md §15.25 ("keyup tail" /
"bristle broom") map exactly to this source:

| Invariant | Reference file:line | Why it matters |
|---|---|---|
| TX DSP stops with `dmode=1` blocking flush | console.cs:30357 | Faded TX I/Q tail completes BEFORE wire MOX clears — no key-click |
| Wire MOX bit clears AFTER TX DSP downslew | console.cs:30357 → 30374 | If MOX clears first, an in-flight buffer of TX I/Q from the faded tail leaks out as garbage |
| `mox_delay` (default 10 ms) between TX-stop and MOX-clear | console.cs:30367-30368 | In-flight samples in the C&C/EP2 queue drain |
| `ptt_out_delay` (default 20 ms) between MOX-clear and RX-DSP-start | console.cs:30377-30378 | T/R relay physically switches; RX front-end stabilizes |
| RX DSP restarts AFTER the relay settle | console.cs:30379-30383 | RX filters see clean RX IQ from the moment they restart — no transition artifacts to grind |

**Lyra implication:** the rip's MOX FSM must use the same SetChannelState
+ Thread.Sleep sequence semantics (Lyra-native equivalents:
`wdsp::SetChannelState(...)` direct cffi call + `std::this_thread::sleep_for(ms)`),
in this exact order. Earlier Lyra patches that tried to "fade audio"
or "reset AGC" instead of stopping the RX DSP channel were chasing
the symptom, not the cause.

### TR sequencing delays (the operator-tunable knobs)

| Knob | Default | Where applied | Purpose |
|---|---|---|---|
| `rf_delay` | varies (operator-set; reference exports `udRFDelay=50` for HL2 ops) | console.cs:30342-30343 (keydown, between hardware-MOX-set and TX-DSP-start) | T/R relay settle for external amp hot-switch protection |
| `mox_delay` | 10 (operator-set; export shows 15 for HL2) | console.cs:30367-30368 (keyup, between TX-DSP-stop and HdwMOXChanged) | In-flight samples drain |
| `space_mox_delay` | 0 (export: 13) | console.cs:30352-30353 (keyup, start of the sequence) | Pre-keyup pause |
| `ptt_out_delay` | 0 (export: 5; comment says "added wcp 2018-12-24") | console.cs:30377-30378 (keyup, between MOX-clear and RX-DSP-start) | HW T/R switch time |
| `key_up_delay` | (export: 10) | console.cs:30362-30363 (CW keyup only, if !cw_fw_keyer) | CW key-up settle |

### §5.5 — ATT-on-TX wire encoding

`console.cs:19148-19178` `m_bATTonTX` setter:

```cs
private bool m_bATTonTX = true;     // default ON
public bool ATTOnTX {
    get { return m_bATTonTX; }
    set {
        if (!value && _auto_attTX_when_not_in_ps) return;
        m_bATTonTX = value;
        updateAttNudsCombos();
        if (PowerOn) {
            if (m_bATTonTX) {
                int txatt = getTXstepAttenuatorForBand(_tx_band);
                if (HardwareSpecific.Model == HPSDRModel.HERMESLITE)
                    NetworkIO.SetTxAttenData(31 - txatt);
                else
                    NetworkIO.SetTxAttenData(txatt);
                Display.TXAttenuatorOffset = txatt;
            } else {
                NetworkIO.SetTxAttenData(0);
                Display.TXAttenuatorOffset = 0;
            }
        }
    }
}
```

**Confirms CLAUDE.md §15.26 ground truth:**

- **HL2 wire encoding: `wire = 31 - signed_dB`** (line 19165).
  Lyra commit `73a459b` already correct.
- Non-HL2 wire encoding: `wire = signed_dB` directly.
- ATT-on-TX OFF → `SetTxAttenData(0)` (line 19173).
- Default = ON (line 19148).
- `_auto_attTX_when_not_in_ps` lockout — when the PS auto-att
  state machine owns it, operator can't toggle.

### §5.4 — DDC routing on (MOX, PS_armed) state product

Driven by `UpdateDDCs(rx2_enabled)` (called at console.cs:30330
on keydown and 30370 on keyup). Need to read that function next
(separate file/section) to capture the routing matrix.

The reference's `cntrl1` field + the gateware routing of DDC0/
DDC1 to the PA coupler during MOX+PS+HL2 is documented in
CLAUDE.md §3.8 / §15.26; need source-verify via `UpdateDDCs`
and the gateware bits.

---

## §8 — HL2 EP2 wire frame (`WriteMainLoop_HL2`)

`networkproto1.c:869-1098+`. Captured cases 0-10; case 11+ in
follow-up read.

### Frame structure

Per UDP datagram: 2× 512-byte USB frames.

Per USB frame (networkproto1.c:881-890):
```c
txbptr[0] = 0x7f;    // sync byte 0
txbptr[1] = 0x7f;    // sync byte 1
txbptr[2] = 0x7f;    // sync byte 2
// then C0-C4 control bytes (5 bytes)
// then 504 bytes payload (63 LRIQ tuples × 8 bytes each)
```

### Hermes-II TX edge re-prime (HL2 no-op)

`networkproto1.c:885-891`:
```c
if (XmitBit != PreviousTXBit) {
    if (nddc == 2)
        out_control_idx = 2;     // jump to RX1 VFO C&C on MOX edge
    PreviousTXBit = XmitBit;
}
```

This re-primes DDC0 with TX freq on the MOX edge for Hermes-II
(nddc=2). **HL2 nddc=4 — no jump fires.** HL2 relies on the
persistent `tx[0].frequency` + the duplex bit. The §15.18
Phase 1 Lyra commit pre-emits all three TX-freq C&C registers
(0x02, 0x08, 0x0a) on `_set_tx_freq` to mirror this — correct.

### MOX bit lives in C0

`networkproto1.c:896`:
```c
C0 = (unsigned char)XmitBit;
```

**Every C&C frame** carries the MOX bit as C0 bit 0 — single
chokepoint, single test, can never desynchronize with the C&C
round-robin advance.

### I2C dispatch (preempts the C&C round-robin)

`networkproto1.c:898-943`: when the I2C queue has pending
transactions, the next frame's C0 is built as an I2C command
(0x3c or 0x3d address with control bit), C2-C4 carry the
address + data. The frame is "stolen" from the round-robin for
this purpose. Has a `delay` countdown so I2C transactions don't
back-to-back.

This is the mechanism the Apollo-tuner I2C work would have used
— but the actual PA enable lives in case 10 C2 bit 3, NOT the
I2C path. Per CLAUDE.md §15.26 verification, the "I2C side-
channel" hypothesis was REFUTED by the gateware RTL read.

### C&C round-robin cases (file:line ladder)

`networkproto1.c:946-1098+` — `switch (out_control_idx)`:

| Case | C0 bit pattern | Reg name | What it carries |
|---|---|---|---|
| 0 | `0x00` (XmitBit\|0) | General | SampleRate(C1) ; EER + OC pins(C2) ; ATT bits + preamp + dither + random + RX1-out + XVTR/RX1/RX2-in(C3) ; ANT + duplex(0x04) + nddc-1<<3 + diversity(C4) |
| 1 | `0x02` | TX VFO | `prn->tx[0].frequency` big-endian (C1-C4) |
| 2 | `0x04` | RX1 VFO (DDC0) | RX0 freq normally; **TX freq if (nddc==2 && XmitBit && puresignal_run)** — the Hermes-II PS routing |
| 3 | `0x06` | RX2 VFO (DDC1) | RX1 freq normally; TX freq if (nddc==2 && XmitBit && PS); RX0 if nddc==5 (ANAN) |
| 4 | `0x1C` | ADC + TX-ATT | C3 = `prn->adc[0].tx_step_attn & 0x1F` — **5-bit field, the encoding Lyra had wrong** (Lyra commit `73a459b` fixed) |
| 5 | `0x08` | RX3 VFO (DDC2) | nddc==5 → RX2 freq; else **TX freq** (HL2 PS feedback DDC) |
| 6 | `0x0a` | RX4 VFO (DDC3) | **TX freq always** (HL2 PS feedback DDC) |
| 7 | `0x0c` | RX5 VFO (DDC4) | TX freq |
| 8 | `0x0e` | RX6 VFO | RX0 freq (unused HL2) |
| 9 | `0x10` | RX7 VFO | RX0 freq (unused HL2) |
| 10 | `0x12` | HL2-specific | **C1 = drive_level** (top 4 bits = HL2 16-step DAC) ; **C2 = mic_boost\|line_in<<1\|ApolloFilt(0x04)\|ApolloTuner(0x08)\|ApolloATU(0x10)\|ApolloFiltSelect(0x20)\|0x40** ; C3 = HPF flags + `pa<<7` (legacy, HL2 ignores) ; C4 = LPF flags |
| 11 | `0x14` | Step-attn + PS | (see follow-up read) |
| 12+ | ... | (more cases) | ... |

**Critical for §8 — the PA enable mechanism on HL2+:**

`networkproto1.c:1079-1080`:
```c
C2 = ((prn->mic.mic_boost & 1) | ((prn->mic.line_in & 1) << 1) | ApolloFilt |
      ApolloTuner | ApolloATU | ApolloFiltSelect | 0b01000000) & 0x7f;
```

With `ApolloTuner = 0x08` and `ApolloFilt = 0x04`, when PA is
enabled:
- `ApolloTuner` is set → C2 bit 3 = 1 → **gateware sees
  pa_enable=1, PA bias activates** (per ak4951v4 RTL
  `control.v:209-220` `pa_enable<=data[19]`)
- `ApolloFilt` is set → C2 bit 2 = 0 → tr_disable=0 → T/R
  relay can fire

This is the CLAUDE.md §15.26 finding, now sourced byte-correctly
from the reference: PA enable = case-10 C2 bit 3. **Lyra commit
`cbba63a` already correct.**

Case 11 (0x14) carries step-attn + puresignal_run flag — pending
read.

### MOX gating layered (the wire-side belt + suspenders)

Two independent layers:
1. **C0 bit 0 = MOX** — every C&C frame carries the live MOX
   bit. Gateware reads it per frame.
2. **TX I/Q zero-fill on `!XmitBit`** — `sendProtocol1Samples`
   line 1227: `if (!XmitBit) memset(outIQbufp, 0, ...)`. Even
   if MOX-bit-in-C0 said "TX off" but a stray I/Q sample was
   in the buffer, this zeroes it before the wire.

Both must be honored by the Lyra wire-send thread.

---

## Open items remaining for §5 and §8

1. **`HdwMOXChanged(bool tx, double freq)`** at console.cs:29749
   — need to read what this function actually does (writes which
   `NetworkIO.*` registers in what order).
2. **`AudioMOXChanged(bool tx)`** at console.cs:29733 — what
   the audio.cs side gets told.
3. **`UpdateDDCs(bool rx2_enabled)`** — the DDC routing matrix
   for (MOX, PS_armed) state product (§5.4).
4. **`UpdateAAudioMixerStates()`** — the AAmixer state product.
5. **`WriteMainLoop_HL2` cases 11+** — step-attn (case 11) +
   reset-on-disconnect (case 18) + remaining cases.
6. **`ForceCandCFrame`** at networkproto1.c:134 — the priming
   sequence sent at startup (sends 3 C&C frames before normal
   operation).
7. **`sendProtocol1Samples` integration with `XmitBit`** —
   where `XmitBit` global is written (it's the bridge between
   the C# MOX FSM and the C++ wire-send thread).

Resolves in next reads.

*File written 2026-06-04 during Phase 0 read.*
