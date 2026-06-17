# Audio I/O (VAC) + TX/RX Profiles — grounded port plan

**Status:** PLANNING (2026-06-14).  Operator directive: "look hard at
Thetis, port likewise; tie audio in/out into Profiles; capture the FULL
plan now so we can add/trim."  This doc is the reference study + the
proposed lyra-cpp port plan + the open decisions.  NO code yet.

Reference = Thetis 2.10.3.13 (`D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13`)
+ operator's working-config screenshots (`Y:\hold\screenshots\`) + his
`Thetis_database_export_Default_5_16_2026_6_54 PM.xml`.  Provenance lives
here/docs only; shipped code stays Lyra-native, no reference name in
code/commits (the standing rule; WDSP/ChannelMaster ports carry the GPL
attribution header).

---

## 0. RESOLVED — TX Speech panel options not saved by Profiles (#162, fixed + registry-verified 2026-06-17)

Operator: the **TX Speech** panel's ON/OFF stages weren't saved by
Profiles while EQ/Combinator/Plate were. Decisive repro: De-esser ON,
recall an all-off profile → De-esser stayed ON.

**It WAS a capture/write bug (operator's instinct was right; my first
"QML binding-break / UI-refresh" theory was WRONG).** Ground truth came
from dumping the actual stored profile
(`HKCU\Software\N8SDR\Lyra-cpp\profiles\item\<name>` = the compact JSON):
in `DSP-TEST` the `eq`/`combinator`/`plate` blobs held real operator
changes (custom EQ gains, Combinator attack/ratio/SBC, Plate `bypass:false`),
but `speech` was **all-default** (`deessOn:false …`) even though the De-esser
had been turned ON — proving the panel changes never reached `SpeechModel`
at capture time.

**Root cause — QML signal shadowing.** `SpeechPanel`'s reusable `Stage`
card declares `signal toggled(bool v)` and the ON `Button` emitted it via
unqualified `onClicked: toggled(checked)`. But a Qt Quick `Button`
(`AbstractButton`) **already has a built-in `toggled` signal**, which
shadows the Stage's custom one inside the button's scope — so the click
emitted the *button's* `toggled` (a no-op here), the Stage's
`onToggled: (v) => Speech.X = v` never fired, and the model never updated.
The `checkable` button still flipped visually (why it *looked* ON but
saved OFF). Sliders were unaffected — they emit `parent.moved(value)`
(explicitly qualified → the CtlRow's signal). EQ/Combinator/Plate were
unaffected — none use a custom `toggled` signal; their toggles write the
model directly.

**Fix (SHIPPED to branch 2026-06-17, registry-verified):** `id: stage` on
the `Stage` component + `onClicked: stage.toggled(checked)` (qualify the
emit). One QML file, two lines; NO `checkable`/`Binding` changes. After
the fix, saving with De-esser ON + Thresh −11 + Range 5 stored
`deessOn:true, deessThreshDb:-11, deessRangeDb:5` — confirmed in the
registry.

**Lesson:** a clean build + qmllint is NOT proof — an earlier inferred
"fix" (non-checkable toggles + a fighting `Binding` on draggable sliders)
made the whole Speech panel inert and was reverted. Verify UI changes by
launching/clicking (or, for capture, dumping the stored profile bytes)
before declaring done.

**Residual (separate, cosmetic, NOT fixed):** after clicking a `checkable`
toggle in-session, its `checked: <model>` binding is severed, so recalling
a *different* profile updates model+DSP+next-save correctly but may not
refresh the button's lit state. Startup/first-recall is fine. Handle
carefully + self-verify if pursued.

---

## 1. Reference findings — how Thetis does audio I/O

### 1.1 Host audio = the IVAC engine (`ChannelMaster/ivac.c`)
Thetis does NOT hand-roll PortAudio in C#.  Host audio in/out is the
**IVAC** subsystem — a ChannelMaster C unit that internally owns:
- the **PortAudio** device open (host-API index + input/output device
  index: `SetIVAChostAPIindex`, `SetIVACinputDEVindex`, output dev),
- the **adaptive resampler** (varsamp/rmatch PI-loop) bridging the PC
  soundcard clock ↔ the radio's 48 kHz wire clock — the two-clock-drift
  fix, with the live **Var Ratio / Overflow / Underflow** readouts seen
  on the VAC1 screenshot,
- the **ring buffers** (RB In/Out + PortAudio In/Out latency, manual or
  auto).

Driven entirely through a `SetIVAC*` API (ivac.c PORT exports):
`SetIVACrun` `SetIVACiqType` `SetIVACstereo` `SetIVACvacRate`
`SetIVACmicRate` `SetIVACaudioRate` `SetIVACvacSize` `SetIVACmicSize`
`SetIVACiqSizeAndRate` `SetIVAChostAPIindex` `SetIVACinputDEVindex`
(+ output dev) `SetIVACRBReset` … and gain RX/TX.

There are **two instances: IVAC id 0 = VAC1, id 1 = VAC2** (independent
device + rates + enable), exactly the operator's "some users route
differently and need both."

### 1.2 The VAC1/VAC2 control surface (Setup → Audio → VAC 1 / VAC 2)
Ports to (from the screenshot, all per-VAC, all in the TX profile or
global per the apply path in §1.4):
- **Driver / host API** (MME / WASAPI / WASAPI-exclusive / WDM-KS /
  DirectSound / ASIO), **Input** device, **Output** device, **Enable**
- **Sample rate** (48000), **Buffer size** (2048), **Mono/Stereo**
- **Gain dB: RX / TX** (per-direction trim)
- **Direct I/Q → Output to VAC** (raw IQ out instead of demod audio;
  + SwapIQ, Use RX2) — advanced/IQ-recording
- **Buffer Latency (ms): RingBuffer In/Out + PortAudio In/Out**, each
  Manual or Auto — the latency knobs (cf. our §15.7-class tuning)
- **Auto Enable: "Enable for Digital modes, Disable for all others"**
  ← the DIGU/DIGL auto-switch
- **Override/bypass-for-Phone group**: Allow PTT / SPACE / MOX to
  override-bypass VAC; **VOX uses MIC instead of VAC**; **Mute will
  mute VAC**
- Live telemetry: TO-VAC(Out)/FROM-VAC(In) Overflows / Underflows /
  Var Ratio / RingBuffer fill % + the two scope strips

Audio sub-tabs present: **VAC 1, VAC 2, Options, Advanced, Recording**
(only VAC1 captured; VAC2 mirrors it; Options/Advanced/Recording not yet
captured — request if needed).

### 1.3 The TX Profile (Setup → Transmit) — the bundle
A TX Profile is a **DB row** (operator's export has the columns).
Screenshot + source confirm it bundles:
- **Transmit Filter High / Low** (= TX bandwidth, 50…4995 shown)
- **Mic: Source (Mic In / Line In)** + **Gain Max/Min** + **20 dB Boost**
- **Tune**: TX-meter mode, Use Drive / Tune / Fixed-Drive (+ value)
- **Monitor**: TX AF level, Ignore-Master-AF-Change
- **AM Carrier Level**
- **Speech Processor**: CESSB Overshoot Control; Limit-Drive-on-Ext-Amp
- **External TX Inhibit** (update-with-state / reversed-logic)
- **`VAC1_Auto_On`** (the digital-mode auto-enable — a profile column,
  setup.cs:3665 writes / :9657 applies)
- Optionally, gated by checkboxes: **VAC1 device details**, **VAC2
  device details**, **PA profile** (see §1.4)

Profile UI: dropdown + **Save / Delete**, **Additional TX Profiles**
list (Default / Default DX / Digi 1K@1500 / Digi 1K@2210 / AM /
4K-N8SDR-AM …) + **Include** + **Export Current Profile**.
Behaviour toggles: **Auto Save TX Profile on change**, **on Thetis
close**, **Highlight TX Profile Save Items**, and the orange
"TX Profile modified — Save profile to store" banner.

### 1.4 The profile↔audio tie-in (the load-bearing wiring)
Source-verified (`Console/setup.cs`):
- `VAC1_Auto_On` is a TX-profile column: written :3665, read-applied
  :9657 (`chkAudioVACAutoEnable.Checked = (bool)dr["VAC1_Auto_On"]`).
- **Restore-VAC-from-profile is OPT-IN, gated per checkbox:**
  `chkRestoreVAC1DeviceDetailsFromTXProfile` (:9536) /
  `chkRestoreVAC2DeviceDetailsFromTXProfile` (:9564) — only when ticked
  does switching profile also re-point the VAC device/driver.  So:
  profile ALWAYS carries the digital-auto-enable + gain + BW + mic
  source; profile carries the VAC *device selection* only if the
  operator opts in (otherwise device stays put, settings recall).
- `console.VACAutoEnable` (console.cs:13465) is the live flag the mode
  change consults: on entering DIGU/DIGL with VACAutoEnable set, VAC is
  auto-enabled; on leaving, auto-disabled (the "Enable for Digital,
  Disable for others" semantics).

### 1.5 DSP buffer/latency surface (Setup → DSP → Options) — adjacent
Operator-flagged "we are missing this": per-mode (SSB/AM, FM, CW,
Digital) **RX/TX Buffer Size (IQcomp)** + **Filter Size (taps)** +
**Filter Type (Low Latency / …)** + **Filter Windows** + **WDSP Cache
Impulse Data / Save-Restore cache**.  This is a DSP-tuning surface, NOT
audio-routing — tracked as its own item; relevant to latency but
independent of the VAC/profile port.  (Lyra already ships WDSP WISDOM-
equivalent? — verify; the "Filter Impulse Cache" maps to the wisdom
work the Python build did.)

---

## 2. The lyra-cpp architecture decision (the big one)

Lyra-cpp has **no host-audio subsystem today** — RX out = AK4951 jack
over the wire (`dispatchAudioFrame → OutBound(0)`), mic in = AK4951 over
the wire (EP6 slot 24-25), single-crystal, no resampler.  Adding PC/VAC
in+out is net-new infrastructure that serves BOTH directions.  Two ways:

**Option A — PORT `ivac.c` (recommended, "do as Thetis does").**
Port the IVAC engine into lyra-cpp source exactly as `cmbuffs`/`cmaster`/
`aamix`/`ilv`/`obbuffs`/`networkproto1` were (verbatim, GPL-attributed,
the `wire/` discipline).  IVAC brings its own PortAudio device I/O +
varsamp/rmatch resampler + rings; Lyra drives it with the `SetIVAC*` API
through a new `wire/IVAC.{h,cpp}` + wdspcalls entries.
- PROS: maximally reference-faithful (the project's proven discipline);
  the two-clock resampler + ring orchestration come for free, battle-
  tested; VAC1/VAC2 = IVAC id 0/1 by construction; the
  override-bypass/auto-enable/gain semantics port 1:1.
- CONS/decision: **adds a PortAudio dependency** to the bundle (Thetis
  ships it too).  Need PortAudio built/bundled in `_native/` alongside
  the WDSP/FFTW DLLs.

**Option B — Qt-native host audio (`QAudioSource`/`QAudioSink`) + port
only the rmatch/varsamp resampler.**
- PROS: no PortAudio dep (Qt Multimedia already linked); device
  enumeration is Qt-idiomatic.
- CONS: we re-implement the ring + PI-loop orchestration that ivac.c
  already solves; diverges from the port discipline; more original
  real-time code on the dangerous wire/audio boundary (the class of
  thing that bites — cf. the Python build's rmatch effort).

**Recommendation: Option A (port ivac.c).**  Consistent with every
other ChannelMaster unit, lowest original-RT-code risk, and the
operator's explicit "port likewise."  The PortAudio bundle is the one
real cost; it's a known, GPL-compatible dep Thetis already relies on.

---

## 3. Profiles = the container (build first / alongside)

The audio source+route is a **profile field**, so Profiles is the
subsystem the audio-I/O work plugs into.  Design:

### 3.1 Profile model
A `TxRxProfile` = a named bundle of operator-facing TX/RX state.  Fields
(superset, trim later):
- **TX**: tx_filter_low/high (BW), mic_source (mic1/linein/tci/vac1/
  vac2), mic_gain, mic_boost, tune/drive mode + value, monitor AF,
  AM carrier, CESSB, PHROT, (EQ/Combinator/tube/de-esser when they
  land), TX-meter mode.
- **RX**: rx_filter_low/high (the 🔗 BW lock pairing), AGC profile,
  audio out sink (AK4951 / VAC1 / VAC2 / monitor), volume/AF.
- **AUDIO ROUTE**: vac1_enable + auto-enable-for-digital, vac1 device
  details (opt-in restore), vac1 gain RX/TX; same for vac2.
- **mode binding** (the operator's DIGU→TCI): optional `bind_mode`
  field so a profile auto-recalls when the operator switches to that
  mode (the Lyra extension over Thetis's VAC-only auto-enable).

### 3.2 Storage + apply
- **QSettings JSON** under `tx_profiles/` (matches the existing #49
  spec + the Python project's pattern), one JSON dict per profile;
  `default_profile` key.  (Thetis uses a DB; QSettings-JSON is the
  Lyra-native equivalent — no SQLite dep.)
- **Apply atomically**: setting a profile pushes ALL fields through the
  existing Radio setters in one pass (BW, mic source via the §App-audio
  source selector, gains, VAC enable/route).  Mirrors Thetis's apply.
- **Opt-in device restore** (Thetis `chkRestoreVAC*DeviceDetails`): a
  toggle for whether profile-switch also re-points the VAC device, vs
  recall-settings-but-keep-device.
- **Auto-Save-on-change** + **Save/Delete/Export** + "profile modified"
  indicator (the orange-banner equivalent) + per-mode binding table.

### 3.3 Reconcile with the locked tasks
- **#49 (TX Profile Manager) spec AMENDMENT:** the line "Manual select
  only — NO auto-detect by call" was about **callsign** auto-detection
  (still rejected).  **Per-mode auto-recall is DIFFERENT and now in
  scope** (operator-confirmed 2026-06-14): a profile may bind to a mode
  (DIGU/DIGL→digital profile w/ source=TCI).  No callsign magic.
- **#55** (RX/TX quick-preset chips) folds in as the front-panel
  recall UI for these profiles.
- **#102/#103** (VAC1/VAC2 bridges) become "IVAC id 0/1" under the
  ported engine — BOTH built (operator: users route differently).
- **#104** (HL2 Line In) = a mic-source token (codec C&C mux), a
  profile field, independent of IVAC.
- **#90** (monitor out) = a third IVAC/host-audio output instance or a
  dedicated monitor sink — a profile field.

---

## 4. Unified source/sink model (what the profile selects)
- **TX audio IN sources** (exactly one active, the `InboundTCITxAudio`
  seam; `TciTxBridge` is the existing template, IVAC mic-in is the new
  sibling): `mic1` (AK4951) · `linein` (AK4951 mux) · `tci` ·
  `vac1` · `vac2`.
- **RX/monitor audio OUT sinks**: `ak4951` (wire, current) · `vac1-out`
  · `vac2-out` · `monitor`.  IVAC handles the host ones + resample.
- A new thin `ITxAudioSource` interface is justified now (4+ concretes)
  — supersedes the tx1_ssb_design §5.4 "single class" note (that note
  predates having >1 host source; flag it for amendment).

---

## 5. Proposed staged build order (revisit after operator review)
0. **Profile model + storage + apply-on-switch** (no audio yet) — the
   container; wire the EXISTING fields (BW, mic source mic1/tci, gains)
   through it first so "switch profile → state recalls" works before
   any host audio.  Low risk, immediately useful, unblocks everything.
1. **Port `wire/IVAC.{h,cpp}`** (+ PortAudio bundle + wdspcalls
   `SetIVAC*`) — the host-audio engine, wire-inert until driven.
2. **VAC1 RX-out** (playback only, no PTT coupling — lowest risk;
   "listen on PC" + digital-decode out).
3. **VAC1 mic-in** (TX source; back into keying/PTT territory — careful,
   reuse the §15.25 ordering).
4. **VAC2** (id 1 — second independent device; small increment once #1-3
   exist).
5. **Auto-enable-for-digital + per-mode profile binding** (the DIGU→TCI
   recall).
6. **Monitor out (#90)** + Options/Advanced/Recording tab parity.

Each stage: grounded design → red-team (charter §6 if it touches the
wire/RT path) → operator HL2 bench.  (Note: the process-isolation
charter in the Python project's notes is a SEPARATE codebase — does NOT
bind lyra-cpp; lyra-cpp's discipline is the verbatim-port + HL2-bench
methodology.)

---

## 6. Open decisions for the operator
1. **Option A (port ivac.c + PortAudio bundle) vs B (Qt-native + port
   resampler)** — recommend A.  Confirm the PortAudio-dependency is
   acceptable.
2. **Profile storage**: QSettings-JSON (recommended, no new dep) vs a
   SQLite DB like Thetis.
3. **Scope trim**: which §3.1 fields ship in the first profile cut vs
   later (EQ/Combinator don't exist yet — profile schema should reserve
   them but not block).
4. **Per-mode binding UX**: a simple "bind this profile to mode X"
   dropdown in the profile manager, vs Thetis's VAC-only auto-enable.
5. **Remaining screenshots** to mine for parity: VAC2 / Audio-Options /
   Audio-Advanced / Recording / general-radio-model / DDC-and-ADC —
   read now for the full surface, or defer.

---

---

## 7. DECISIONS LOCKED (operator-confirmed 2026-06-14)

1. **Option A — PORT `ivac.c`** ("do as Thetis does").  PortAudio
   dependency accepted + bundled.  Both VAC1 + VAC2 (IVAC id 0/1).
2. **Storage = QSettings-JSON** (no SQLite dep).
3. **Profiles-FIRST sequencing** — Stage 0 = the profile model/container
   before any host audio.
4. **DSP per-mode buffer/filter/latency surface** (the "DSP Buffers we
   are missing this" shot: SSB/AM·FM·CW·Digital RX/TX buffer-size
   (IQcomp) + filter taps + filter type/window + WDSP impulse cache) —
   **TRACKED as its own item, do NOT lose it**; wire in once the
   per-mode DSP-settings surface is added.  Separate from the VAC port.
   → task created.
5. **Deferred Thetis audio sub-tabs** (Audio → Options / Advanced /
   Recording) + `general radio model` + `DDC and ADC` — TRACKED for
   later surface-parity mining; pull from Thetis source when we decide
   to add the equivalent options.  → noted in the IVAC-port task.

### Front-facing Profile UI (operator-confirmed)
Mirror Thetis's two-surface model:
- **Front-panel profile dock** (Lyra `GlassPanel`, one-click recall +
  quick change) — this IS task **#55** (operator-named profile chips /
  dropdown).  Thetis places it lower-center/right on the main screen;
  Lyra = a dockable panel.
- **Settings → Profiles tab** = the full editor (Save / Load / Delete /
  Export / Set-default / per-field capture + the per-mode binding) —
  this IS task **#49** (the container, Stage 0).
Both read/write the same QSettings-JSON profile store; the front dock is
a thin recall/!modified view over the Settings-tab model.

*Next: produce the detailed per-stage design (Profile schema + apply
path + front-dock/Settings-tab UI; then the IVAC port surface) and begin
Stage 0 (Profile model).*
