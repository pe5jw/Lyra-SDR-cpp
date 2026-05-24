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
- [Getting around the window](#getting-around-the-window)
- [The panadapter (spectrum display)](#the-panadapter-spectrum-display)
- [Tuning panel](#tuning-panel)
- [Mode + Filter panel](#mode--filter-panel)
- [Band panel](#band-panel)
- [Audio panel](#audio-panel)
- [Display panel](#display-panel)
- [Weather alerts](#weather-alerts)
- [Updates](#updates)
- [Backing up & sharing your settings](#backing-up--sharing-your-settings)
- [Settings → Hardware](#settings--hardware)
  - [Operator / Station](#operator--station)
  - [Band plan (Region)](#band-plan-region)
  - [Radio](#radio)
  - [Filter board (N2ADR / compatible)](#filter-board-n2adr--compatible)
  - [USB-BCD (linear-amp band switching)](#usb-bcd-linear-amp-band-switching)
- [Settings → Audio](#settings--audio)
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
- **Audio panel** — **Mute / Unmute** and set **Vol**. Choose the output
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
- **⬆ vX.Y.Z** (only when present) — an update is available; click it to
  open the release page. See [Updates](#updates).
- **Clocks** — a **local** clock (amber) and a **UTC / Zulu** clock (cyan,
  ending in *Z*), centered in the header. They read straight off your PC
  clock, so keep that synced for accurate UTC.
- **Weather badges** — ⚡ lightning, 💨 wind, ⚠ severe — appear toward the
  right edge when an alert is active and **flash** on the most serious
  tier. Hidden when all-clear. See [Weather alerts](#weather-alerts).

---

## Getting around the window

Lyra's window is built from **dockable panels** — like a workbench you
arrange to taste. Lyra opens **full screen (maximized)** by default — the
way it's meant to be run — in a curated default layout (panadapter on top,
control panels in a row beneath).

- **Move a panel:** drag it by its title bar. Drop it against an edge to
  re-dock, or out on its own to float.
- **Resize the spectrum vs. waterfall:** drag the divider between the
  panadapter and the waterfall. Its position is remembered with your layout.
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

- **Click** anywhere to tune RX1 there.
- **Click + drag** left/right to pan across the band.
- **Mouse wheel** steps the frequency (by the Tuning panel's Step size).
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
Settings → Visuals, with quick controls on the Display panel.

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
  (sensible presets for SSB, CW, AM, FM, digital), and Lyra remembers the
  width you last used for each mode. If you **drag a passband edge** on the
  panadapter to a width that isn't a preset, the combo shows it as
  **"(custom)"** at the top of the list so the readout always matches what
  you're actually hearing; pick a preset to snap back.

SSB and digital filters open right at the carrier (no low-cut gap); CW
filters are centered on your **CW Pitch**; AM/DSB/FM are symmetric around
the carrier.

---

## Band panel

Quick band switching across the HF/6m amateur bands. Click a band button
(**160m … 6m**) and Lyra tunes RX1 to that band's default frequency. The
button for the band you're currently on lights up (a red-glow highlight),
so you can see at a glance where you are — and it follows the frequency,
so it updates whenever you tune into a different band by any means.

Pick the mode and filter for the band in the **Mode + Filter** panel.

---

## Audio panel

Controls what you hear.

- **Mute / Unmute** — silences or restores the audio; the **LIVE / MUTED**
  label shows the current state at a glance. (Lyra starts **unmuted**.)
- **Vol** — output volume, shown in dB beside the slider (−∞ when fully
  down).

Choosing **where** the audio goes (HL2 headphone jack vs. a PC sound
device) lives in **[Settings → Audio](#settings--audio)** — it's a
set-once choice, not an everyday control.

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
- **💨 Wind** — shows gust (or sustained) speed. Yellow/orange by your
  thresholds, **red = extreme** (flashing).
- **⚠ Severe** — an NWS thunderstorm/severe warning is active (red,
  flashing). Hover for the headline.

**Where the data comes from** depends on which sources you enable
(Settings → Weather): **Blitzortung** (global lightning, free),
**NWS / weather.gov** (wind + severe + live station wind, free, US), and
your own **Ambient Weather** or **Ecowitt** station (needs that account's
keys). Lyra needs your **location** to know what's "nearby" — set your
grid square in [Settings → Hardware → Operator / Station](#operator--station).

**Notifications** — optionally a desktop pop-up and an audible chime when
an alert first crosses a tier (with a cool-down so it doesn't nag). Toggle
both in Settings → Weather, and use **Send test alert** there to see the
badges light up.

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
panadapter band-plan overlay uses. *(The overlay itself — sub-band
shading, landmarks, out-of-band advisories — is coming in a later build;
this setting is here and remembered so it's ready when the overlay lands.)*

### Radio

Find and connect to your HL2 / HL2+. **Discover** scans the LAN, **Open**
connects to the selected radio, **Close** disconnects, and the status line
shows what you're connected to. Lyra remembers the last radio and shows it
here on launch. (The toolbar **▶ Start / ■ Stop** does the same thing.)

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

*Last updated as features ship. Sections are added when their feature
lands — see the maintainer note at the top.*
