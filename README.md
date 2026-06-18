# Lyra — C++23 / Qt 6 Rebuild

Native rebuild of [Lyra](../lyra) (the Python+Qt6 desktop SDR transceiver
for the Hermes Lite 2 / 2+).  The Python tree is preserved in `../lyra/`
as the protocol research + doc archive reference; this is a clean ground-
up rewrite using the architecture the project should have started with.

> **Acknowledgments at a glance:** Lyra-cpp incorporates code ported
> from two GPL-compatible open-source projects:
> **[WDSP](https://github.com/TAPR/OpenHPSDR-wdsp)** (Dr. Warren Pratt,
> NR0V) for DSP algorithms, and the **[MI0BOT openHPSDR-Thetis
> fork](https://github.com/mi0bot/OpenHPSDR-Thetis)** (Reid MI0BOT/GI8TME,
> building on Richie Samphire ramdor's upstream maintained at
> https://github.com/ramdor/Thetis) for the TX baseline ChannelMaster
> architecture.  Both upstream projects are GPL v3+; per-file copyright
> headers are preserved in every ported file.  See [NOTICE.md](NOTICE.md)
> and [CREDITS.md](CREDITS.md) for full attribution.

## Architecture

| Layer            | Choice                                                                                              |
|------------------|-----------------------------------------------------------------------------------------------------|
| Language         | C++23                                                                                               |
| UI framework     | Qt 6 (Qt Quick / QML) — Lyra-native                                                                 |
| Graphics         | Qt RHI (Vulkan/D3D12 on Windows, Metal on macOS, OpenGL fallback)                                   |
| RX DSP           | WDSP DSP engine (Warren Pratt NR0V, GPL v3+) — loaded natively                                      |
| RX architecture  | Lyra-native (no Python, no GIL) + Lyra-native flair (NPE modes, AEPF, captured-profile IQ-domain NR, per-band bounds memory, EiBi overlay, NCDXF beacon follow, ...) |
| **TX baseline**  | **Ported from [openHPSDR Thetis (MI0BOT fork)](https://github.com/mi0bot/OpenHPSDR-Thetis) ChannelMaster (cmaster, aamix, ilv, supporting modules) — GPL v3+, attributed per file** |
| **TX DSP additions** | **Lyra-native** (Combinator multiband compressor, Plate Reverb, parametric EQ, formant boost, sibilance enhance, DX cut-through, de-esser, auto-AGC — sit BEFORE the ported TXA chain) |
| FFT              | FFTW3 (per-host wisdom-cached on first launch)                                                      |
| Wire I/O         | Native UDP (`QUdpSocket`) on dedicated OS threads — no GIL                                          |
| Threading        | `std::jthread` + Qt thread pools, OS-priority + MMCSS later                                         |
| Build            | CMake 3.21+                                                                                         |
| Compiler         | MSVC v143 (VS 2022/2026) on Windows; clang/gcc on Linux; Apple Clang on macOS                       |
| License          | GPL v3+ (matches both upstream projects)                                                            |

**No Python. No GIL. No cffi-on-the-wire-path. No in-process bottleneck.**

## Features (v0.4.0)

A full receive **and transmit** SDR transceiver for the Hermes Lite 2 / 2+,
native C++ end to end.  Lyra transmits every voice mode (SSB / AM / DSB /
SAM / FM) plus digital via TCI, and ships a complete native TX audio
processing rack.  (Still on the roadmap: CW transmit, dual receiver / RX2,
and PureSignal — see below.)

* **Radio** — HPSDR Protocol 1 discovery (multi-NIC) + live RX off the
  HL2/HL2+ on dedicated OS threads; auto-connect to the last radio.
* **DSP** — WDSP RX chain: USB/LSB/CW/AM/FM/DIG modes, per-mode filters,
  AGC, NR, auto-notch, manual notches, squelch.  Audio out the HL2 jack
  (AK4951) or PC sound card.
* **Transmit** — every voice mode: SSB, AM, DSB, SAM, FM (proper carrier +
  both sidebands on AM/SAM, suppressed-carrier DSB), plus digital via TCI /
  virtual audio cable.  TX power / drive, separate tune drive, AM carrier
  level, mic gain + boost, always-on ALC + operator Leveler.
* **Native TX DSP rack** — a studio audio chain built into the radio, ahead
  of the modulator: an 8-band parametric EQ with a draggable curve + live
  RTA, a multi-stage Speech section (noise gate, auto-AGC, de-esser), a
  5-band Combinator multiband compressor, and a Plate reverb for ESSB air.
* **TX profiles & metering** — save/recall the whole transmit chain as named
  profiles; multimeter (PO / SWR / MIC / COMP / ALC / PA current); hot-mic
  monitor (HL2 jack, a PC device, or over TCI).
* **TX safety** — ATT-on-TX RX-front-end protection, TR-sequencing for amp
  hot-switch safety, and an operator TX time-out.
* **Panadapter + waterfall** — Vulkan/RHI scene-graph spectrum with
  glassy fill/glow, peak-hold markers, noise-floor line, palettes; click/
  drag/wheel tuning; draggable RX passband; collapsible waterfall.
* **Dockable UI** — movable/floatable/tabbed panels (panadapter, tuning,
  mode+filter, audio, display, band, solar) with save/restore layout +
  export/import of the full settings profile.
* **Band switching + per-band memory** — three rows (Ham / BC / Gen),
  returns to each band's last frequency, mode, and panadapter/waterfall dB
  ranges; optional 11m/CB band with all-40-channel panadapter markers.
* **GEN / TIME / Memory** — three general-coverage slots, an HF
  time-station cycle (WWV/WWVH/CHU/BPM/RWM/…, ordered by your callsign),
  and a 20-slot frequency memory bank with CSV import/export.
* **EiBi shortwave overlay** — the EiBi broadcast schedule on the
  panadapter, on-air-now aware (cyan = live, grey = scheduled), click to
  tune in AM; in-app download or manual CSV import.
* **Band-plan overlay** — per-region (US / IARU R1 / R3) sub-band
  segments, band-edge warnings, digital + NCDXF beacon markers with
  click-to-tune and live beacon-station tooltips; out-of-band advisory.
* **TCI server** — TCI v1.9/2.0 (Expert Electronics) over WebSocket:
  drive frequency / mode / filter / volume from SDRLogger+, Log4OM,
  N1MM, etc.; a header indicator shows connected-client count; optional
  audio + IQ binary streaming and Thetis-style emulation/option toggles.
* **DX-cluster spots** — TCI cluster spots painted on the panadapter
  (country-tagged, CW-pitch-aware placement), with show/max/lifetime
  controls, own-callsign highlight + color, a toast when you're spotted
  (on/off + re-notify cooldown), and a Clear button.
* **Solar / propagation panel** — HamQSL SFI/A/K + 10-band day/night
  heat-map, plus NCDXF beacon auto-follow.
* **Weather alerts** — lightning / wind / severe-storm badges from
  Blitzortung / NWS / Ambient / Ecowitt.
* **Quality-of-life** — header clocks with NTP drift check, GitHub
  update notifications, in-app diagnostic log viewer (no console window),
  operator/station identity + band-plan region in Settings.

Every feature landed gated by an operator HL2 bench test.

## Download & run (Windows)

Just want to operate? You don't need to build anything.

1. Go to the **[Releases page](https://github.com/N8SDR1/Lyra-SDR-cpp/releases)**.
2. Download the latest **`Lyra-Setup-X.Y.Z.exe`** installer and run it.
3. Launch Lyra. First start does a one-time FFT optimization (cached per
   machine), then auto-discovers your HL2 / HL2+ on the network.

Requires Windows 10 (1809+) or 11, 64-bit. The build-from-source
instructions below are only needed if you want to develop or build it
yourself.

## Getting help & reporting bugs

Lyra is in active development — testers and bug reports are welcome.
See **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)** first for current rough edges and
what hasn't been built yet (so you don't report a planned feature as a bug).

* **Discord:** <https://discord.gg/nbJEqvFQ> — questions, feature ideas,
  and quick help.
* **Bug reports:** open a
  **[GitHub issue](https://github.com/N8SDR1/Lyra-SDR-cpp/issues)** (or post
  in Discord). To make a report actionable, please include:
  * Lyra version (Help → About, or the installer filename),
  * Windows version, and whether your radio is an **HL2 or HL2+**,
  * what you did and what happened (steps to reproduce),
  * the in-app **diagnostic log** (in Lyra, open the diagnostic log viewer
    and copy the relevant lines — no console window needed).

## Prerequisites (Windows)

* **Visual Studio 2022 or 2026 Community** with the "Desktop development
  with C++" workload + **MSVC v143** toolchain + Windows 11 SDK + CMake.
* **Qt 6.11.1** installed at `C:\Qt\6.11.1\` with the **MSVC 2022 64-bit**
  binding (add via Qt's `MaintenanceTool.exe` if the curated installer
  only pulled MinGW).

## Build (Windows command-line)

From an **x64 Native Tools Command Prompt for VS 2026**:

```bat
cd Y:\Claude local\SDRProject\lyra-cpp
cmake -B build -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build
build\lyra.exe
```

Or **Visual Studio 2026**: File -> Open -> CMake -> point at `CMakeLists.txt`
in this directory.  VS auto-configures, then F5 to build + run.

Or **Qt Creator**: File -> Open File or Project -> pick `CMakeLists.txt`,
let it configure, hit the green Run button.

## License

GPL v3 or later — matches WDSP, openHPSDR Thetis, and the parent
[Lyra](../lyra) Python project.  See [LICENSE](LICENSE) for the full
license text, [NOTICE.md](NOTICE.md) for upstream-project attribution
(WDSP + Thetis), and [CREDITS.md](CREDITS.md) for the full
acknowledgments and contributor list.

### Why this matters

Two open-source projects make Lyra-cpp possible without years of
ground-up implementation:

- **[WDSP](https://github.com/TAPR/OpenHPSDR-wdsp)** — Dr. Warren
  Pratt NR0V's DSP engine.  RX audio chain (NR, AGC, ANF, LMS, NB,
  AEPF, bandpass, demod, panel pan), TX audio chain (ALC, leveler,
  compressor, CFC, PHROT), and the planned PureSignal port (calcc +
  iqc).  Lyra-cpp links the bundled WDSP DLL and calls into
  it from native C++; the bundled DLL itself carries WDSP's GPL v3+
  license and copyright.

- **[openHPSDR Thetis (MI0BOT fork)](https://github.com/mi0bot/OpenHPSDR-Thetis)**
  — the HL2-focused fork of Richie Samphire (ramdor)'s
  [upstream Thetis](https://github.com/ramdor/Thetis), maintained by
  Reid (MI0BOT / GI8TME) with the HL2/HL2+ specific work that makes
  the Hermes Lite 2 family first-class.  Lyra-cpp's TX baseline ports
  the Thetis ChannelMaster layer (cmaster.c + aamix.c + ilv.c +
  supporting modules) with full per-file GPL attribution.  This
  brings tested, debugged, multi-radio (HL2/HL2+/ANAN/Orion/...) TX
  dispatch into Lyra-cpp; Lyra-native TX DSP enhancements layer on
  top.

The Python Lyra (`../lyra/`) and Lyra-cpp are both N8SDR's projects;
Lyra-cpp is the C++23 rebuild that makes hard-realtime audio + wire
I/O practical on Windows / macOS / Linux without Python's GIL.

Cross-platform support (macOS / Linux) is planned once the transmit
feature set settles; Qt 6 + modern C++ stdlib handles most of the cross-platform
lifting, with platform-shim modules for Win32-specific items (MMCSS
thread priority, WSAEventSelect → epoll/kqueue, etc.).
