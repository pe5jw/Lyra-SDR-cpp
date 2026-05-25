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
- [Meter panel](#meter-panel)
- [Solar / Propagation panel](#solar--propagation-panel)
- [Weather alerts](#weather-alerts)
- [Updates](#updates)
- [Backing up & sharing your settings](#backing-up--sharing-your-settings)
- [Settings → Hardware](#settings--hardware)
  - [Operator / Station](#operator--station)
  - [Band plan (Region)](#band-plan-region)
  - [Diagnostics (debug log)](#diagnostics-debug-log)
  - [Radio](#radio)
  - [Filter board (N2ADR / compatible)](#filter-board-n2adr--compatible)
  - [USB-BCD (linear-amp band switching)](#usb-bcd-linear-amp-band-switching)
- [Settings → Audio](#settings--audio)
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
  changing LNA doesn't shift the signal reading. **Auto · AF · Bal · Out**
  are still greyed in their final positions — each lights up as its
  control is wired in. Choosing **where** the audio goes (HL2 headphone
  jack vs. a PC sound device) lives in **[Settings → Audio](#settings--audio)**.

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

In **[Settings → Meter](#settings--audio)** you can tune both timings:
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
> **[Settings → Meter](#settings--audio)**: tune to a known reference
> (WWV, or a signal generator at a known level) and adjust the **S-meter
> calibration** offset until it matches. With this tap the trim is only a
> few dB. The meter compensates for the **LNA gain** automatically, so the
> reading stays put as you adjust LNA — you can calibrate at any setting.
> The relative movement, peak-hold, SNR, and
> noise-floor behaviour are all live regardless.
>
> Transmit meters (Power / SWR / ALC / Mic, etc.) arrive with the
> transmit feature — the meter panel will gain those sources then.

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
binary frames — no extra toggle needed.

> RX2 over TCI is deferred until Lyra has a second receiver. Today the
> server advertises a single channel.

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
