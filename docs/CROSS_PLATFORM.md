# Cross-platform builds (macOS / Linux) — porting notes

Status: **planning note, not yet implemented.** Lyra ships Windows-only
through v0.2.1. This captures what it takes to build for macOS and Linux so
the plan isn't lost when TX work lands.

**Recommendation: do the port AFTER TX/RX is stable on Windows.** TX work
touches `hl2_stream.cpp` heavily (the same file that carries the
Windows-specific socket/timer code below), so porting now would mean
maintaining a 3-OS abstraction while the protocol layer is still moving.

---

## Why it's feasible

The app is **C++23 + Qt6 + CMake** — inherently cross-platform. `CMakeLists.txt`
already guards the Windows bits behind `if(WIN32)`.

Qt's **RHI** auto-selects the graphics backend per OS, so the panadapter /
waterfall Quick scene needs **no code changes**:

| OS      | RHI backend            |
|---------|------------------------|
| Windows | Direct3D 11/12 → OpenGL fallback |
| macOS   | Metal                  |
| Linux   | Vulkan → OpenGL fallback |

### Already portable (no work)
- All C++/QML UI, panels, meters, bands, TCI server
- `QSettings` — adapts automatically (registry → plist → INI)
- Paths — `QStandardPaths` is per-OS already
- `hl2_discovery.cpp` — uses `QUdpSocket`
- FFTW / rnnoise / specbleach — portable C (apt / Homebrew / build)

---

## The work (4 items)

### 1. WDSP native engine — the biggest external chunk
`build/_native/` ships **Windows binaries**: `wdsp.dll`, `libfftw3-3.dll`,
`libfftw3f-3.dll`, `rnnoise.dll`, `specbleach.dll`. WDSP is GPL C source
(Warren Pratt NR0V); it must be **compiled per-OS** into `.so` (Linux) /
`.dylib` (macOS). FFTW/rnnoise/specbleach come from package managers or
build from source. macOS should target a universal (arm64 + x86_64) build.

### 2. `hl2_stream.cpp` — the hot wire path (real code work)
Deliberately uses raw Windows APIs for tight EP2/EP6 cadence:
- **WinSock2** (`winsock2.h`, `WSA*`) → POSIX sockets (`<sys/socket.h>`)
- **`CreateWaitableTimerExW`** (timer-paced EP2 writer) → `timerfd` /
  `clock_nanosleep` (Linux), dispatch/mach timers (macOS)
- **`timeBeginPeriod(1)`** (winmm, 1 ms scheduler tick) → not needed / use
  the high-res timer directly on Unix

Cleanest approach: a thin `udp_socket` + `wire_timer` shim with `#ifdef _WIN32`
branches, leaving the wire/cadence logic untouched. This shim is actually
*cleaner* than the current Windows-only path.

### 3. `wdsp_native.cpp`
`LoadLibrary`/`GetProcAddress` → `dlopen`/`dlsym` (a few lines, `#ifdef`).

### 4. `usb_bcd.cpp` (FTDI external-amp BCD control)
Uses FTDI D2XX (`windows.h`). FTDI ships Mac/Linux `libftd2xx`, OR `#ifdef`-stub
it on those platforms first (most testers don't use external-amp BCD).

### CMake
Mirror the existing `if(WIN32)` blocks with `if(APPLE)` / `if(UNIX)`:
drop `ws2_32`/`winmm`/`d3d*`/`mpr`/`userenv`/`src/lyra.rc`/`WIN32_EXECUTABLE`
on non-Windows; `find_library`/`pkg_check_modules` the native libs per OS.

---

## Packaging

| OS      | Deploy tool              | Artifact            | Notes |
|---------|--------------------------|---------------------|-------|
| Windows | `windeployqt` + Inno Setup | `Lyra-Setup-X.Y.Z.exe` | current |
| macOS   | `macdeployqt`            | `.app` → `.dmg`     | Developer-ID codesign + notarize to avoid Gatekeeper warnings |
| Linux   | `linuxdeploy --plugin qt`| **AppImage** (most portable), or `.deb`/Flatpak | |

---

## Building without owning the hardware

You can't build a Mac without a Mac — but you don't need to own one:

- **GitHub Actions matrix** (`windows-latest` + `ubuntu-latest` + `macos-latest`)
  builds all three on every `v*` tag push and attaches artifacts to the
  Release automatically. Use `jurplel/install-qt-action` for Qt.
- **Linux** can also be built locally in **WSL2** for quick iteration.

This is the scalable path: tag → CI builds 3 platforms → assets land on the
GitHub Release.

---

## When the time comes, two deliverables
1. **Portability audit** — punch-list of every Windows API call + effort estimate.
2. **GitHub Actions build matrix** — Mac/Linux binaries out of CI, no hardware needed.
