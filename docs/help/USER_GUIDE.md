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
- [Getting around the window](#getting-around-the-window)
- [The panadapter (spectrum display)](#the-panadapter-spectrum-display)
- [Tuning panel](#tuning-panel)
- [Band panel](#band-panel)
- [Audio panel](#audio-panel)
- [Display panel](#display-panel)
- [Settings → Hardware](#settings--hardware)
  - [Radio](#radio)
  - [Filter board (N2ADR / compatible)](#filter-board-n2adr--compatible)
  - [USB-BCD (linear-amp band switching)](#usb-bcd-linear-amp-band-switching)
- [Settings → Visuals](#settings--visuals)
  - [Trace color](#trace-color)
  - [Spectrum fill](#spectrum-fill)
  - [Trace smoothing](#trace-smoothing)
  - [Peak glow](#peak-glow)
  - [Glass sheen](#glass-sheen)
  - [Gridline brightness](#gridline-brightness)
  - [Frame rate](#frame-rate)
  - [dB range (floor / ceiling)](#db-range-floor--ceiling)
  - [Watermark](#watermark)
  - [Meteors](#meteors)

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
  decoded on the PC and played through whatever output you pick in the
  **Audio** panel.
- **HL2+** — the HL2 base **plus** the AK4951 audio add-in board (and the
  updated HL2+ gateware). Adds on-board audio routing and a microphone
  input for future transmit.

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
the **Radio** section:

1. Power on the HL2 / HL2+ and make sure it's on the same network.
2. Click **Discover** to scan the LAN. Radios that answer appear in the
   list below.
3. Select your radio and click **Open** to start the stream — the status
   line reads **Connected to …**. (Lyra also auto-connects to the last
   radio you used, so it may already be running.)
4. You should see a live spectrum and hear the band noise floor. Click
   **Close** to disconnect.

### 4. Tune and listen

- **Tuning panel** — set the **RX1** frequency: type it in MHz or use the
  **−10k / −1k / +1k / +10k** step buttons. Jump between bands from the
  **Band panel**.
- **Audio panel** — **Mute / Unmute**, set **Vol**, and choose the
  output device under **Out**.
- **Panadapter** — drag the right-hand edge to set the signal-strength
  scale; see [The panadapter](#the-panadapter-spectrum-display).

That's the whole receive path. Make it look the way you like under
**Settings → Visuals** (**Ctrl+,**), and arrange the panels however suits
you — see [Getting around the window](#getting-around-the-window).

### Finding your version

The version is shown in the **title bar** and under **Help → About
Lyra…**. Include it when reporting a problem so it can be matched to the
exact build.

---

## Getting around the window

Lyra's window is built from **dockable panels** — like a workbench you
arrange to taste. Lyra opens **full screen (maximized)** by default — the
way it's meant to be run — with the panadapter filling the top and the
control panels in a row beneath.

- **Move a panel:** drag it by its title bar. Drop it against an edge to
  re-dock, or out on its own to float.
- **Resize the spectrum vs. waterfall:** drag the divider between the
  panadapter and the waterfall. Its position is remembered along with
  your layout.
- **Show / hide panels:** the **View** menu lists every panel; tick or
  untick to show or hide it.
- **Lock the layout:** **View → Lock panels** (or **Ctrl+L**) freezes
  everything in place so you can't nudge a panel by accident during
  operating. Unlock the same way. Your window size, panel layout, and
  divider position are remembered between sessions.

**Saving a layout you like.** Once the panels are arranged the way you
want them:

- **View → Save current layout as my default** — snapshots the current
  arrangement (panels *and* the spectrum/waterfall divider).
- **View → Restore my saved layout** — snaps everything back to that
  snapshot any time you've shuffled things around.
- **View → Reset to default layout** — returns to Lyra's built-in
  arrangement (panadapter on top, control panels in a row beneath).

**Opening Settings:** **File → Settings…**, or press **Ctrl+,**.

---

## The panadapter (spectrum display)

The panadapter is the live picture of the radio spectrum — signals show
up as peaks rising out of the noise floor. Lyra draws it with a glassy,
glowing look that takes advantage of your graphics card.

**The dB scale (signal-strength scale) down each side.** The numbers on
the left and right edges show signal strength in dB. You can adjust the
scale by **dragging on the right-hand edge** of the panadapter:

| Where you drag (right edge) | What it does |
|---|---|
| **Top third** | Raises / lowers the **ceiling** (top of the scale) |
| **Bottom third** | Raises / lowers the **floor** (bottom of the scale) |
| **Middle third** | **Pans** the whole scale up or down together |

Dragging **up** always raises that edge — like lifting the scale. This is
the quick way to "zoom" the vertical scale so signals fill the display
nicely. (The exact numbers are also in **Settings → Visuals → dB range**.)

**The frequency scale** runs along the bottom in MHz, centered on your
receive frequency.

---

## Tuning panel

Sets the **RX1 receive frequency** (the radio's first receiver).

- **Type a frequency** in MHz in the center field and press Enter.
- **Step buttons** nudge the frequency: **−10k / −1k / +1k / +10k**
  (kilohertz).

To jump between bands, use the **Band panel**.

The field tracks the radio as you tune by other means, but it won't
overwrite what you're typing mid-edit.

---

## Band panel

Quick band switching across the HF/6m amateur bands. Click a band button
(**160m … 6m**) and Lyra tunes RX1 to that band's default frequency. The
button for the band you're currently on lights up (a red-glow highlight),
so you can see at a glance where you are — and it follows the frequency,
so it updates whenever you tune into a different band by any means.

Each band also has a default mode (LSB on 160/80/40, CWU on 30, USB on
the higher bands) — Lyra will switch to it automatically once mode
selection is added.

---

## Audio panel

Controls what you hear.

- **Mute / Unmute** — silences or restores the audio; the **LIVE /
  MUTED** label shows the current state at a glance.
- **Vol** — output volume, shown in dB beside the slider (−∞ when fully
  down).
- **Out** — the output device. Pick where Lyra sends received audio
  (your sound card, headphones, a virtual cable to digital-mode
  software, etc.).

---

## Display panel

The handful of spectrum/waterfall controls you reach for most often,
one click away instead of buried in Settings. These are the same
settings as **Settings → Visuals** — change them in either place and
both stay in sync.

- **Spec** — spectrum frame rate (how many times per second the
  panadapter redraws).
- **WF** — waterfall scroll rate (history rows per second).

More controls (zoom, peak/data hold) appear here as those features land.
The look-and-feel settings — palettes, smoothing, glow, gridline — live
in **Settings → Visuals** (they're set-and-forget, not everyday tweaks).

---

## Settings → Hardware

The **Hardware** tab (Settings — **Ctrl+,**) is where Lyra connects to
the radio and drives optional station hardware.

### Radio

Find and connect to your HL2 / HL2+. See [Getting started](#getting-started)
for the step-by-step: **Discover** scans the LAN, **Open** connects to the
selected radio, **Close** disconnects, and the status line shows what
you're connected to. Lyra remembers the last radio and shows it here on
launch.

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

## Settings → Visuals

Everything that controls how the panadapter *looks* lives on the
**Visuals** tab of Settings (**Ctrl+,**). Changes apply live — you see
them immediately. Your choices are saved between sessions.

### Trace color

How the spectrum trace (the moving signal line) is colored. Pick a mode
from the dropdown:

- **Solid color** — one fixed color for the whole trace. Choose from the
  preset color chips, or click **Custom color…** for any color you like.
  This is the classic look.
- **By signal strength** — the trace color changes with how strong each
  part of the signal is, like a heat map (weak = dark/cool, strong =
  bright/hot). Pick a **palette** from the dropdown and a **preview strip**
  shows you exactly what that palette looks like (weak on the left,
  strong on the right). Choose **Custom color…** in the palette list to
  build your own dark-to-bright ramp from a base color of your choosing.

### Spectrum fill

**Show fill beneath the trace** — fills the area under the signal line
with a soft gradient for a richer look. Turn it off for a clean,
line-only trace.

### Trace smoothing

Smooths the trace over time so it's less jittery. **0 = Off** (rawest,
most responsive); higher values (up to **10**) give a calmer, smoother
trace. This is time-based smoothing — it does **not** flatten or hide real
signal peaks, it just settles the noise.

### Peak glow

Adds a luminous bloom around strong peaks so they really pop off the
screen. **0 % = Off**; raise it (up to **100 %**) for more glow. Costs a
little graphics work only when it's on.

### Glass sheen

A subtle glassy highlight across the panadapter for the "floating glass"
look. **0 % = Off**; **20 %** is the default — tasteful. Raise for a more
pronounced sheen.

### Gridline brightness

How bright the reference grid lines are, from **0** (no grid) to **100**
(bright). Default **35** — visible but not distracting.

### Frame rate

How many times per second the panadapter redraws, **1–240 fps**. Default
**60**. Higher is smoother but uses more graphics power; lower saves
power on a modest machine.

### dB range (floor / ceiling)

The exact numbers for the bottom (**floor**) and top (**ceiling**) of the
signal-strength scale. Most operators just drag the scale on the
panadapter edge (see above), but you can type precise values here.

### Watermark

**Show Lyra constellation watermark** — a faint Lyra (the lyre /
constellation) image with a gently pulsing **Vega** star, drawn over the
panadapter as a tasteful background mark. On by default. Untick to hide
it.

### Meteors

Occasional "shooting star" streaks that drift across the panadapter —
ambient sky-weather, deliberately rare so they're a pleasant surprise
rather than a distraction.

- **Show occasional meteor streaks** — master on/off (on by default).
- **Meteor frequency** — the average gap between meteors, **5–120 s**
  (default **30 s**). Lower = more frequent.
- **Gold fireballs** — the percent chance a meteor is a warm-gold
  "fireball" instead of the usual cool blue, **0–100 %** (default **15 %**).
  Set to 0 for all-blue meteors.

---

*Last updated as features ship. Sections are added when their feature
lands — see the maintainer note at the top.*
