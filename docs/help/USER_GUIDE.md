# Lyra — User Guide

A friendly, plain-language guide to operating Lyra. Written for hams,
not programmers — if you can click a menu, you can use this.

> **About this guide.** It grows feature-by-feature as Lyra is built.
> Each section describes what a control does, where to find it, and the
> sensible defaults — so nothing ships undocumented. This file is also
> the source for Lyra's in-app **Help** section.
>
> **Maintainers:** when a user-facing feature lands, add or update its
> section here in the same change. A feature isn't "done" until it's in
> this guide.

---

> **Tip — the "?" badge.** Every panel has a small cyan **?** at the
> far-right of its title bar (the panel's name is on the far left). Click
> it for a tiny menu: **Help guide** jumps this guide to that panel's
> section, **Settings…** opens the matching Settings tab. Quick way to
> learn or adjust any panel.

## Contents

- [Why "Lyra"?](#why-lyra)
- [Getting started](#getting-started)
- [The header (top toolbar)](#the-header-top-toolbar)
- [The status bar (bottom — HL2 telemetry)](#the-status-bar-bottom--hl2-telemetry)
- [Getting around the window](#getting-around-the-window)
- [The panadapter (spectrum display)](#the-panadapter-spectrum-display)
- [Tuning panel](#tuning-panel)
- [Mode + Filter panel](#mode--filter-panel)
- [Band panel](#band-panel)
- [Audio panel](#audio-panel)
- [Display panel](#display-panel)
- [Meter panel](#meter-panel)
- [TX panel](#tx-panel)
- [Solar / Propagation panel](#solar--propagation-panel)
- [Weather alerts](#weather-alerts)
- [Updates](#updates)
- [Backing up & sharing your settings](#backing-up--sharing-your-settings)
- [Operating with an external amplifier (hot-switch protection)](#operating-with-an-external-amplifier-hot-switch-protection)
- [Settings → Hardware](#settings--hardware)
  - [Operator / Station](#operator--station)
  - [Band plan (Region)](#band-plan-region)
  - [Diagnostics (debug log)](#diagnostics-debug-log)
  - [Radio](#radio)
  - [Transmit (PA enable + safety timeout + hardware PTT)](#transmit-pa-enable--safety-timeout--hardware-ptt)
  - [Filter board (N2ADR / compatible)](#filter-board-n2adr--compatible)
  - [USB-BCD (linear-amp band switching)](#usb-bcd-linear-amp-band-switching)
- [Settings → Audio](#settings--audio)
- [Settings → TX (TR sequencing + cos² fade)](#settings--tx-mic--alc-tr-sequencing--cos-fade)
- [Settings → Network (TCI)](#settings--network-tci)
  - [DX-cluster spots](#dx-cluster-spots)
- [Settings → Visuals](#settings--visuals)
  - [Trace color](#trace-color)
  - [Spectrum fill](#spectrum-fill)
  - [Peak markers](#peak-markers)
  - [Noise-floor line](#noise-floor-line)
  - [Trace smoothing](#trace-smoothing)
  - [Peak glow](#peak-glow)
  - [Glass sheen](#glass-sheen)
  - [Gridline brightness](#gridline-brightness)
  - [Frame rate](#frame-rate)
  - [dB range (floor / ceiling)](#db-range-floor--ceiling)
  - [Watermark](#watermark)
  - [Meteors](#meteors)
  - [Graphics backend](#graphics-backend)
- [Settings → Weather](#settings--weather)
- [Credits and References](#credits-and-references)

---

## Why "Lyra"?

The openHPSDR naming tradition leans Greek. The radio hardware itself is
**Hermes** (the messenger god). Thetis — the venerable reference PC
client — is named after the sea-nymph mother of Achilles. New projects in
this ecosystem have historically reached into the same mythology for
names that share the family.

**Lyra** continues that tradition:

- **Apollo's lyre** was the instrument that turned invisible vibrations
  in the air into music — which is essentially what an SDR does: it takes
  vibrations in the electromagnetic field, digitizes them, and turns them
  back into something you can hear.
- The **constellation Lyra** contains **Vega** — a star with a long
  history of amateur-radio significance (Vega is the zero point of the
  astronomical magnitude scale, and was the first star ever photographed).
  That's the lyre + Vega you'll find as the faint watermark on the
  panadapter.
- It's short, pronounceable in every language the HPSDR community uses,
  and stays well clear of any existing SDR software or hardware product
  name.

Lyra is built by **Rick Langford (N8SDR)**. This version is a ground-up
native **C++23 / Qt 6** rebuild (Qt Quick + Vulkan/RHI) of the original
Python Lyra — same spirit, no Python and no GIL anywhere. Licensed
**GPL v3 or later**, to align with the wider openHPSDR / WDSP ecosystem.

73 and good DX.

---

## Getting started

### 1. Know your hardware

- **HL2** (plain) — a stock Hermes Lite 2 board. Received audio is
  decoded on the PC and played through whatever output you pick in
  **Settings → Audio**.
- **HL2+** — the HL2 base **plus** the AK4951 audio add-in board (and the
  updated HL2+ gateware). Adds on-board audio routing and a microphone
  input for future transmit. The HL2's own headphone jack is the default,
  lowest-latency audio path.

### 2. Network

The HL2 is a Layer-2 Ethernet device speaking HPSDR Protocol 1 over UDP
port 1024. It has to be reachable on your local network:

- **Direct Ethernet** PC ↔ HL2 — simplest, no switch needed.
- **Same LAN** through any gigabit switch — also fine.
- **Across routers** — not supported (discovery is broadcast-only).

The first time you run Lyra, allow it through **Windows Firewall** so it
can talk to the radio (inbound UDP 1024 for `lyra.exe`).

### 3. First contact

Connecting to the radio lives in **Settings → Hardware** — open Settings
(**Ctrl+,** or **File → Settings…**), pick the **Hardware** tab, and find
the **Radio** section. Or just use **▶ Start** on the toolbar:

1. Power on the HL2 / HL2+ and make sure it's on the same network.
2. Click **▶ Start** on the toolbar (or **Discover** → **Open** in
   Settings → Hardware → Radio). Lyra also auto-connects to the last radio
   you used, so it may already be running.
3. You should see a live spectrum and hear the band noise floor. **■ Stop**
   disconnects.

### 4. First-launch note (one-time)

The very first time Lyra runs (and after a settings reset), it builds
optimized signal-processing plans ("FFT WISDOM"). A small window says so;
it takes a few minutes once, then Lyra opens normally and every launch
after is fast. Let it finish.

### 5. Tune and listen

- **Tuning panel** — set the **RX1** frequency on the LED readout; see
  [Tuning panel](#tuning-panel).
- **Mode + Filter panel** — pick the **mode** (USB/LSB/CW/…) and **filter
  bandwidth**; see [Mode + Filter panel](#mode--filter-panel).
- **Band panel** — jump between bands.
- **Audio panel** — the **DSP + AUDIO** strip: **Vol / MUTE**, the **AGC**
  cycle, and **Noise Reduction** (NR on/off + Mode 1–4 + AEPF + NPE).
  Choose the output
  device in **Settings → Audio**.
- **Panadapter** — click/drag/wheel to tune; drag the right edge to set
  the signal-strength scale. See [The panadapter](#the-panadapter-spectrum-display).

### Finding your version

The version is shown in the **title bar** and under **Help → About
Lyra…**. Include it when reporting a problem so it can be matched to the
exact build.

---

## The header (top toolbar)

The strip across the top, between the menu bar and the panels:

- **▶ Start / ■ Stop** — connect to the radio and start streaming, or stop
  it. On Start, Lyra connects to your saved radio (or scans and opens the
  first one found).
- **Connection status** — "Disconnected" / "Connected to …".
- **● TCI** — the TCI-server indicator, just after the connection status.
  Green **● TCI: N** when one or more programs (logger, cluster, etc.) are
  connected, showing the client count; amber **● TCI** when the server is
  enabled but idle. Hidden when TCI is off. See
  [Settings → Network (TCI)](#settings--network-tci).
- **★ Spotted!** — appears just before the clocks when *your own callsign*
  is spotted on the DX cluster (if the toast/badge option is on). See
  [DX-cluster spots](#dx-cluster-spots).
- **⬆ vX.Y.Z** (only when present) — an update is available; click it to
  open the release page. See [Updates](#updates).
- **Clocks** — a **local** clock (amber) and a **UTC / Zulu** clock (cyan,
  ending in *Z*), centered in the header. They read straight off your PC
  clock. **Right-click either clock** to *Check clock drift now…* — Lyra
  asks a network time server how far off your PC clock is and tells you
  (under 1 sec = fine). This matters for the **NCDXF beacons**: their
  rotation is locked to UTC on 10-second slots, so a PC clock that's off by
  a few seconds makes the beacon markers / Follow track the *wrong* station.
  If your clock is off, a **⚠** appears on the UTC clock; use the same
  right-click menu's *Sync time (w32tm /resync)* to fix it (Windows).
- **Weather badges** — ⚡ lightning, 💨 wind, ⚠ severe — appear toward the
  right edge when an alert is active and **flash** on the most serious
  tier. Hidden when all-clear. See [Weather alerts](#weather-alerts).

---

## The status bar (bottom — HL2 telemetry)

The thin strip across the bottom of the main window. While a stream
is connected, it shows live **HL2 telemetry** read from the EP6
status rotation:

```
HL2:  T 24.7°C    V 12.3 V    PA 0.00 A
```

- **T** — board temperature in °C, sampled every ~400 ms.
- **V** — HL2 supply voltage in volts (a normal HL2 / HL2+ reads
  **12.3 – 13.0 V**; well below ~11 V suggests the supply is sagging
  under load).
- **PA** — combined PA drain current in amps. The key telemetry
  during transmit:
  - **≈ 0 A** at rest (PA bias disabled, gateware idle).
  - **~0.2 A** at idle bias (PA Enable ON, MOX keyed, Drive = 0 %).
  - **~1.8 A** at full output on a bare HL2+ on the dummy load.
  - **Goes BOLD RED at ≥ 50 mA** — visual confirmation that the
    gateware is producing RF. If you see red and your wattmeter
    reads zero, something in the chain (relay, BPF, antenna) is
    eating the RF.

`n/a` in any field means the gateware hasn't delivered that slot
yet (briefly at startup) or the telemetry decode is mid-rotation.
A persistent `n/a` is a stream / gateware issue worth investigating.

> The PA-current readout is the **single most useful TX bench
> instrument** Lyra exposes. It's the gateware's own measurement,
> independent of any external watt-meter, and tells you whether
> the PA is actually biased + drawing current the way it should
> be for the drive you commanded.

---

## Getting around the window

Lyra's window is built from **dockable panels** — like a workbench you
arrange to taste. Lyra opens **full screen (maximized)** by default — the
way it's meant to be run — in a curated default layout (panadapter on top,
control panels in a row beneath).

- **Move a panel:** drag it by its title bar. Drop it against an edge to
  re-dock, or out on its own to float.
- **Resize the spectrum vs. waterfall:** drag the divider between the
  panadapter and the waterfall — the drag is smooth and the waterfall stays
  clean even when squashed into a thin strip. Its position is remembered
  with your layout.
- **Show / hide panels:** the **View** menu lists every panel; tick or
  untick to show or hide it.
- **Lock the layout:** **View → Lock panels** (or **Ctrl+L**) freezes
  everything so you can't nudge a panel by accident during operating.

**Saving a layout you like.** Once arranged the way you want:

- **View → Save current layout as my default** — snapshots the current
  arrangement (panels *and* the spectrum/waterfall divider).
- **View → Restore my saved layout** — snaps back to that snapshot.
- **View → Reset to default layout** — returns to Lyra's built-in
  arrangement. (Use this if a layout ever gets scrambled.)

Your window size, layout, and divider position are remembered between
sessions. To move a layout to another PC or keep a backup, see
[Backing up & sharing your settings](#backing-up--sharing-your-settings).

**Opening Settings:** **File → Settings…**, or press **Ctrl+,**.

---

## The panadapter (spectrum display)

The panadapter is the live picture of the radio spectrum — signals show
up as peaks rising out of the noise floor. Lyra draws it with a glassy,
glowing look that takes advantage of your graphics card.

**Tuning on the panadapter:**

- **Click** anywhere to tune RX1 there. Where it lands depends on the
  **Exact / 100 Hz** setting and the **Panafall step** on the
  [Display panel](#display-panel): in **Exact** the click snaps to the
  Panafall-step grid (1 Hz = truly exact), in **100 Hz** it rounds to the
  nearest 100 Hz.
- **Click + drag** left/right to pan across the band.
- **Mouse wheel** steps the frequency by the **Panafall step** (set on the
  Display panel). **Ctrl + wheel** zooms instead.
- A small **frequency readout** follows your cursor (toggle in
  Settings → Visuals).

**The RX filter passband** is shown as a translucent box over the tuned
signal. **Drag either edge** of the box to widen or narrow the receive
bandwidth — the **RX BW** readout in the Mode + Filter panel updates to
match (and is remembered per mode). The orange center line marks your
tuned carrier (offset into the passband on CW, where the tone sits).

**The dB scale (signal-strength scale) down each side.** The numbers on
the left and right edges show signal strength in dB. Adjust by **dragging
on the right-hand edge**:

| Where you drag (right edge) | What it does |
|---|---|
| **Top third** | Raises / lowers the **ceiling** (top of the scale) |
| **Bottom third** | Raises / lowers the **floor** (bottom of the scale) |
| **Middle third** | **Pans** the whole scale up or down together |

Dragging **up** always raises that edge. (Exact numbers live in
**Settings → Visuals → dB range**.)

**The frequency scale** runs along the bottom in MHz, centered on your
receive frequency. **Zoom** in/out from the [Display panel](#display-panel).

**Overlays you can turn on:** [peak markers](#peak-markers) (held signal
peaks) and a [noise-floor line](#noise-floor-line) — both configured in
Settings → Visuals, with quick controls on the Display panel; the
[EiBi shortwave schedule](#shortwave-broadcasters-eibi); and
[DX-cluster spots](#dx-cluster-spots) fed in over TCI.

---

## Tuning panel

Sets the **RX1 receive frequency** on a big amber **LED-style readout**
(megahertz · kilohertz · hertz), the way old Lyra showed it.

- **Click a digit** to select it (a cyan underline marks it), then **roll
  the mouse wheel** over the display to tune that digit's place up/down;
  the **arrow keys** also nudge it.
- **Roll the wheel** anywhere on the display to tune by the current
  **Step** (see below).
- **Double-click** the display to type an exact frequency.
- **Step** — the tune step for the wheel: **1 Hz / 10 Hz / 100 Hz / 1 kHz
  / 10 kHz** (default **1 kHz**).
- **CW Pitch** — only shown in CW modes (CWU/CWL): your preferred sidetone
  / beat-note pitch, **200–1500 Hz** (default 600). The receive filter
  centers on this pitch and the tuned-carrier marker offsets to match, so
  a signal you zero-beat lands at your chosen tone.

The Lyra logo sits at the left of the panel. To jump between bands, use
the **Band panel**; to change mode or filter width, the **Mode + Filter**
panel.

---

## Mode + Filter panel

Sets how RX1 demodulates and how wide the receive filter is.

- **Rate** — the IQ sample rate / panadapter span: **96 k / 192 k /
  384 k**. Higher shows more spectrum at once (wider waterfall) for a bit
  more CPU/network.
- **Mode** — **LSB, USB, CWL, CWU, DSB, AM, FM, DIGU, DIGL**.
- **RX BW** — the receive filter bandwidth. The list is **per-mode**
  (sensible presets for SSB, CW, AM, FM, digital, with SSB running up to
  10 kHz and AM up to 12 kHz for ESSB-style wide audio), and Lyra
  remembers the width you last used for each mode. If you **drag a
  passband edge** on the panadapter to a width that isn't a preset, the
  combo shows it as **"(custom)"** at the top of the list so the readout
  always matches what you're actually hearing; pick a preset to snap back.
- **🔗 (Lock)** — links RX and TX bandwidths so changes to either side
  mirror the other for the current mode. Click to toggle. Toggling it
  ON pulls the RX bandwidth into TX. With the lock OFF, RX and TX BW
  are independent per-mode.
- **TX BW** — the transmit filter bandwidth (high edge for SSB).
  Per-mode just like RX BW. The actual TX passband is the **Filter
  Low edge** (set in Settings → Audio, shared with RX) up to this
  high edge — e.g. with Filter Low = 70 Hz and TX BW = 4 kHz, your
  TX runs a 70-4000 Hz bandpass. Sign-codes correctly per mode (USB
  positive, LSB negative-and-swapped) inside the WDSP TXA chain.

SSB and digital filters open at the **Filter Low edge** (Settings → Audio
→ Filter Low edge, shared RX+TX, default 100 Hz); CW filters are
centered on your **CW Pitch**; AM/DSB/FM are symmetric around the
carrier (the Filter Low edge doesn't apply to those modes).

---

## Band panel

Quick band switching, in three rows:

- **Ham** — the HF/6m amateur bands (**160m … 6m**). Click one and Lyra
  returns RX1 to **the last frequency you were on in that band** (the
  band's default the first time). The button for the band you're on lights
  up (red-glow), following the frequency however you tune.
  An optional **11m** button (the **Citizens Band**, 26.965–27.405 MHz AM)
  appears at the end of this row, right after 6m, when you enable it in
  **Settings → Hardware → Band panel**. When you're tuned on it, the
  panadapter marks all **40 CB channels** (Ch1…Ch40) across the top —
  **click a channel marker to jump to it** in AM.
- **BC** — the shortwave **broadcast** meter bands (**120m … 13m**, all
  AM). Like the Ham bands, BC and the 11m band **remember their last
  frequency and mode** and return to it when you click them (band default
  the first time). The active band lights the same way.
- **Gen** — the GEN1/2/3 general-coverage slots (below).

**GEN1 / GEN2 / GEN3** (to the right of the band buttons) are
**general-coverage slots** for listening outside the ham bands —
shortwave, MW, time stations, utility. Each works like a band button but
for any frequency: click it to jump to that slot's last freq + mode, and
while it's the active slot (highlighted) **tuning around updates it**, so
it always remembers where you left off. Defaults are WWV 10 MHz, WWV
15 MHz, and 1 MHz (MW), all AM. **Right-click** a GEN button to reset it
to its default. Clicking an amateur band button leaves general-coverage.

**TIME — HF time stations.** Between GEN3 and Mem, the **TIME** button
hops through the world's shortwave time-and-frequency stations — handy for
checking propagation or setting your clock by ear:

- **Left-click TIME** to tune the next station/frequency. It cycles through
  the whole list — WWV (2.5/5/10/15/20/25 MHz), WWVH, CHU, BPM, RWM, and
  others — advancing one step each click, and picks up where you left off
  next time. The mode is set for you (AM, or **USB for CHU**).
- **Right-click TIME** for the full list grouped by station — pick any
  station and frequency directly, or **Reset cycle to first entry**.
- The stations are ordered to put the most useful ones first based on your
  callsign (a US operator leads with WWV, etc. — set your callsign in
  **Settings → Hardware**).

**Mem — frequency memory bank.** At the end of the GEN row, the **Mem**
button holds up to **20 saved frequencies**, each remembering its name,
frequency, mode, and (optionally) RX filter bandwidth:

- **Left-click Mem** to open the recall list — pick a preset and Lyra
  tunes straight to it (mode first, then frequency).
- **Right-click Mem** to **Save current frequency** (the VFO is stored with
  an auto-name like "14.074 USB") or open **Manage presets…**.
- Full editing lives in **Settings → Bands → Memory**: a table where you
  can rename, retype the frequency/mode/bandwidth, add notes, delete, clear
  all, and **import/export CSV** (columns: Name, Freq_Hz, Mode, RX_BW_Hz,
  Notes) to back up or share your list.

Pick the mode and filter for the band in the **Mode + Filter** panel.

**Per-band memory.** Lyra remembers each band's settings independently and
restores them when you move to that band: the **last frequency** (recalled
by the band buttons), the **demod mode**, the **panadapter dB min/max**
(reference-level range), and the **waterfall dB min/max**. Set 40m to LSB with a tight waterfall range and 20m to a wider
one, and each comes back the way you left it as you hop between them — no
need to re-adjust every time. (Auto-scale on/off is global; RX filter
bandwidth is remembered per *mode*, as before.) Memory is saved live as you
adjust, and persists across sessions.

---

## Audio panel

The **DSP + AUDIO** panel — what you hear and how it's cleaned up. It's
laid out in old Lyra's three-row arrangement:

**Row 1 — Levels**
- **Vol** — output volume, shown in dB beside the slider (−∞ when fully
  down). The mouse wheel nudges it in fine steps.
- **MUTE** — silences or restores the audio without disturbing the Vol
  slider; the button reads **MUTED** while engaged. (Lyra starts
  **unmuted**.)
- **LNA** — RF input gain on the HL2's AD9866 PGA (−12…+31 dB; slider or
  mouse-wheel). Higher = more sensitivity; back off on strong bands to
  avoid ADC overload. The S-meter compensates for it automatically, so
  changing LNA doesn't shift the signal reading.
- **AF** — audio makeup gain (0…+40 dB), applied **before** Vol. Use it to
  set a comfortable working level for your headphones/speakers once, then
  ride **Vol** on top of it for moment-to-moment changes. The value shows
  in dB beside the slider.
- **Bal** — stereo balance: pans the audio left/right. Centre = both
  channels equal; the slider snaps to dead-centre near the middle so it's
  easy to recentre.
- **Auto · Out** are still greyed in their final positions — each lights up
  as its control is wired in. Choosing **where** the audio goes (HL2
  headphone jack vs. a PC sound device) lives in **[Settings → Audio](#settings--audio)**.

**Row 2 — DSP toggles + AGC**
- A row of effect buttons **NB · BIN · NR · ANF · LMS · SQ · APF · NF**.
  **NR** (Noise Reduction), **ANF** (Auto Notch — nulls carriers/
  heterodynes automatically), **LMS** (Line Enhancer — lifts CW/tones out
  of the noise), **NF** (manual Notch Filter), **SQ** (all-mode squelch),
  **NB** (Noise Blanker), **APF** (Audio Peak Filter), and **BIN**
  (Binaural) are all live. NR and LMS are *complementary* — NR subtracts
  broadband noise, LMS predicts the periodic signal, so both can run
  together on weak CW.
- **BIN — binaural pseudo-stereo:** widens the soundstage on **headphones**
  by sending the signal to one ear and a 90°-phase-shifted copy to the
  other, so weak CW/SSB seems to lift out of the noise and voices gain a
  sense of space. A **depth** slider appears in the bottom row while BIN
  is on (0 = mono → 100 = full); loudness stays constant as you sweep it.
  (Headphones only — on speakers the effect collapses.)
- **APF — CW peaking:** a narrow peak parked on your CW pitch that lifts
  a CW tone out of the surrounding hiss. It engages only in **CWU/CWL**
  (no effect in SSB/AM/FM) and re-centres automatically when you change
  the CW pitch. A **gain** slider appears in the bottom row while APF is
  on — pick how hard it lifts (3–18 dB in 3 dB steps; default 12). Stacks
  well with LMS for weak-signal CW.
- **NB — noise blanker:** suppresses *impulse* noise (ignition, power-line
  arcing, lightning crashes) by gating it out of the raw IQ before demod.
  A **strength** slider appears in the bottom row while NB is on — higher
  = more aggressive blanking; back it off if it starts chewing CW/SSB
  transients. (For a steady carrier-type interferer use a manual notch or
  ANF instead — NB is for clicks/pops, not tones.)
- **SQ — all-mode squelch:** mutes the audio between transmissions and
  unmutes when a signal opens it. It routes automatically by mode — a
  voice-presence detector (SSQL) on SSB/CW/DIG, the FM noise squelch on
  FM, and the carrier squelch on AM/SAM/DSB. A **threshold** slider
  appears in the bottom row while SQ is on: higher = tighter (only
  stronger signals open it); the usual sweet spot is ~10–30.
- **NF — manual notches** (right-click the panadapter to use):
  - **Right-click empty spectrum** drops a notch at that frequency
    (200 Hz wide) and turns NF on.
  - **Drag** a red band to slide it onto the offending carrier.
  - **Mouse-wheel** over a band: down = wider, up = narrower.
  - **Right-click** a band to remove it.

  Each notch **visibly cuts the spectrum trace and the waterfall** in
  that region — the trace drops to the noise floor and the waterfall
  shows a dark stripe across the notch width, so you can see exactly
  what's being removed instead of guessing. The notches are deep and
  sharp by design — Lyra uses WDSP's NBP filter in a deep
  rectangular-window, 2048-tap configuration, so a carrier drops into the
  noise rather than just dipping. The **NF** button toggles all notches
  at once without losing their positions; the count shows beside the
  button. ANF (the auto-notch) likewise runs deep adaptive settings — a
  marked step up from the softer notching in the Python build.
- **AGC** — click anywhere on the AGC cell (it highlights on hover) to
  cycle **Off → Fast → Med → Slow**. Med is the everyday default; Fast for
  fast-changing signals, Slow for steady ones, Off for fixed gain (digital
  modes / measurement). The three modes are genuinely distinct — Fast
  recovers in ~50 ms with no hang, Med in ~250 ms, Slow holds ~1 s then
  decays over ~500 ms (industry-standard time constants). The cell shows the AGC
  **threshold** and the **live gain action** (dB) beside the mode.

**Row 3 — Noise Reduction character**
- **NR Mode (1–4)** — picks the WDSP denoiser's gain function:
  **1** Wiener + speech-presence, **2** plain Wiener (edgier), **3**
  MMSE-LSA (the smoothest, default), **4** trained-adaptive (most
  aggressive). Turn **NR** on in Row 2, then sweep modes to find the best
  sound for the band.
- **AEPF** — anti-musical-noise smoother. On (default) engages *both* of
  WDSP's cleanup stages — artifact elimination **and** the post-filter
  that stock WDSP leaves off by default — so the "musical twinkle" is
  knocked down hard while MMSE-LSA (Mode 3) keeps the voice natural
  rather than robotic.
  Turn it off to hear raw EMNR on already-quiet bands. If you still want
  the most natural voice, stay on **Mode 3** (Mode 4 trades smoothness for
  aggression and brings musical noise back).
- **NPE** — how the denoiser tracks the noise floor: **OSMS** (smooth,
  best for steady atmospheric hiss) or **MCRA** (faster-tracking, better
  for changing/intermittent QRM).
- **LMS strength** — appears in this row only while **LMS** is on: 0 is
  subtle, 50 is the WDSP-class default, 100 is full prediction (more taps,
  harder pull). Most useful digging weak CW out of band hiss.

Surfacing NR Mode + AEPF + NPE as separate knobs is one of Lyra's
differentiators — most SDR apps hide them. All three persist across
restarts.

### Captured noise profile (NR-C) — Lyra's signature noise reduction

This is the one that's genuinely different. Instead of a generic denoiser,
NR-C learns **your** band's actual noise spectrum and subtracts it — in the
IQ domain, *before* WDSP's chain — so it's mode-independent and doesn't fight
the AGC. Used well it drops the noise floor several dB and the signal pretty
much sits where it was.

**The workflow (left to right on Row 3):**

1. Tune to a **quiet, signal-free** spot on the band you're working — NR-C
   subtracts whatever it captures, so don't capture on top of a signal.
2. Pick a **capture length** (3 / 5 / 10 s) next to the **📷 Cap** button.
3. Press **📷 Cap**. The button shows a percentage while it averages the
   noise; press it again to cancel.
4. Press **Save**, give the profile a name (e.g. `40m-night`, `20m-ESSB`) in
   the box that pops up. It's now in the picker.
5. Flip **NR-C** on. The captured noise is subtracted from RX. The panadapter
   shows the cleaned spectrum too, so you can watch the floor drop.
6. Fine-tune with the **⚙** button (appears next to NR-C while it's on):
   - **Strength** (1–5×) — how hard to subtract. Higher = more cut.
   - **Floor** (−3 to −30 dB) — the deepest any bin is allowed to drop.
     More negative = more aggressive.
   - **Smoothing** (0–95%) — steadies the gain so it doesn't "twinkle."
     Raise it if deeper Strength/Floor starts to sound watery.

**FFT size** (2048 / 4096 / 8192) sets the resolution — 8192 resolves wide
ESSB noise finest (a touch more latency), 2048 is lowest latency. Changing it
clears the current profile (a profile is tied to the size it was captured at).

**Profiles are files you own.** Each saved profile is a small `.lnp` file in
`%LOCALAPPDATA%\N8SDR\Lyra-cpp\profiles\` — browsable, backup-able, and
shareable between rigs (drop a friend's `.lnp` in there and it shows up in
your picker). Profiles are tagged with their capture rate + FFT size and
won't load against a mismatched rate (you'll get a recapture hint).

**Manage them in Settings → Noise:** the full list with rate/size/date, plus
**Rename**, multi-select **Delete**, and **Open folder**.

Notes: NR-C adds about one FFT window of RX latency (≈21 ms at 4096) *only
while it's on*; off, the path is unchanged. It's independent of the WDSP
**NR** denoiser — you can run either, both, or neither.

---

## Display panel

The handful of spectrum/waterfall controls you reach for most often, one
click away instead of buried in Settings. These mirror **Settings →
Visuals** — change them in either place and both stay in sync. The panel
is laid out in old Lyra's three-row arrangement:

- **Zoom** — magnify the panadapter around center: a preset combo
  (**1× / 2× / 4× / 8× / 16×**) plus a fine slider and a live "N.Nx"
  readout. Zooming in narrows the displayed span so you can pull a single
  signal apart.
- **Panafall** — the step the mouse wheel tunes by when you scroll over
  the panadapter/waterfall (**1 Hz … 100 kHz**). This is your coarse
  band-skim step, separate from the fine VFO step on the Tuning panel.
- **Exact / 100 Hz** — how a panadapter **click/drag/wheel** tune lands:
  - **Exact** — go to where you clicked, snapped to the **Panafall step**
    above. With Panafall = 1 Hz that's truly exact; set it to 10/50 Hz
    (or higher) and clicks snap to that grid. The RX1 readout shows the
    full resolved frequency.
  - **100 Hz** — quantize every click/drag/wheel tune to the nearest
    100 Hz, regardless of the Panafall step.

  (At low zoom the panadapter is pixel-limited — each pixel can be tens
  of Hz wide — so to actually land on a 1 Hz boundary, zoom in or use the
  Tuning panel's fine step.)
- **Spec** — spectrum frame rate (how many times per second the panadapter
  redraws).
- **WF** — waterfall scroll rate (history rows per second).
- **Peak Hold** — how the [peak markers](#peak-markers) behave:
  - **Off** — no peak markers.
  - **Live** — markers ride the current spectrum.
  - **1 / 2 / 5 / 10 / 30 s** — hold each peak for that long, then fade.
  - **Hold** — peaks never fade until you press **Clear**.
- **Decay** — how fast held peaks fade once their hold time is up:
  **Fast / Med / Slow** (only matters in the timed modes).
- **Clear** — wipe the held peaks and start fresh (handy in **Hold** mode).

The look-and-feel settings — palettes, smoothing, glow, gridline, peak
style/color, noise floor — live in **Settings → Visuals**.

---

## Meter panel

A GPU-drawn **RX signal-strength meter** with two looks — pick whichever
you like with the small **Arc | Bar** toggle in the panel's top-right
corner (your choice is remembered):

- **Horizon Arc** (default) — a sweep dial against a fixed, calibrated
  S-scale. The fill climbs through colour zones (cyan → green → amber →
  red over S9) with a glowing "comet head" at the current level, a thin
  **peak-hold** pip that holds then fades, and a faint **history trail**
  in the dial's belly showing the last few seconds (handy for watching
  QSB / a station rising). Big **S-unit** readout in the centre with the
  **dBm** and **SNR** lines beneath.
- **Plasma Bar** — a continuous level bar through the same colour zones,
  with a bright leading edge, peak-hold cap, a glass reflection beneath,
  and the readout (S-units / dBm / SNR) along the top.

Both styles also carry an optional **max-peak marker** — a second, **red**
"high-water mark" that latches the highest level reached and falls back
*gently*, so a fleeting DX or QSB crest stays readable long after the fast
yellow peak pip has dropped. The fast pip tracks recent activity; the red
max marker remembers the loudest moment.

In **[Meter panel](#meter-panel)** you can tune both timings:
**peak-hold** (how long the fast pip hangs before falling, default 800 ms)
and the **max-peak marker** (on/off + its own hold time, default 3 s)
before its gentle decay.

What the meter shows:

- **S-units** — standard scale (S1…S9, then +20/+40/+60 dB over S9). The
  scale follows the band: S9 = −73 dBm below 30 MHz, −93 dBm above, the
  same convention major HF SDR software uses.
- **dBm** — the calibrated signal level.
- **SNR** — signal *above the noise floor* in dB (shows `—` when there's
  nothing above the floor). The faint band/marker on the meter is the
  current noise floor, so you can see at a glance how far a signal stands
  out — i.e. how readable it is, not just how strong.

The meter reads the in-passband signal level from the DSP engine (the
same source professional SDR software uses) and only moves while you're
receiving (press **▶ Start** first; it rests at S0 when idle).

> **Calibration:** the meter taps WDSP's in-passband `RXA_S_PK` level —
> the standard in-passband signal-strength point — and maps it through
> the HF/VHF S-unit scale (S9 = −73 dBm below 30 MHz, −93 dBm above). To make the
> *absolute* dBm/S reading exact, trim it once in
> **[Meter panel](#meter-panel)**: tune to a known reference
> (WWV, or a signal generator at a known level) and adjust the **S-meter
> calibration** offset until it matches. With this tap the trim is only a
> few dB. The meter compensates for the **LNA gain** automatically, so the
> reading stays put as you adjust LNA — you can calibrate at any setting.
> The relative movement, peak-hold, SNR, and
> noise-floor behaviour are all live regardless.
>
> **Transmit meters** are fully wired. PWR (forward power, watts),
> SWR (antenna match), ID (PA bias current), VDD (PA supply), Temp
> (HL2 board) plus the three WDSP TXA chain meters — **ALC** (gain
> reduction in dB), **MIC** (mic peak in dBFS), and **COMP** (leveler
> gain reduction in dB) — all read live from the running TX chain
> the moment MOX engages, and show "—" on RX. Set the active TX
> source (and an optional secondary readout line) in
> **[Meter panel](#meter-panel)**. The panel auto-swaps from your RX
> source to your TX source on every MOX edge and back, so you can
> watch what you need without manual switching. Tip for dialing in
> the chain: pick **MIC** as primary + **ALC** as secondary, key
> the mic, and adjust Mic Gain until MIC peaks land near −6 dBFS
> with ALC reduction staying under 3 dB.

---

## TX panel

The operating-time TX controls — a single strip across one of the
docks. Layout (left → right):

```
[TX]    Drive ──●── 25 %   Mic ──●── +10 dB              [ TUN ]  [ MOX ]
```

- **TX Drive** (slider, 0–100 %, default 0) — how hard the HL2
  drives its on-board PA. **0 % = no carrier even with PA enabled
  and MOX keyed.** Start LOW (5–10 %) on a dummy load; raise gradually
  while watching the watt-meter. **Mouse-wheel** adjusts (Shift + wheel
  = 5 % steps). Live readback to the right shows the current %.
- **Mic Gain** (slider, −90 dB to +40 dB, default 0 dB) — input gain
  into the WDSP TXA modulator (PanelGain1 — the only operator-tunable
  software gain stage in the TX chain, before the bandpass / leveler /
  ALC / everything downstream). **0 dB is WDSP unity** = no boost vs
  the raw mic signal. Typical SSB voice runs **+10 to +20 dB**; ESSB
  with headroom **+25 to +35 dB**. The range matches the reference's
  Default TX profile (−90 deep-attenuate floor, +40 ceiling), so the
  full operating travel is preserved. **Mouse-wheel** adjusts (Shift +
  wheel = 5 dB steps). Bidirectionally bound with the spin-box in
  **Settings → TX → Mic + ALC** — moving either updates both, so you
  can adjust on the fly here or type a precise value in Settings.
  Watch the **ALC meter** as you raise — back off when ALC engages
  hard (more than ~3 dB of gain reduction means you're driving past
  the limiter ceiling and creating splatter risk).
- **+20 chip** (next to the Mic Gain readout) — engages the HL2 codec's
  hardware **+20 dB mic preamp** (analog PGA in the codec, ahead of the
  DSP chain). Use this when your hand-mic or headset mic is genuinely
  weak — Mic Gain near the top of its travel and still not hitting the
  WDSP TXA chain at a workable level. The chip lights orange when on,
  greys when off; single click to toggle. **Persists across launches.**
  *Only affects the codec mic source* — PC mic and TCI audio bypass the
  codec entirely, so this chip has no effect on them. Composes with the
  Mic Gain slider: HW +20 dB → SW continuous trim above that.
- **TUN button** — single-click gesture: arms a 1 kHz **tune carrier**
  AND keys MOX in one click. Click again to release MOX; the carrier
  auto-disarms on the next MOX-off edge for any reason (your click,
  the safety timeout, the FSM unwinding). When armed the button
  border glows amber. Carrier power scales with TX Drive %.
- **MOX button** — the keying button. Funnels through Lyra's TR-
  sequenced FSM (the same one your **space-bar momentary** uses when
  no text widget has focus). Goes **bold red** the instant the wire-
  MOX bit settles after the TR-delay window. A short tap shorter
  than the TR-delay (≈ 65 ms by default) still flashes the button
  **orange** for ~220 ms so you get visual feedback that your press
  registered — even if the FSM cancelled mid-keydown.

What the button colours mean, at a glance:

| MOX button colour | Meaning |
|---|---|
| Gray | Idle (RX) |
| **Orange flash** (~220 ms) | You just pressed; wire-MOX hasn't settled yet |
| **Bold red** | Wire-MOX bit is high — radio is keyed, RF flowing if PA + Drive permit |

TUN is a strict superset of MOX for tune-up: it does what MOX does
PLUS injects the 1 kHz tune carrier. Use **MOX** when you want the
relay to switch (CW key-down testing, mic feed-through, or with
future SSB modulator); use **TUN** when you want a steady carrier
on your dial frequency to set drive level / read an SWR / cal an
external watt-meter / bias-tune an external amp.

> **Both buttons honour the safety knobs.** They route through the
> same FSM that respects the **TR-sequencing delays** (see
> [Settings → TX](#settings--tx-mic--alc-tr-sequencing--cos-fade)) and the
> **PA Enable** safety gate (see
> [Settings → Hardware → Transmit](#transmit-pa-enable--safety-timeout--hardware-ptt)).
> Carrier amplitude follows the **cos² fade envelope** on every
> keydown and keyup — no hard step, no click.

> **PA Enable is separate from MOX.** With PA disabled, keying MOX
> or TUN gives you the relay click + the wire-MOX state but **zero
> RF** (the gateware PA bias never engages). This is the safe state
> for first-time bench work — leave PA off until you're confident
> in your Drive setting, fade defaults, and amp configuration.

> **Foot switch / hand-mic PTT** — the HL2's hardware PTT-input pin
> is supported via an opt-in checkbox in
> [Settings → Hardware → Transmit](#transmit-pa-enable--safety-timeout--hardware-ptt).
> Default OFF for safety; tick it after a quick bench check on your
> specific HL2 unit.

---

## Solar / Propagation panel

A compact strip showing current **HF propagation** conditions from
[HamQSL](https://www.hamqsl.com/) (N0NBH's solar data):

- **SFI** (Solar Flux Index), **A** (A-index), **K** (K-index) — each
  colour-coded green / amber / red so you can read conditions at a glance
  (higher SFI is better; lower A and K are better). Hover the **SFI** box
  for the full picture — sunspot number, X-ray level, solar wind, and when
  HamQSL last updated.
- **A 10-band heat-map** — `160` through `6` m, each tinted **green
  (Good)**, **amber (Fair)**, **red (Poor)**, or **gray (no prediction)**.
  Hover a band for its rating.

The ratings are shown for **your current day or night** — Lyra works out
whether it's daylight at your location and picks HamQSL's day or night
figures accordingly. For that to be right, set your **grid square** (or
manual lat/lon) in
[Settings → Hardware → Operator / Station](#operator--station); without a
location it assumes daytime. HamQSL doesn't predict **160 m** or **6 m**,
so those two always show gray.

The data is fetched when Lyra starts and refreshed about every 15 minutes
(the day/night split updates on its own as the sun rises and sets). It's
read-only and needs no account or key — just an internet connection. If
the feed can't be reached, the last-known figures stay on screen and the
SFI tooltip notes the error.

**NCDXF beacon auto-follow.** The **Follow** button on the right of the
panel lets you chase a single NCDXF beacon station through the rotation.
Pick a station (e.g. **4U1UN**) from the dropdown and Lyra automatically
tunes (in CW) to whichever of the five beacon bands that station is
transmitting on *right now* — 20 → 17 → 15 → 12 → 10 m — re-checking every
second and hopping with it through the 3-minute cycle. It's a quick way to
hear how one station is propagating across the bands. Set it to **Off** to
stop; your choice is remembered between sessions. (While following, Lyra
re-tunes on its own, so manual tuning won't "stick" until you turn Follow
off.)

If you don't want the panel, close it like any other (the **View** menu
re-opens it). It's a dock — drag, float, or tab it wherever you like.

---

## Weather alerts

Lyra can watch for nearby **lightning**, high **wind**, and **severe-storm
warnings**, and show flashing badges in the header so you know to think
about your antennas. Turn it on and configure it in
**[Settings → Weather](#settings--weather)**.

> ⚠ **Advisory only.** These alerts are a convenience, **not** a substitute
> for official warnings or your own judgment. You must accept a short
> disclaimer before enabling them.

**What you'll see** — badges appear toward the right of the header when an
alert is active, and **flash** on the most serious tier:

- **⚡ Lightning** — shows the closest strike distance + compass bearing.
  Yellow ≈ far, orange ≈ mid, **red = close** (flashing).
- **💨 Wind** — shows gust (or sustained) speed. The badge appears only
  once wind **reaches the thresholds you set** (Settings → Weather):
  **orange** when sustained or gust crosses your value, **red = extreme**
  (your value + 15, flashing). In calm air below your thresholds there's
  no wind badge at all.
- **⚠ Severe** — an NWS thunderstorm/severe warning is active (red,
  flashing). Hover for the headline.

**Where the data comes from** depends on which sources you enable
(Settings → Weather): **Blitzortung** (global lightning, free),
**NWS / weather.gov** (wind + severe + live station wind, free, US), and
your own **Ambient Weather** or **Ecowitt** station (needs that account's
keys). Lyra needs your **location** to know what's "nearby" — set your
grid square in [Settings → Hardware → Operator / Station](#operator--station).

**Notifications** — optionally a desktop pop-up and an audible chime when
an alert first crosses a tier (with a cool-down so it doesn't nag). Lyra's
own weather pop-ups are titled **"Lyra — …"** so you can tell them apart
from other notifiers (your Ambient app, a Windows weather widget, etc.).
The wind pop-up only fires when your **local** readings cross your
thresholds; an area **NWS High/Extreme Wind Warning** pops a toast only if
you opt in (**"Pop NWS High/Extreme Wind Warnings"**, off by default — the
badge still reflects it either way). Toggle the desktop/chime options in
Settings → Weather, and use **Send test alert** there to see the badges
light up.

---

## Updates

Lyra checks GitHub for new releases so you don't miss one.

- A quiet check runs a few seconds after launch. If a newer version
  exists, you get a one-time pop-up (**Open release page / Remind me later
  / Skip this version**) and an **⬆** indicator stays in the toolbar.
- Check whenever you like: **Help → Check for Updates…**.
- Downloads are on the project's GitHub Releases page; grab the new
  installer and run it over the top.

---

## Backing up & sharing your settings

Lyra can save your **entire configuration** — panel layout, saved default
layout, and all your preferences (visuals, mode/bandwidths, weather, etc.)
— to a single portable file.

- **File → Export settings…** — writes a `.lyra` profile. Keep it as a
  backup, or hand it to another operator so they get your exact setup.
- **File → Import settings…** — loads a `.lyra` profile; the layout applies
  immediately and Lyra offers to restart to apply the rest.

This is the easy way to recover if a layout gets scrambled, or to set up a
second PC the same way. (One thing that intentionally **doesn't** travel in
a profile: the graphics backend, since that's a per-machine hardware
choice.)

---

## Operating with an external amplifier (hot-switch protection)

If you're driving an external solid-state HF linear amplifier from
the HL2, **read this section first.** Hot-switching — applying RF
into a T/R relay that hasn't finished switching — can destroy a SS
linear's PA. Lyra has two layers of protection against this; they're
load-bearing, not cosmetic, and the defaults are chosen for typical
1 kW SS HF linears.

### How the two layers compose

| Layer | Knob | What it does |
|---|---|---|
| 1. **Scheduling** | RF Delay (default 50 ms) | After the wire-MOX bit goes hot, Lyra waits 50 ms before allowing any RF onto the antenna. Gives the external amp's T/R relay time to fully close. |
| 2. **Amplitude shape** | Fade-In Duration (default 50 ms) | Even WITHIN the RF Delay window, the host-side I/Q amplitude rises smoothly from zero via a cos² envelope. Redundant protection against any residual MOX-bit / relay timing skew. |

Both knobs live on **[Settings → TX](#settings--tx-mic--alc-tr-sequencing--cos-fade)**.

### Workflow before the first on-air keying

The discipline that's saved more amps than anyone wants to admit:

1. **Dummy load on the antenna port.** Until you've confirmed the
   full chain works the way you expect, the HL2 talks to a 50 Ω
   resistor, not your antenna. A dummy load tolerates hot-switching;
   a relay-bypassed antenna won't.
2. **External amp OFF / bypassed.** First-RF goes to the dummy load
   straight from the HL2's on-board PA (a few watts). Confirm
   wattmeter behaviour, panadapter trace, and that TUN / MOX both
   ramp cleanly.
3. **PA Enable OFF in [Settings → Hardware → Transmit](#transmit-pa-enable--safety-timeout--hardware-ptt).**
   Run through your keying ergonomics first — TUN button, MOX
   button, space-bar, drive-slider movement — with **no actual RF**.
   You should hear the HL2 relays click and see the wire-MOX state
   flip; the wattmeter should read zero.
4. **PA Enable ON, Drive low (5–10 %).** Key TUN once. Watch the
   wattmeter trace ramp up smoothly over ~50 ms, hold steady, then
   ramp down smoothly. **No clicks, no spikes** at either edge.
5. **Verify your [Settings → TX](#settings--tx-mic--alc-tr-sequencing--cos-fade) values match your amp.**
   The defaults are bench-safe for typical 1 kW SS linears.
   If your amp's documentation specifies a T/R relay settle time
   greater than 30 ms, **leave RF Delay and Fade-In Duration at
   their defaults or HIGHER**. Only consider lowering them if your
   amp's spec explicitly says it can handle a faster onset (e.g.
   QSK-rated amps with vacuum relays).
6. **External amp ON / inline.** Re-do the TUN test with the amp in
   line, still on the dummy load. Confirm the amp keys and reads
   the right output for your drive level. Watch for any sign of
   relay arcing or other distress.
7. **Antenna connected.** Final step. By the time you reach this,
   the chain has been verified end-to-end.

### What to tune (and what NOT to tune)

| If you're seeing… | Tune this | NOT this |
|---|---|---|
| Amp T/R relay arcing on key-up | **Space MOX Delay** (and Fade-Out Duration must stay ≤ Space MOX Delay) | Don't touch RF Delay — keyup arcing is a different problem from keydown hot-switching |
| Amp shows symptoms of hot-switching on key-DOWN | **RF Delay** (raise it) — match your amp's T/R relay settle spec | Don't reduce Fade-In Duration as a "fix" — it's the second protective layer |
| Audible click in your monitor at keydown | **Fade-In Duration** — should be ≥ ~30 ms for clean SSB-band sound | Don't extend RF Delay just to remove a click — that's the amplitude envelope's job |
| Audible click in your monitor at keyup | **Fade-Out Duration** (raise it up to Space MOX Delay) | Don't extend Space MOX Delay just for a click — that lengthens the re-key window too |

### When Lyra is killed mid-transmit (process crash, power glitch)

The HL2 gateware has its own watchdog: no EP2 keepalive frames for
N seconds (gateware-specific, typically ~1 sec) → gateware drops PA
bias. This is your last-resort safety if Lyra dies while keyed.

**But it's slow.** A second of stuck carrier into an antenna is
enough to embarrass yourself or worse. The discipline above —
dummy-load → external-amp-off → progressively widen — exists
because the gateware watchdog isn't fast enough to substitute for
operator-side discipline.

---

## Settings → Hardware

The **Hardware** tab (Settings — **Ctrl+,**) holds your station identity
and connects to the radio + optional station hardware.

### Operator / Station

Your identity and location, used by the weather alerts (and, in time, the
solar panel and logging):

- **Callsign** — your call (stored uppercase).
- **Grid square** — your Maidenhead locator (e.g. `EN82`, `EN82dk`). When
  it's valid, Lyra computes your latitude/longitude from it — shown live
  in **Computed lat/lon**.
- **Manual lat / lon (°)** — a fallback if you'd rather not use a grid (or
  don't have one). Used only when the grid is blank/invalid.

Grid wins when it's valid; otherwise the manual lat/lon is used. Either way
that's the location the weather sources use for "nearby."

### Band plan (Region)

**Region** — your IARU region (**US / IARU Region 2**, **IARU Region 1**,
**IARU Region 3**, or **None**). This selects which amateur band-plan the
panadapter overlay draws. Set it to **None** to turn the whole overlay off.

The overlay paints a thin strip across the **top of the panadapter**:

- **Sub-band segments** — a coloured bar showing the mode allocations in
  view: **CW** (blue), **digital** (magenta), **SSB** (green), **FM**
  (orange). The label (CW / DIG / SSB / FM) shows when the segment is wide
  enough on screen.
- **Digital landmarks** — small **▼** markers at the common calling
  frequencies (**FT8 / FT4 / WSPR / PSK**), gold. **Click a marker to tune
  straight to it** (and Lyra switches to the suggested mode).
- **NCDXF beacon markers** — the five International Beacon Project
  frequencies, in cyan. **Hover one** and the tooltip shows the station
  that's transmitting *right now* (the 18 worldwide beacons rotate every
  10 seconds) — e.g. "Now: 4U1UN — United Nations, NY". Click to QSY
  there in CW.
- **Band-edge warning lines** — red dashed lines at each band's edges, so
  you can see at a glance when you're tuning toward the edge of an
  allocation.

Each of those four layers has its own checkbox here so you can show only
what you want. They're all on by default. **Segment colors** — the four
swatch buttons (CW / DIG / SSB / FM) let you recolor each mode category;
click a swatch to pick a color, or choose the original color to clear the
override.

As you tune, Lyra shows a brief message at the **bottom of the window**
when you cross a band edge — "In band: 40m (US)" when you're inside an
allocation, or "⚠ Out of band — X.XXX MHz is outside the US amateur
allocations" when you're not. (Gated on the band-edge layer above.)

> The band plan is **advisory only** — the HL2 is unlocked and Lyra will
> tune anywhere it can receive. Sub-band boundaries vary by license class
> and country and change over time; verify against your own regulator
> before transmitting near an edge. The strip is a navigation aid, not a
> legal reference.

### Shortwave broadcasters (EiBi)

Lyra can overlay the **EiBi shortwave-broadcast schedule** on the
panadapter so you can see *who's on the air* across the SW broadcast and
utility ranges. Each station shows as a small tick + name:

- **Cyan** = broadcasting **right now** (the current UTC time falls inside
  the station's scheduled window for today).
- **Grey** = scheduled on this frequency but **off-air** at the moment
  (only shown if you turn off "Hide off-air").
- **Hover** a station for its frequency, language, target area, and on-air
  status; **click it to tune there in AM**.

Turn it on in **Settings → Bands → SW Database**:

- **Enable EiBi shortwave overlay** — master on/off (off by default).
- **Update database now** — downloads the current season's schedule
  (~3 MB) from eibispace.de. **You must do this once** before anything
  appears — the data isn't bundled (EiBi's licence is free but
  no-redistribution). Re-run it when the season changes (roughly April and
  October).
- **Load CSV file…** — the fallback if the download fails. eibispace.de's
  security certificate expires periodically, which can block the in-app
  download; when that happens, download the `sked-<season>.csv` from the
  EiBi website in your browser, then point Lyra at it with this button.
  (Lyra copies it into its own folder, so you can delete the download
  afterward.)
- **Hide off-air** — show only stations broadcasting now (on by default).
- **Show in all bands** — by default the overlay is hidden inside the
  amateur bands (so it doesn't clutter ham operating); tick this to show it
  everywhere.
- **Minimum transmitter power** — hide the weak stations; show only
  ≥50/100/250 kW transmitters to cut clutter.

> EiBi schedules are a planning aid — actual broadcasts vary with
> propagation, season, and last-minute changes. A cyan station may be
> inaudible (wrong band conditions) and you may hear stations the database
> doesn't list. Data © the EiBi project (eibispace.de).

### Radio

Find and connect to your HL2 / HL2+. **Discover** scans the LAN, **Open**
connects to the selected radio, **Close** disconnects, and the status line
shows what you're connected to. Lyra remembers the last radio and shows it
here on launch. (The toolbar **▶ Start / ■ Stop** does the same thing.)

### Transmit (PA enable + safety timeout + hardware PTT)

The deliberate-arm safety gates for getting on the air. Both are
intentionally OUT of the operating-time TX panel — they're set-and-
forget config you arm once when you're ready to transmit, not knobs
you touch between QSOs.

- **Enable PA** (checkbox, default **OFF**) — toggles the HL2 gateware's
  PA bias. With this OFF, keying MOX or TUN gives you the relay click
  and the wire-MOX state but **zero RF** (the gateware never biases
  the PA). With this ON, MOX + non-zero Drive % produces real RF. **The
  safest first-time-on-the-air bench gate**: leave OFF for relay /
  panadapter / wattmeter wiring checks; turn ON only after you're
  confident in your Drive setting, fade defaults, and amp configuration.
  Lyra defensively clears PA on every stream close and open — restart
  Lyra and PA is OFF again, no matter where you left it.
- **TX safety timeout** (spin box, default **600 s = 10 min**, range
  60 – 1200 s) — if the radio stays continuously keyed past this
  duration, the FSM auto-releases MOX through the normal keyup
  sequence (no shortcut on the wire — full TR-delay unwind, ATT-on-TX
  restore, the works). A pop-up toast notes the auto-release.
- **Bypass safety timeout** (checkbox, default OFF) — for long-form
  AM ragchews or slow CW beacons where the 10-minute default is
  genuinely too short. Bypass disables the timer entirely. **Use
  with intent** — a stuck PTT or a software fault now has no host-
  side rescue (the HL2 gateware watchdog is still in play, but
  treats EP2 keepalive as the only liveness signal).
- **Enable hardware PTT input (foot switch / hand mic)** (checkbox,
  **default OFF**) — when ticked, the HL2's hardware PTT-input pin
  is read on every status frame and an edge (press / release) is
  forwarded to MOX, so a foot switch, hand-mic PTT, or external
  mic-button keys Lyra just like the on-screen MOX button.
  **⚠ Bench-verify before enabling.** Some HL2 units (notably most
  observed HL2+ / AK4951 units) carry a non-zero PTT-input level at
  RX rest, so an always-on forwarder would mis-read it as a press
  and produce a phantom-TX surge the moment you launch Lyra.
  Default OFF lets you control when the read goes live: confirm
  with a scope or with the foot-switch unplugged that the pin
  reads stable 0 at rest, then tick this box. If your foot-switch
  press doesn't key after enabling, the issue is upstream of Lyra
  (HL2 connector / switch / cable / wiring) — Lyra can't paper
  over a dirty level at rest.

> **Why these are in Hardware, not in TX:** Settings → TX is for
> *timing* knobs you might tune to match an external amp's switching
> spec. Settings → Hardware → Transmit is the *arm/disarm* surface
> for "ready to actually radiate" — different operational gesture,
> different cognitive bucket. Both honour each other: PA off ⇒ no
> RF regardless of timing, and the timing knobs are inert when PA
> is off.

### Filter board (N2ADR / compatible)

If you have an external band-pass filter board on the HL2's
open-collector (OC) outputs, tick **Enable external filter board** and
Lyra switches the OC pins per band so the board follows your tuning —
front-end protection against strong out-of-band signals (a nearby AM
broadcaster, say). The live **OC outputs** readout shows which J16 pins
are currently driven. Leave it off if you don't have a board — harmless
either way (the pins just drive nothing).

### USB-BCD (linear-amp band switching)

Drives a Yaesu-style **BCD band code** out an FTDI cable so an external
linear amp's auto-bandswitch follows Lyra's band. Tick **Enable
USB-BCD**, choose your **BCD cable**, and the **BCD output** readout shows
the current code. **60 m** has no standard BCD code — tick **60 m uses the
40 m filter** to send the 40 m code instead of bypassing.

> ⚠ **Verify before you key.** Confirm the wiring and do a low-power test
> on each band before transmitting at full power — the wrong code can
> route TX through the wrong filter.

(If the FTDI driver, `ftd2xx.dll`, isn't installed, this section says so
instead — install the FTDI D2XX driver to use USB-BCD.)

### Diagnostics (debug log)

Lyra runs without a console window, so if something misbehaves there's no
black terminal to copy errors from. Instead, Lyra keeps a **diagnostic
log** you can read, copy, and send to us.

- **Help → Show Log…** opens the log viewer — a scrolling view of this
  session's diagnostic messages with **Copy all**, **Save…**, and **Open
  log folder** buttons. It updates live while open.
- **Enable verbose debug logging** (here in Diagnostics, also a checkbox
  in the log viewer) turns on detailed capture. Leave it **off** for
  normal use — the log then keeps only warnings and errors and stays
  small. Turn it **on** when you're chasing a problem: enable it,
  reproduce the issue, then Copy or Save the log.
- A copy of the log is always written to a file on disk
  (`…/N8SDR/Lyra-cpp/logs/lyra-log.txt`, reachable via **Open log
  folder**), refreshed each launch — so even a crash leaves a trace.

When you report a bug, attach the log (or paste it) along with what you
were doing and what you expected.

---

## Settings → Audio

Where Lyra sends received audio:

- **HL2 audio jack (AK4951)** — the default on HL2+ hardware. Audio is
  decoded on the PC, sent back to the radio, and played from the HL2's own
  headphone jack. Single-crystal, lowest latency, bypasses the PC sound
  card.
- **PC sound device** — pick any output Windows offers (sound card,
  headphones, a virtual audio cable to WSJT-X / FLDigi, etc.).

Pick your output device here; everyday **Mute** and **Vol** stay on the
[Audio panel](#audio-panel).

**Filter Low edge (RX + TX)** — single shared low cutoff for the
SSB / DIG audio bandpass on both receive and transmit. Range
**0–500 Hz**, default **100 Hz**.

- **100–200 Hz** suppresses 50/60 Hz mains coupling on the mic path
  and reduces low-end rumble on receive — safe choice for narrow
  comms.
- **50–70 Hz** preserves chest-resonance / low-end body for
  ESSB-style wide audio — verify your station isn't picking up
  mains hum at very low cuts.
- **0 Hz** opens the filter all the way down to the carrier (no
  low cut) — some operators want this for ultra-wide ESSB into a
  clean RF environment.

Applies to **SSB and DIG modes only** (USB / LSB / DIGU / DIGL —
the asymmetric passband case). CW is pitch-centred and AM / DSB /
FM are symmetric around DC; those modes ignore this setting.

You can also **drag the low edge of the passband rectangle** on the
panadapter (left edge on USB / right edge on LSB) to set this value
in real time. Dragging below 0 just pins at 0.

> **Interim setting** until the TX Profile Manager ships — once
> profiles arrive, each named profile will carry its own RX + TX
> (low, high) pair that overrides this default on profile load.

---

## Settings → TX (Mic + ALC, TR sequencing + cos² fade)

> **⚠ Read [Operating with an external amplifier](#operating-with-an-external-amplifier-hot-switch-protection) FIRST**
> if you're driving a solid-state HF linear from the HL2. The TR-
> sequencing + fade defaults on this tab are **hot-switch-safe** for
> typical 1 kW SS linears; reducing them without knowing your amp's
> T/R relay settle spec can damage the PA. Tooltips on the most
> dangerous knobs carry the warning; don't dismiss it.

Three sub-sections. The **Mic + ALC** group controls input/output
gain on the WDSP TXA chain — operator-tunable, live-apply. The
**TR Sequencing** and **Amplitude Envelope** groups control the
timing + amplitude shape of the MOX → RF edges. All values
persisted across Lyra restarts.

### Mic + ALC (TXA input + output gain stages)

The two operator-tunable gain stages on the WDSP TXA modulator chain
— input (mic into the modulator) and output (ALC ceiling before the
I/Q reaches the wire).

| Knob | Default | What it controls |
|---|---|---|
| **Mic Gain** | 0 dB | The mic-into-modulator gain (WDSP TXA PanelGain1 — TXA chain stage #3, before phrot / EQ / leveler / CFCOMP / bandpass / compressor / OSCtrl / ALC). **Bidirectionally bound with the TxPanel front-UI slider** — slider for quick QSO-time adjustments, spin-box here for typed precision. 0 dB = WDSP unity; +10 to +20 dB typical SSB; +25 to +35 dB ESSB with headroom. Range −90 dB to +40 dB matches the reference's Default TX profile. |
| **ALC Max Gain** ⚠ | +3 dB | The ALC (Automatic Level Control) max-gain ceiling — the limiter that catches mic-input peaks the leveler and compressor didn't bound, **before** the I/Q reaches the wire. **Load-bearing default**: WDSP's own create-time default for this is 0 dB, which pins the entire TX output chain at a hard 0-dB limiter ceiling regardless of mic level — that was the 2026-05-31 first-SSB-bench 0.2 W root cause (Lyra never called the setter before this slice). +3 dB mirrors the verified reference's profile default and lifts the ceiling enough for normal SSB peaks to pass through to the wire. Operator tuning: lower (e.g. 0 to +1 dB) for tighter splatter protection at the cost of headroom; higher (e.g. +5 to +10 dB) for more program-level headroom at the cost of splatter-protection margin. ±3 dB around the default is the sane range; outside that, ALC stops being a meaningful safety net. |

> **The ALC ceiling is a SAFETY knob, not a daily-use one.** Set it
> once to match your mic + voice + amp combination, then leave it.
> The daily-use control is Mic Gain — that's what you tune per QSO.
> Watch the ALC meter as you raise Mic Gain; if ALC is engaging hard
> (more than ~3 dB of gain reduction visible on the meter), you're
> driving past the limiter ceiling and creating splatter risk —
> either reduce Mic Gain or carefully raise the ALC ceiling.

### TR Sequencing + Amplitude Envelope (timing knobs below)

Seven operator-tunable knobs that control the **timing of the MOX → RF
edges** and the **amplitude shape** of the TX I/Q at the start and
end of every keying event. All values are in milliseconds; live-
apply (changes take effect on the next MOX edge — no restart, no
MOX cycle required). Persisted across Lyra restarts.

### TR Sequencing (PTT → wire-MOX → RF timing)

Four scheduling knobs. They unwind in this order on every keying
event:

| Knob | Default | What it controls |
|---|---|---|
| **MOX Delay** | 15 ms | From operator-PTT (your click / space-bar / TUN) to the wire-MOX bit going hot. Gives the RX-protect step-attenuator + external filter-board relays a window to settle into TX configuration **before** RF appears. |
| **RF Delay** ⚠ | 50 ms | From wire-MOX hot to the "RF settled" edge (when actual RF appears on the antenna). **This is the hot-switch protection window for external SS linears** — their T/R relay needs typically 30–50 ms to settle. The default is bench-safe for typical 1 kW SS HF linears. **Reducing this below your amp's T/R settle spec can destroy the PA.** |
| **Space MOX Delay** | 13 ms | Keyup re-key window: the time between operator-keyup and the wire-MOX bit clearing. Allows a mic-clip / CW-dot-tail re-key in this window to collapse-stay-TX (no on-the-air drop, no extra T/R cycle). Also sets the upper bound on **Fade-Out Duration** below — fade-out must fit inside this window or it gets truncated at MOX-clear. |
| **PTT-Out Delay** | 5 ms | Final cleanup window after wire-MOX clears — before step-att restores, OC pattern flips back to RX, and `moxActive` emits false. Lets external relays finish switching back **before** the RX front-end re-opens. |
| **TX-Stop Delay** | 10 ms | **In-flight UDP datagram clear window.** Time between the TX-DSP channel stopping (blocking flush) and the wire MOX bit clearing on keyup. UDP datagrams already-sent (or sitting in your OS network buffer) carrying MOX=1 + non-zero TX I/Q need this window to actually reach + be processed by the HL2 BEFORE the wire MOX state flips — otherwise the gateware could see momentary MOX=0 with stale TX I/Q from a previous datagram. Default 10 ms matches the verified-reference value. Reduce only on a very low-latency NIC + LAN where you're confident no datagrams sit in OS buffer. |

### Amplitude Envelope (cos² fade on TX I/Q)

Two knobs that shape the **amplitude** of the TX I/Q at the EP2
wire-pack stage. The shape is a cos² half-cycle (raised cosine —
smooth, no high-frequency content, the gold-standard envelope for
PTT-onset shaping).

| Knob | Default | What it controls |
|---|---|---|
| **Fade-In Duration** ⚠ | 50 ms | The cos² ramp from zero to full amplitude at keydown. **Belt-and-suspenders amp protection** — even if RF Delay is right, the soft amplitude rise INSIDE the RF Delay window adds redundant protection against any residual MOX-bit / relay timing skew. Default matches the default RF Delay so the ramp completes exactly when "RF settled" fires. **Reducing this without knowing your amp's switching behaviour can expose the PA to hot-switch transients.** |
| **Fade-Out Duration** | 13 ms | The cos² ramp from full amplitude to zero at keyup. **Must fit inside Space MOX Delay above** — the gateware DAC stops consuming TX I/Q the instant wire-MOX clears, so a fade-out longer than Space MOX Delay gets truncated mid-ramp (audible click). RF going DOWN doesn't damage amps (the relay disengages cleanly), so this is shorter than Fade-In — purely click-prevention. |

The asymmetric default (50 ms in / 13 ms out) reflects the
**asymmetric risk**: RF coming UP into a still-switching relay can
destroy amps; RF coming DOWN cannot.

### Restore hot-switch-safe defaults

A single button that resets all seven values to **15 / 50 / 13 / 5 /
50 / 13 / 10** ms — the bench-validated profile that's safe for typical
1 kW SS HF linears (and reference-faithful for the in-flight UDP
datagram clear). Use it any time you've experimented with values and
want a known-good starting point.

> **The two layers compose into one "clean PTT onset" story.** The
> TR-sequencing values control *scheduling* of the MOX → step-att →
> RF → cleanup edges; the amplitude envelope shapes the *I/Q
> amplitude* itself. At defaults the fade-in completes exactly when
> "RF settled" fires (50 ms after wire-MOX hot), and the fade-out
> completes exactly when wire-MOX clears (13 ms after operator-keyup).

> **Live-apply behaviour:** changes take effect on the next MOX edge,
> not the in-flight one. If you change a value while the radio is
> keyed, the in-flight transition finishes at the prior rate; the
> new value applies at the next keydown or keyup. So you can A/B
> values mid-session by changing then re-keying — no MOX cycle,
> no app restart.

---

## Settings → Network (TCI)

**TCI** (Transceiver Control Interface, the Expert Electronics protocol)
lets logging, cluster, and digital-mode programs drive Lyra over your
local network — set frequency, mode, and filter; read your current state;
and pull DX-cluster spots onto the panadapter. SDRLogger+, Log4OM, N1MM,
WSJT-X and others speak it.

Lyra runs a TCI **server**; the other program connects to it as a client.

- **Bind address** — which network interface to listen on. Leave blank /
  `0.0.0.0` to accept connections from any interface (e.g. another PC on
  your LAN); use `127.0.0.1` to allow only programs on this same PC.
- **Port** — the TCI listening port (default **50001**, the Expert
  default). Match this in your logger's TCI settings.
- **Rate limit** — minimum gap between repeated broadcasts of the same
  value, to avoid flooding a client during fast tuning.
- **Send full state to clients on connect** — push the current
  frequency/mode/filter the moment a client connects, so it starts in
  sync.
- **Add "CW" to the modulations list (CWL/CWU alias)** — some clients
  only understand a bare `CW`; enable this so they can select it.
- **Emulate ExpertSDR3 / Emulate SunSDR2 PRO** — report an Expert
  protocol/device name. Enable these only if a client refuses to talk to
  Lyra unless it sees a SunSDR/ExpertSDR rig.
- **TCI server running** — the master on/off. When on, the header shows
  the **● TCI** indicator (green with a client count, amber when idle).

**Audio / IQ streaming** is automatic: if a connected client asks for an
audio or IQ stream, Lyra sends it the receiver audio (or raw I/Q) as TCI
binary frames — no extra toggle needed. Lyra advertises the audio format
(48 kHz / float32 / stereo / 2048-sample frames / 50 ms TX-buffer hint)
at connect so the client decoder configures itself correctly the moment
it attaches.

> RX2 over TCI is deferred until Lyra has a second receiver. Today the
> server advertises a single channel.

### Digital modes over TCI (FT8 / FT4 / MSK144 / Q65 / etc.)

Lyra is a fully bidirectional TCI partner for the digital-modes clients
operators already use — MSHV, JTDX, WSJT-X and similar. The client
both **receives** Lyra's RX audio over TCI and **sends** its modulator
audio back over TCI for Lyra to transmit; no VAC, no virtual cables,
no host-side sound card needed.

**One-time setup:**

1. **Settings → Network (TCI)** — make sure the server is running and
   the port matches what your digital-modes client expects (default
   **50001**).
2. **Settings → Audio → Mic source** — pick **TCI**. This routes the
   digital-mode client's audio into Lyra's TX chain in place of the
   hand-mic. (The client can also auto-select TCI by sending a
   `TRX:0,true,tci` command — supported, but having the picker on TCI
   means manual TUNE buttons in the client also work.)
3. In the client (MSHV / WSJT-X / JTDX) configure: TCI server
   `127.0.0.1:50001`, sample rate **48 kHz**, block size **2048**,
   buffering **50 ms**. Most clients set these defaults out of the box;
   just confirm.

**Operating flow:**

* The client tunes Lyra's VFO and sets the mode (typically **DIGU** for
  FT8 on the standard 14.074 / 7.074 / 21.074 / … MHz dial frequencies).
* RX audio streams to the client continuously; its waterfall decodes the
  band exactly as if the client were attached to a Thetis-class radio.
* When the client wants to transmit, it sends a TRX-on command. Lyra
  engages MOX, the client's modulator audio is fed through the WDSP TXA
  chain (with your **TX mic gain**, **ALC**, **leveler**, **TX bandpass**,
  and any EQ active), and onto the HL2 wire. At keyup the client sends
  TRX-off; Lyra returns to RX.
* The **Mic gain** slider on the TX panel still applies to TCI audio —
  the same chain handles voice and digital. Once Task #49 (TX Profile
  Manager) ships you'll save per-profile mic-gain trims, so switching
  between an SSB-voice profile and an FT8/MSHV profile recalls the
  right trim automatically.

**Troubleshooting:**

* **MOX engages but the client says "no audio"** — confirm Mic source =
  TCI; without it Lyra processes audio from the configured hand-mic
  source while the client's audio gets dropped.
* **Client RX waterfall looks unusably hot** — Lyra advertises the
  format at connect so the client's RX gain slider should land in a
  sensible position. If your client doesn't honour the advertised
  parameters, drop its RX gain slider until the waterfall sits in the
  normal -20 to -10 dB range.
* **You're transmitting but nobody's decoding you (no PSKReporter
  spots)** — usually means a frequency / band / power issue on the
  station side (wrong dial, antenna fault, low drive). Check the
  external watt-meter, confirm the band is open, and try a known-good
  digital-mode sked partner. The TCI audio path itself is verified
  reference-faithful end-to-end.

### DX-cluster spots

When a TCI client (a logger wired to a DX cluster, RBN, or Skimmer) sends
spots, Lyra paints them right on the panadapter at each spotted frequency
— callsign-tagged, with the spotter's **country** abbreviation. CW spots
are placed allowing for your **CW pitch** so they sit on the actual signal,
not zero-beat. **Click a spot** to jump there. Controls live in the same
**Settings → Network** tab:

- **Show spots on the panadapter** — master on/off for the overlay.
- **Max spots** — cap how many are drawn at once.
- **Spot lifetime** — how long (in **minutes**) a spot stays before it
  ages out.
- **Highlight my callsign when spotted** + **Highlight colour** — draw
  *your* spots in a color you pick (default pink/magenta) so they stand out.
- **Pop up a notification when I'm spotted** — show a toast (and the
  header **★ Spotted!** badge) when your own callsign is spotted.
- **Re-notify after** — how long to wait before notifying again about
  your callsign, so you're not spammed (set to *every time* to disable the
  cooldown).
- **Clear spots now** — wipe the current spot list.

---

## Settings → Visuals

Everything that controls how the panadapter *looks* lives on the
**Visuals** tab of Settings (**Ctrl+,**). Changes apply live — you see
them immediately. Your choices are saved between sessions.

### Trace color

How the spectrum trace (the moving signal line) is colored. Pick a mode
from the dropdown:

- **Solid color** — one fixed color for the whole trace. Choose from the
  preset color chips, or click **Custom color…** for any color you like.
- **By signal strength** — the trace color changes with how strong each
  part of the signal is, like a heat map (weak = dark/cool, strong =
  bright/hot). Pick a **palette** and a **preview strip** shows it (weak
  left, strong right). Choose **Custom color…** in the palette list to
  build your own dark-to-bright ramp from a base color.

### Spectrum fill

**Show fill beneath the trace** — fills the area under the signal line
with a soft gradient for a richer look. Turn it off for a clean,
line-only trace. (You can pick a separate **fill color** in solid mode.)

### Peak markers

Highlights the **held peak** of each signal across the tuned passband — a
running "high-water mark" so you can see the strongest a signal has been.
Drive the **mode** (Off / Live / timed / Hold) and **Decay** from the
[Display panel](#display-panel); the look lives here:

- **Show peak markers** — master on/off (the Display panel's Peak Hold
  combo also turns this on when you pick a mode).
- **Style** — **Line**, **Dots**, or **Triangles** (default Dots).
- **Show dB** — label the strongest few peaks with their level in dB.
- **Colour** — the marker color (default warm amber).
- **Decay** — fade rate in dB/s for the timed hold modes (the Display
  panel's Fast/Med/Slow are presets for this).

Peaks are drawn only inside your RX filter passband, so they track the
signal you're actually listening to.

### Noise-floor line

**Show noise-floor line** — a dashed reference line at the current noise
floor, with an **"NF −NN dBFS"** label. Handy for judging how far a signal
sticks up out of the noise. Pick its **Colour** (default sage green). On
by default.

### Trace smoothing

Smooths the trace over time so it's less jittery. **0 = Off** (rawest,
most responsive); higher values (up to **10**) give a calmer trace. This
is time-based smoothing — it does **not** flatten or hide real peaks.

### Peak glow

Adds a luminous bloom around strong peaks so they pop off the screen.
**0 % = Off**; raise it (up to **100 %**) for more glow. Costs a little
graphics work only when it's on.

### Glass sheen

A subtle glassy highlight across the panadapter for the "floating glass"
look. **0 % = Off**; **20 %** is the default. Raise for a stronger sheen.

### Gridline brightness

How bright the reference grid lines are, **0** (no grid) to **100**
(bright). Default **35**.

### Frame rate

How many times per second the panadapter redraws, **1–240 fps**. Default
**60**. Higher is smoother but uses more graphics power.

### dB range (floor / ceiling)

The exact numbers for the bottom (**floor**) and top (**ceiling**) of the
signal-strength scale. Most operators just drag the scale on the
panadapter edge, but you can type precise values here.

### Watermark

**Show Lyra constellation watermark** — a faint Lyra (the lyre /
constellation) image with a gently pulsing **Vega** star, over the
panadapter. On by default. Untick to hide it.

### Meteors

Occasional "shooting star" streaks that drift across the panadapter —
ambient sky-weather, deliberately rare.

- **Show occasional meteor streaks** — master on/off (on by default).
- **Meteor frequency** — average gap between meteors, **5–120 s**
  (default **30 s**). Lower = more frequent.
- **Gold fireballs** — percent chance a meteor is a warm-gold "fireball"
  instead of cool blue, **0–100 %** (default **15 %**).

### Graphics backend

*Advanced — restart to apply.* Which graphics engine Lyra renders with.
Leave it on **Auto (recommended)** — on Windows that's Direct3D 11, the
most compatible. **Vulkan / D3D12 / D3D11 / OpenGL** can be forced if you
know your card prefers one. (If panels ever fail to draw or crash when you
drag them, set this to **Direct3D 11** or **Auto** and restart.) This
setting stays on the machine — it isn't carried in an exported profile.

---

## Settings → Weather

Configure the [weather alerts](#weather-alerts) here. Nothing runs until
you accept the disclaimer and tick **Enable**.

- **Disclaimer** — tick to acknowledge the alerts are advisory only; this
  unlocks **Enable weather alerts**.
- **Sources** (enable any combination):
  - **Blitzortung** — global lightning network, free, no key. Uses your
    location.
  - **NWS / weather.gov** — US wind + severe-storm alerts, free, no key.
  - **NWS METAR** — live wind from a nearby station (set its **ICAO** below).
  - **Ambient Weather** — your own station (needs API + Application keys).
  - **Ecowitt** — your own station (needs Application key + API key + the
    gateway MAC).
- **Thresholds:**
  - **Lightning range** — how far out to consider strikes (shown in your
    chosen distance unit).
  - **Wind sustained ≥ / gust ≥** — the speeds (mph) that raise the wind
    tier.
  - **METAR station** — the ICAO id (e.g. `KTOL`) for live wind.
  - **Distance unit** — Miles or Kilometres (also used by the ⚡ badge).
- **Notifications:**
  - **Desktop notification on alert** — a system pop-up on a new alert.
  - **Audible chime on alert** — a beep on a new alert.
  - **Send test alert** — lights the badges + fires one notification for a
    few seconds so you can check it all works.
- **Station API credentials** — Ambient API/Application keys, Ecowitt
  Application/API keys, and Ecowitt gateway MAC (only needed for those
  sources).

Remember to set your location in
[Settings → Hardware → Operator / Station](#operator--station) — without
it, the location-based sources can't tell what's nearby.

---

## Credits and References

Lyra is a native C++23 / Qt 6 / Vulkan rebuild written from scratch by
**Rick Langford (N8SDR)**. The projects, documents, and conventions below
were studied for *ideas, ballistics, and protocol structure* while
building it. **No source code was copied** — each is referenced as
"how the standard idiom works," and Lyra implements each idea natively
in C++.

### Inspiration and references (studied, not copied)

- **Thetis SDR** (openHPSDR) — primary reference for HPSDR Protocol 1
  wire format, TX-chain ordering, meter ballistics, ATT-on-TX policy,
  TR-sequencing conventions, AAmixer routing.
- **openHPSDR project** — HPSDR Protocol 1 / 2 specifications, the
  authoritative wire-protocol documents.
- **PowerSDR** — Thetis's predecessor; convention provenance for many
  UI idioms (per-mode bandwidth memory, AGC profiles, etc.).
- **HermesLite 2 wiki + ak4951v4 gateware (RTL)** — the gateware-
  designer's authoritative register documentation, plus the Verilog
  source of the operator's specific HL2+ variant. The RTL is the
  ground-truth source for EP6 telemetry slot decoding, PA-enable
  mechanism, and TX-I/Q wire gating on the AK4951 gateware revision.
- **pihpsdr** — cross-reference for HL2 PA-enable bit, TX-drive
  nibble, and the generic Protocol-1 HL2 wire path.
- **Quisk** — cross-reference for HermesLite hardware register
  conventions (`hermes/quisk_hardware.py`).
- **linHPSDR** — additional cross-reference for the Protocol-1
  HL2 wire path (`protocol1.c`).
- **EESDR V3** — UI convention reference and the public **TCI**
  (Transceiver Control Interface) protocol specification that Lyra
  implements against.
- **Behringer X-Air mixer series** — operator-supplied screenshots
  drive the upcoming Lyra-native **Combinator** + 5-band parametric
  EQ design (lands in v0.2.1). The X-Air's multiband compressor +
  EQ behaviour is the reference for Lyra's TX dynamics chain.
- **SparkSDR** (binaries only — no source) — referenced for the HL2
  push-pull PA bias procedure documentation.
- **Lyra (Python predecessor)** — the author's earlier PySide6 SDR
  application. Several Lyra-cpp modules (band table, band-plan data,
  open-collector pin tables, grid-square math, frequency-display
  widget) are explicit ports from the Python predecessor — same
  author, same project lineage, internal port.

Lyra's source tree intentionally keeps these names out of code,
comments, commits, and operator-visible UI strings (the
"no-attribution rule"): the references inform the design, but every
line of Lyra is Lyra-native C++. This Credits section is the single
operator-facing place that names them, openly and once.

### Licensed components (GPL v3+)

Lyra is **GPL v3 or later** and is therefore license-compatible with
the GPL-licensed components it depends on:

- **WDSP** — DSP engine by **Warren Pratt (NR0V)**. GPL v3+. Lyra
  links to the WDSP shared library (`wdsp.dll` + FFTW companions)
  and calls its public C API through C++ wrappers. **No source
  modifications.** WDSP powers the RXA chain (noise reduction,
  AGC, bandpass, demod) and the TXA chain (modulator, ALC, leveler,
  bandpass, future EQ/compressor).
- **TCI protocol** (Transceiver Control Interface) — public
  specification from **Expert Electronics** (EESDR). Lyra
  implements a TCI server compatible with the v1.9 / v2.0 spec so
  external logging / cluster / digital-modes software (SDRLogger+,
  any TCI-aware client) can drive Lyra.
- **WDSP FFTW** (Fastest Fourier Transform in the West) — GPL.
  Used by WDSP internally; bundled as `libfftw3-3.dll` /
  `libfftw3f-3.dll`. Lyra's first-launch FFTW WISDOM build optimises
  FFT plans for the host CPU and caches them under
  `%APPDATA%\Lyra\fftw\`.

### Contributors

- **Rick Langford (N8SDR)** — project lead, architecture, all code.
- **Brent Crier (N9BC)** — co-contributor (joined during v0.0.9.1
  testing on the Python predecessor; continues into the C++ rebuild).
- **Timmy Davis (KC8TYK)** — tester (v0.1 tester flight).
- **W5UDX** — DSP2024P Plate Reverb presets and verification (lands
  with the Plate Reverb in v0.2.1).

### License

**GPL v3 or later.** Source repository:
[github.com/N8SDR1/Lyra-SDR-cpp](https://github.com/N8SDR1/Lyra-SDR-cpp).
Full license text: `LICENSE` / `NOTICE.md` in the source tree. See
the **About Lyra** dialog (**Help → About Lyra…**) for the version
+ build date of the running binary.

---

## Support Lyra — buy the developer a coffee ☕

Lyra is **free**, **open-source** (GPL v3 or later — see the License), and
primarily built by **Rick Langford (N8SDR)** in his spare time, with
**Brent Crier (N9BC)** joining as co-contributor during early testing and
**Timmy Davis (KC8TYK)** joining for the v0.1 tester flight. There are no
ads, no telemetry, no subscription tier, no "pro" upsells.

If Lyra has saved you from a clunky SDR workflow, helped you work a new
state on FT8, or just makes your shack a little more comfortable, **a small
donation goes a long way** — it pays for the development hardware and keeps
the project moving through the long TX-path work still ahead.

**[☕ Donate via PayPal](https://www.paypal.com/donate/?business=NP2ZQS4LR454L&no_recurring=0&item_name=Built+by+a+fellow+ham%2C+for+the+community.++Free+to+use%2C+free+to+share.+A+small+donation+keeps+the+code+flowing.+73+de+N8SDR&currency_code=USD)**

Any amount is welcome — $5 buys a coffee, $20 buys a pint at the hamfest,
$100 keeps the development bench rolling for a month. You can also reach the
donate page any time from **Help → About Lyra**.

> *Built by a fellow ham, for the community. Free to use, free to share.
> A small donation keeps the code flowing. 73 de N8SDR.*

---

*Last updated as features ship. Sections are added when their feature
lands — see the maintainer note at the top.*
