# Lyra — C++23 / Qt 6 Rebuild

Native rebuild of [Lyra](../lyra) (the Python+Qt6 desktop SDR transceiver
for the Hermes Lite 2 / 2+).  The Python tree is preserved in `../lyra/`
as the protocol research + doc archive reference; this is a clean ground-
up rewrite using the architecture the project should have started with.

## Architecture

| Layer          | Choice                                                           |
|----------------|------------------------------------------------------------------|
| Language       | C++23                                                            |
| UI framework   | Qt 6 (Qt Quick / QML)                                            |
| Graphics       | Qt RHI (Vulkan/D3D12 on Windows, Metal on macOS, OpenGL fallback) |
| DSP            | WDSP DSP engine, loaded natively (GPL v3+)                       |
| FFT            | FFTW3 (per-host wisdom-cached on first launch)                   |
| Wire I/O       | Native UDP (`QUdpSocket`) on dedicated OS threads — no GIL       |
| Threading      | `std::jthread` + Qt thread pools, OS-priority + MMCSS later      |
| Build          | CMake 3.21+                                                      |
| Compiler       | MSVC v143 (VS 2022/2026) on Windows; clang/gcc on Linux; Apple Clang on macOS |
| License        | GPL v3+                                                          |

**No Python. No GIL. No cffi. No in-process bottleneck on the wire path.**

## Features (v0.1.4)

A working receive-side SDR for the Hermes Lite 2 / 2+, native C++ end to
end.  (Transmit is on the roadmap; the current build is RX-focused.)

* **Radio** — HPSDR Protocol 1 discovery (multi-NIC) + live RX off the
  HL2/HL2+ on dedicated OS threads; auto-connect to the last radio.
* **DSP** — WDSP RX chain: USB/LSB/CW/AM/FM/DIG modes, per-mode filters,
  AGC, NR, auto-notch, manual notches, squelch.  Audio out the HL2 jack
  (AK4951) or PC sound card.
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
* **Solar / propagation panel** — HamQSL SFI/A/K + 10-band day/night
  heat-map, plus NCDXF beacon auto-follow.
* **Weather alerts** — lightning / wind / severe-storm badges from
  Blitzortung / NWS / Ambient / Ecowitt.
* **Quality-of-life** — header clocks with NTP drift check, GitHub
  update notifications, in-app diagnostic log viewer (no console window),
  operator/station identity + band-plan region in Settings.

Every feature landed gated by an operator HL2 bench test.

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

GPL v3+, matches the parent `Lyra` Python project (`../lyra/`) and WDSP
(`../lyra/dsp/_native/`, when integrated in a later commit).  See
`../LICENSE` and `../NOTICE.md`.
