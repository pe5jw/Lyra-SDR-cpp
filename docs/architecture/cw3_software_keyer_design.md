# CW-3 — Host software keyer (CWX) design

**Status:** grounded design, awaiting operator sign-off. NO code yet.
**Scope:** #105 CW-3. The host-driven morse keyer — the core that
keyboard-CW, TCI CW (CW-4), and any host-text CW share. **NOT** the
physical paddle (CW-2, shipped, gateware-autonomous) and **NOT** the
external Winkeyer (#171, separate COM-port integration).

Provenance: this design is modeled on the Thetis 2.10.3.13 CWX engine
(`Console/cwx.cs`, `cwkeyer.cs`) and the HL2+ ak4951v4 gateware RTL
(`hl2_rtl_radio.v`), with the SDRLogger+ TCI CW client as the CW-4
protocol reference. Shipped code/comments/commits use RF/gateware
first-principles terms only — provenance lives here, per the project's
no-attribution rule. WDSP is not involved in CW.

---

## 1. What is ALREADY in place (CW-1/CW-2)

The entire **wire surface is done and live**; CW-3 adds only the host
keyer that drives it.

- **Wire bits exist + are packed live.** `RadioNet.h:345-348` defines
  `tx[0].cwx / cwx_ptt / dot / dash`. The EP2 overlay
  (`NetworkProto1.cpp:99-108`) packs `(cwx_ptt<<3 | dot<<2 | dash<<1 |
  cwx) & 0x0F` into the TX I-sample LSBs **when `cw.cw_enable` is set**
  (HL2: 4 bits; non-HL2: 3 bits). Already shipped + bench-proven by the
  paddle path.
- **`cw_enable` arming.** `applyCwKeyerEnable()` sets `cw.cw_enable` in
  CW mode (CWL=3/CWU=4) when `cwFwKeyer_`. The same master gate the
  paddle uses; CWX needs it set too (already is, in CW mode).
- **Gateware keys the carrier from host CWX.** `radio.v:1024/1055/1078`
  — the TX state machine enters `CWTX` on `cwx_keydown` exactly as on
  the paddle's `ext_keydown`; `radio.v:974-975` decodes
  `cwx_keydown = tx_tuser[2]`, `cwx_keyup = tx_tuser[1]`. So the host
  drives the key state via the LSB bits → the gateware makes the
  carrier envelope + HW sidetone. **No WDSP, no host carrier gen.**
- **Sidetone is gateware-generated**, level/freq from `cw.sidetone_*`,
  `sidetone_freq = cwPitchHz_` (unified pitch). Already wired
  (`applyCwConfigToPrn`). CWX sidetone is free.
- **FSM forward-compat.** `HL2Stream::PttSource` enum
  (`hl2_stream.h:835`) is `{Manual, HwPtt, Tci}` with an explicit
  "Cw / Vox land … follow the same pattern" note. `requestMox(on,
  source)`, `setMox`, `injectTxIq`, and the keydown/keyup FSM
  (`fsmKeydownPostMox`/`fsmKeyupPostSpace`, mox/space delays) exist.
- **Pitch is unified** (CW-2): one `WdspEngine::cwPitchHz`, RX beat +
  marker + TX carrier offset + sidetone.

## 2. Reference model (extracted, file:line in the session record)

**Thetis CWX** (`cwx.cs`): ASCII→element table (24-bit dot/dash
pattern + 5-bit element count); PARIS timing `tel = 1200/WPM` ms,
dash = 3·dot, inter-element 1·dot, inter-char 3·dot, inter-word
7·dot; **no weighting in CWX**. A 32 KB element ring FIFO
(push/pop, mutex). A Win32 multimedia timer at `tel` ms drives a
`process_element()` state machine that consumes the FIFO. PTT-lead:
on keydown it asserts PTT, waits `pttdelay`, then keys; on done it
drops key, holds PTT `ttdel` (~300 ms), then drops PTT. Abort
(`quitshut`): clear both FIFOs, drop key + PTT immediately. Sidetone
is gateware-generated (host sets level/freq only).

**SDRLogger+ TCI client** (the CW-4 target): sends
`cw_macros_speed:<wpm>` (before text), `cw_macros:0,<TEXT>` (send),
`cw_macros_stop` (abort). It is purely a *client* — the radio does
the morse encoding. So CW-4 = wire those three commands to the CW-3
engine. (SDRLogger+'s Winkeyer-USB + audio CW decoder are #171 / a
future RX-decode item — out of CW-3 scope, captured in memory.)

## 3. Lyra-native CW-3 design

### 3.1 Morse engine (`src/tx/CwMorse.{h,cpp}`, new)
- Static `char → element-pattern` table (ASCII, A-Z 0-9 + prosigns/
  punctuation). Lyra-native table, first-principles morse.
- Timing from WPM: `ditMs = 1200 / wpm`; dash = 3·dit; gaps
  1/3/7·dit. **Apply `cwKeyerWeight`** (Lyra already has the operator
  config; Thetis CWX ignores weight — this is a deliberate Lyra
  improvement, decision D). Optional Farnsworth later.
- Pure function: `text + wpm + weight → vector<Element{down,ms}>`.
  Unit-testable with zero hardware.

### 3.2 Keyer engine (`src/tx/CwKeyer.{h,cpp}`, new)
- Element FIFO (a simple deque; the Thetis 32 KB ring is overkill —
  bound it generously) + a **monotonic element-clock pump**
  (decision B: a dedicated keyer timer/thread, off the GUI thread,
  scheduling on absolute deadlines so element timing doesn't drift
  with event-loop jitter — the lesson from the wire-path work).
- State machine mirroring `process_element`: `Idle → PttLead →
  Keying(down/up per element) → Tail → Idle`. At each element edge it
  sets the wire key bits via a stream call (3.3).
- `sendText(QString)` enqueues; `abort()` = clear FIFO + force
  key-up + drop TX (the `quitshut` analog). A paddle press
  (`ext_keydown` via the existing HW path) during CWX aborts CWX
  (paddle wins — safety/operator-intent).

### 3.3 The `Cw` PttSource branch (in `HL2Stream`)
- Add `PttSource::Cw`. On `sendText` start: a Cw-branch keydown that
  **reflects TX state but SKIPS the WDSP `startFn` + `setInjectTxIq`**
  (no SSB modulator — CW carrier is gateware-made from the key bits).
- **Wire bits — VERIFIED against the gateware (was a guess, now
  resolved):** the host keyer drives **`tx[0].cwx` = 1 per Morse mark
  (dit/dah), 0 per gap**, and **`tx[0].cwx_ptt` = 1 held for the whole
  message** (set at start, cleared at end). `dot`/`dash` (bits 2/1)
  are the PHYSICAL paddle and stay 0 for CWX. Trace:
  `dsiq_tdata = 4×{dsethiq_tuser, eth_data}`; the CW nibble lands in
  the I-low byte; `cwx_saved = {eth_data[3]=cwx_ptt, eth_data[0]=cwx}`
  (`dsopenhpsdr1.v:360`); FIFO→`tx_tuser` gives
  `cwx_keydown = cwx_saved[0] = cwx (bit 0)` and
  `cwx_keyup = cwx_saved[1] = cwx_ptt (bit 3)`
  (`hermeslite_core.v:887`, `radio.v:974-975`). The gateware keys the
  carrier on `cwx`, holds TX between elements via `cwx_ptt` + a
  500-unit spacing hang (`dsopenhpsdr1.v:422`). My earlier "dot/dash
  positions" reading was WRONG — corrected here.
- **No composer/enable work needed.** The gateware CWX-enable
  (`cwx_enable <= cmd_data[24]`, `dsopenhpsdr1.v:394`) is the `0x0f`
  C1 bit 0, which the composer already sets from `prn->cw.cw_enable`
  (`FrameComposer.cpp:359`). `cw_enable` is armed in CW mode
  (`applyCwKeyerEnable`) → it enables BOTH the host EP2 overlay AND
  the gateware CWX decode. The CWX wire path is fully live; CW-3a is
  host-side only.
- Add `HL2Stream` methods `setCwxKey(bool down)` (→ `tx[0].cwx`) and
  `setCwxPtt(bool on)` (→ `tx[0].cwx_ptt`) under the prn lock, called
  by the keyer thread.
- **Break-in mirrors CW-2** (the operator-validated paddle behavior):
  QSK → host stays RX (no wire MOX), `cwx_ptt` keys the gateware,
  panadapter stays on the RX waterfall; Semi/Manual → assert host MOX
  too. cw_enable already set in CW mode; sidetone gateware-made.
- On done/abort: drop key → drop `cwx_ptt`/MOX after the tail.

### 3.4 Operator surface — the CW console panel (LOCKED)
- **Chip → floating CW console**, same idiom as the audio rack: a
  header/toolbar chip pops open a panel; user-sized; dockable-capable
  but **floats by default**; position/size persisted. Niche-but-
  valuable surface — only operators who keyboard-send and/or decode
  open it; the main window stays uncluttered for everyone else.
- **The panel is the container, built once to hold both halves:**
  - **Send half (CW-3b):** text send field (Enter sends, Esc aborts)
    + WPM (`cwKeyerSpeedWpm` exists). F1-F9 message memories + macros
    (`%`-substitutions) are a CW-3b follow-on within the same panel.
  - **Decoder half (CW-5):** decoded-text window + tune/AFC-lock
    indicator (see §6). Drops into the same panel — no rework.
- The `CWFWKeyer` toggle (paddle iambic-keyer arm) stays for the
  paddle; CWX is additive. Surface it in the panel/settings when
  convenient (not a first-commit blocker).

## 4. Decisions (RESOLVED 2026-06-18)
- **A — host-bit→gateware mapping: DONE / VERIFIED (2026-06-18).**
  Host drives `tx[0].cwx` (per mark) + `tx[0].cwx_ptt` (held per
  message); `dot`/`dash` stay 0 (paddle only). `cw_enable` (CW-mode
  armed) enables both the host overlay and the gateware CWX decode —
  no composer/enable work. Full trace in §3.3. (The earlier "dot/dash"
  guess was wrong; tracing corrected it.)
- **B — element clock:** **dedicated monotonic timer thread**, off the
  GUI thread (lowest jitter — the wire-path lesson).
- **C — UI:** **chip → floating CW console panel** (audio-rack idiom),
  built once as the container for send (CW-3b) + decoder (CW-5). First
  UI commit = send controls; decoder pane added with CW-5.
- **D — weighting:** **apply** `cwKeyerWeight` in the host morse engine
  (deliberate Lyra improvement; Thetis CWX ignores weight).

## 6. CW-5 — RX CW decoder (separate RX track, operator-greenlit)
A faithful C++23 port of the SDRLogger+ CW decoder (operator's own
code — no licensing concern; a proven algorithm, a port not R&D).
**Few SDR transceiver apps ship a built-in CW decoder — a genuine
differentiator.** RX-only, fully isolated from the TX/wire/safety path:
taps the RX audio stream Lyra already has, mode-gated to CW, emits text
to the CW console panel's decoder pane. Algorithm (must port
faithfully — these are what make it good, not optional): IQ
downconversion + AFC tone tracking, asymmetric envelope follower,
matched filter, SNR squelch gate, edge detection + noise-blip merging,
**Bayesian timing classifier with online fist-learning + Farnsworth
support**, morse-table lookup. Lands after CW-3/CW-4 (or in parallel —
no shared code with the TX keyer).

## 5. Build order
- **CW-3a** — verify §4-A, then morse engine + keyer FIFO/pump + `Cw`
  FSM branch + `setCwx*` stream methods. Bench gate (HL2, dummy load):
  type text in CW mode → clean gateware keying + HW sidetone + RF;
  QSK stays on RX waterfall; paddle still works; RX and SSB/AM TX
  unchanged; abort drops cleanly.
- **CW-3b** — send-field UI + F-key memories/macros + (maybe) the
  CWFWKeyer toggle.
- **CW-4** — wire TCI `cw_macros_speed` / `cw_macros` / `cw_macros_stop`
  (currently swallowed at `tci_server.cpp:1456`) to the CW-3 engine.
- **#171** (separate) — external Winkeyer USB (COM port + the
  SDRLogger+ Winkeyer protocol).
