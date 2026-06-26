# COM-port features — Serial PTT input + Kenwood CAT serial server

**Status:** DESIGN + BUILD (started 2026-06-26). Operator green-lit.
Covers two independent COM-port features (operator-requested):

1. **Serial PTT input** — a 3rd-party digital app (WSJT-X, VarAC,
   fldigi, …) asserts an RTS/DTR line on a (virtual) COM port; Lyra
   detects it and goes TX (which then keys the HL2/ANAN/Brick rig
   downstream, exactly as MOX does today). This is PTT *into* Lyra.
2. **Kenwood CAT serial server** — Lyra acts as a Kenwood rig on a
   COM port so loggers/digital apps can read/set VFO freq + mode and
   key TX over serial CAT. TS-480 / TS-2000 selectable.

**Provenance (per `docs/THETIS_DIRECT_PORT_PLAN.md`, operator-locked
2026-06-09):** the Thetis CAT/serial code lives in `Console\CAT\*.cs`
= the **study-only / write-Lyra-native** bucket (C# UI/control glue,
not a portable C module). So this is native C++23/Qt6, modeled on the
Thetis command coverage + quirks, with **open attribution** (Thetis
may be cited in comments/commits/docs). The Kenwood CAT grammar is an
open published standard; implementing it is not copying anyone.

**Framework:** Qt6 `QSerialPort` + `QSerialPortInfo` (built-in add-on,
no third-party lib). Cross-platform — this is one of the genuinely
portable pieces and adds **zero new Mac/Linux build debt** (it does
not by itself make the app cross-platform; the DSP DLLs + win32 wire
code are the remaining blockers — see [[project_lyra_cpp_platform]]).

---

## 0. Build prerequisite (step 1)

`CMakeLists.txt` does NOT yet link Qt6 SerialPort. Add it:
- `find_package(Qt6 6.11 REQUIRED COMPONENTS … SerialPort)`
- `target_link_libraries(lyra … Qt6::SerialPort)`
- COM-port enumeration for the Settings pickers via
  `QSerialPortInfo::availablePorts()`.

---

## 1. Lyra control surface these features reuse (verified)

The same surface `TciServer` drives (it's the WebSocket CAT-equivalent
and the construction/ownership/Settings pattern to mirror):

- **Freq:** `HL2Stream::rx1FreqHz()` / `setRx1FreqHz(quint32)`,
  `setVfoBHz(quint32)`. Carrier↔DDS offset via
  `WdspEngine::markerOffsetHz()` — CAT reports `carrier = dds +
  markerOffset` and sets `dds = v - off` (exactly TciServer's
  `dispatch()` freq handling).
- **Mode:** `Prefs::mode()` / `setMode(QString)` + `Prefs::modeChanged`;
  `WdspEngine::setMode`. TciServer maps via `toTciMode`/`fromTciMode`.
- **PTT:** `HL2Stream::requestMox(bool, PttSource)` +
  `moxActive()` / `moxActiveChanged(bool)`. The `PttSource` enum
  (`Manual=0, HwPtt=1, Tci=2`) gets a new **`Serial=3`** member +
  a `requestMoxFromSerialPtt(bool)` Q_INVOKABLE wrapper, mirroring
  `requestMoxFromHwPtt`/`requestMoxFromTci`. Wire behaviour is
  identical across sources (same TR-delay chain, ATT-on-TX raise);
  the source is metadata.
- **Run/power:** `HL2Stream::isRunning()`; for CAT `PS;` power on/off
  + `START`/`STOP`, mirror TciServer's `emit startRequested/stopRequested`
  to MainWindow (the server can't reach the connect flow directly).

**Construction/ownership pattern (mirror `mainwindow.cpp:433`):**
```cpp
tci_ = new TciServer(prefs_, qobject_cast<HL2Stream*>(stream_),
                     engine_, spots_, this);
```
Both new objects are built next to `tci_` with the same `prefs_` /
`stream_` / `engine_` pointers, parented to MainWindow, and passed
into `SettingsDialog` for their Settings UI (like `TciServer *tci`).
Everything runs on the **Qt main thread**, async via signals/slots —
NO worker QThread (matches the proven TciServer model; CAT/PTT never
touch the DSP/wire threads, they just call the setters above).

---

## 2. Feature 1 — Serial PTT input  (`src/cat/SerialPtt.{h,cpp}`)

Mechanism (the Thetis `SerialPortPTT.cs` `isCTS()`/`isDSR()` pattern):
- Operator picks a COM port + which input line to watch (CTS or DSR)
  + optional invert. The digital app asserts **RTS or DTR** on its
  end; on a com0com virtual pair that crosses to Lyra's **CTS/DSR**.
- `QSerialPort` opened read-only-ish (we only read modem lines).
  Qt has no portable modem-line-change signal, so **poll
  `pinoutSignals()` on a small QTimer** (~10–20 ms, like Thetis's
  poll thread). On a 0→1 edge → `stream_->requestMoxFromSerialPtt(true)`;
  1→0 → `requestMoxFromSerialPtt(false)`.
- Default-OFF, operator opt-in (same posture as the EP6 HW-PTT-in
  forwarder). Settings: enable, COM-port picker, line (CTS/DSR),
  invert. QSettings `"serialptt/…"`.

This also folds in the long-parked **footswitch-on-serial** need —
same mechanism, the source line is just driven by a switch instead
of an app.

**Complements CAT, not duplicates:** WSJT-X "PTT Method" can be CAT
(handled by §3's `RX;`/`TX;`) *or* RTS/DTR (handled here). Many
setups run CAT for freq/mode on one port + RTS/DTR PTT on a second.

---

## 3. Feature 2 — Kenwood CAT serial server  (`src/cat/CatServer.{h,cpp}`)

Mirrors `TciServer`'s shape (ctor, QSettings `"cat/…"`, `enabled` /
`setEnabled()` start-stop-persist, `start()/stop()`, radio-signal →
unsolicited-push handlers, `startRequested/stopRequested`).

**Transport:** `QSerialPort::readyRead` → append to a buffer →
extract every `;`-terminated command (handle partial reads — buffer
until `;`). Per-command: 2-letter command id, optional payload, `;`.
**Read vs set** distinguished by payload length (`FA;` = read,
`FA00014250000;` = set), exactly the Thetis/Kenwood convention.

**Rig model toggle (TS-480 default / TS-2000):** only the `ID;` and
`IF;` responses differ:
- `ID;` → `ID020;` (TS-480) or `ID019;` (TS-2000).
- `IF;` packed-status field layout per the selected model.
QSettings `"cat/rig_model"`.

**Core command set v1 (the logger essentials):**
| Cmd | Dir | Format | Lyra mapping |
|---|---|---|---|
| `FA` | r/w | `FA` + 11-digit Hz | `rx1FreqHz()` + markerOffset / `setRx1FreqHz` |
| `FB` | r/w | `FB` + 11-digit Hz | VFO-B (`setVfoBHz`); single-VFO today → echo A until RX2/split |
| `MD` | r/w | `MD` + 1 digit | Kenwood mode # ↔ Lyra mode (see map) |
| `IF` | r | packed status | freq/mode/RX-TX/split, model-specific layout |
| `FR` | r/w | `FR` + digit | RX VFO select |
| `FT` | r/w | `FT` + digit | TX VFO select (split) |
| `RX` | w | `RX;` | `requestMox(false, Manual)` (per enum doc: CAT→Manual) |
| `TX` | w | `TX;`/`TX0;` | `requestMox(true, Manual)` |
| `ID` | r | `ID0xx;` | rig-model id |
| `PS` | r/w | `PS` + 0/1 | `isRunning()` / `emit start/stopRequested` |
| `AI` | r/w | `AI` + 0..3 | auto-info: push unsolicited `IF`/`FA`/`MD` on radio change |

**Kenwood mode-number map (TS-480/2000 convention):**
`1=LSB 2=USB 3=CW 4=FM 5=AM 6=DIG(FSK) 7=CW-R 9=DIG-R`. Reuse the
Lyra↔token mapping logic already in `TciServer::toTciMode`/`fromTciMode`
(adapted to numbers).

**Auto-Info (`AI`):** when `AI` > 0, wire `Prefs::modeChanged` +
the stream freq-change signal to push an unsolicited `IF;`/`FA;`/`MD;`
— same broadcast-on-change pattern TciServer uses (`onFreqChanged` /
`onModeChanged`). Rate-limit like TciServer's `broadcast()`.

---

## 4. Build order (each independently shippable)

1. **CMake** — add Qt6::SerialPort + a `QSerialPortInfo` port-list
   helper. *Build-verify the toolchain has SerialPort.*
2. **Serial PTT input** (`SerialPtt` + `PttSource::Serial` +
   `requestMoxFromSerialPtt`) + Settings surface. Smallest, most
   immediately useful (WSJT-X/VarAC keying).
3. **CAT skeleton** — `CatServer` open/close + line assembler +
   `ID`/`PS`/`AI` + Settings + rig-model toggle.
4. **CAT read/set FA/FB/MD/IF/FR/FT** — freq + mode (logger essentials).
5. **CAT RX/TX** keyed PTT.
6. (later) Serial-PTT **input** invert/edge polish; CAT extras as
   testers ask.

## 5. User-guide note (when shipping)
Document the **virtual COM-port pair** (com0com on Windows): Lyra is
the "rig" on one end of the pair; WSJT-X/Log4OM/fldigi connect to the
other end. #1 thing users get stuck on.
