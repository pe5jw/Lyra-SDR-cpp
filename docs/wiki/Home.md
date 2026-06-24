# Lyra — Hermes Lite 2 / 2+ SDR Transceiver

A native **C++23 / Qt 6** SDR transceiver for the **Hermes Lite 2 / 2+** over
HPSDR Protocol 1. Full receive **and transmit** — SSB, AM, DSB, SAM, FM, CW,
and digital (TCI / virtual audio cable) — with a studio‑grade native TX audio
rack, a GPU panadapter, and no Python/GIL anywhere in the signal path.

> **Latest release:** **[v0.4.8](https://github.com/N8SDR1/Lyra-SDR-cpp/releases/latest)**
> — panel docking overhaul + CW transmit metering.
> Download **`Lyra-Setup-X.Y.Z.exe`** and run it.

---

## Download & install (Windows)

1. Grab the latest **[`Lyra-Setup-X.Y.Z.exe`](https://github.com/N8SDR1/Lyra-SDR-cpp/releases/latest)** installer.
2. Run it. The installer adds the Windows Firewall rules Lyra needs, so it
   **connects without administrator rights** (and offers an optional, unticked
   network‑throttling tweak for glitch‑free SDR audio).
3. Launch Lyra, **▶ Start**, and pick your radio. First launch builds the FFT
   plan cache once (a one‑time "optimizing" step), then opens normally.

Windows 10 1809+ / 64‑bit. Requires a GPU with OpenGL 3.3+ for the panadapter.

## User Guide

The full operator's guide lives in the repo and renders on GitHub:
**[docs/help/USER_GUIDE.md](https://github.com/N8SDR1/Lyra-SDR-cpp/blob/main/docs/help/USER_GUIDE.md)**
(also built into the app — **Help → User Guide**). It covers getting on the
air, the panadapter, tuning/filters/bands, audio + mic setup, the meter, the
TX panel and DSP rack, CW, profiles, and operating with an external amp.

## Features

- **Radio** — HPSDR P1 discovery (multi‑NIC) + **Add by IP** for fixed‑IP /
  cross‑subnet radios; live RX on dedicated OS threads; multi‑radio list.
- **DSP** — WDSP RX chain: USB/LSB/CW/AM/FM/DIG, per‑mode filters, AGC, NR,
  auto/manual notches, squelch, 8‑band RX parametric EQ, centre‑tune (CTUN).
- **Transmit** — every voice mode (proper carrier + both sidebands on AM/SAM,
  suppressed‑carrier DSB), CW, and digital via TCI / VAC; TX power/drive,
  separate tune drive, AM carrier level, mic gain + boost, always‑on ALC +
  Leveler.
- **Native TX DSP rack** — 8‑band parametric EQ with draggable curve + live
  RTA, multi‑stage Speech (gate / auto‑AGC / de‑esser), 5‑band Combinator,
  and a Plate reverb for ESSB air.
- **CW** — internal iambic keyer (paddle / straight key), keyboard send +
  contest macro bank, QSK / semi / manual break‑in, adjustable sidetone. The
  meter shows forward power and the VFO reds on‑air while you send.
- **Metering & safety** — multimeter (PO / SWR / MIC / COMP / ALC / PA
  current); ATT‑on‑TX RX‑front‑end protection; TR‑sequencing for amp
  hot‑switch safety; operator TX time‑out; hard External TX Inhibit.
- **Panadapter + waterfall** — Qt RHI scene‑graph spectrum with glassy
  fill/glow, peak‑hold, noise‑floor line, palettes; click/drag/wheel tuning;
  draggable passband; collapsible waterfall.
- **Dockable UI** — drag‑to‑dock (snap to edge / split / tab / float), cyan
  resize separators, four named layout slots + a factory default, and a Lock
  that freezes move *and* resize.
- **Extras** — waterfall callsign ID, band switching + per‑band memory,
  RX CW decoder, solar/propagation panel, weather alerts, TX/RX profiles,
  settings export/import.

On the roadmap: dual receiver (RX2) and PureSignal.

## Links

- **[Releases](https://github.com/N8SDR1/Lyra-SDR-cpp/releases)** — installers + notes
- **[Report a bug / request a feature](https://github.com/N8SDR1/Lyra-SDR-cpp/issues)**
- **[README](https://github.com/N8SDR1/Lyra-SDR-cpp/blob/main/README.md)** — features, build from source
- **License:** GPL v3+ (WDSP‑compatible) — see [NOTICE](https://github.com/N8SDR1/Lyra-SDR-cpp/blob/main/NOTICE.md)

---

*Lyra is built by Rick Langford (N8SDR) for the Hermes Lite 2 community.*
