# update_readme.ps1 -- README_PE5JW.md toevoegen en pushen
# Draai vanuit C:\project\lyra\Lyra-SDR-cpp
# powershell -ExecutionPolicy Bypass -File .\update_readme.ps1

$ErrorActionPreference = "Continue"
$repoDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $repoDir

Write-Host ""
Write-Host "=== README_PE5JW.md toevoegen ===" -ForegroundColor Cyan

# Schrijf README_PE5JW.md direct naar de repo
$readme = @'
# pe5jw fork -- Lyra-SDR-cpp

> **Fork van [N8SDR1/Lyra-SDR-cpp](https://github.com/N8SDR1/Lyra-SDR-cpp)**
> door PE5JW -- aanvullende functies, Windows build scripts, Linux
> compatibility layer en portable releases.

---

## Wijzigingen ten opzichte van upstream

### 1. TCI mic-source auto-restore na PTT release

**Probleem:** Bij gebruik van PC Soundcard (VAC1) als mic source en een
TCI client (WSJT-X, MSHV, SDRLogger+) voor digitale transmissie,
schakelt Lyra automatisch naar "TCI (digital modes)". Na PTT release
blijft de picker op TCI staan -- microfoon werkt niet meer voor SSB.

**Oplossing:** Nieuwe optie in Settings -> TX -> Audio + Gain:

    [ ] Auto-restore mic source after TCI PTT release

Wanneer ingeschakeld: Lyra onthoudt de actieve mic source voor de TCI
transmissie en schakelt automatisch terug na PTT release.
Standaard UIT -- dedicated digitale setups blijven ongewijzigd werken.

**Gewijzigde bestanden:**
- `src/prefs.h` / `src/prefs.cpp`
- `src/tci_server.h` / `src/tci_server.cpp`
- `src/settingsdialog.cpp`

---

### 2. Linux / macOS compatibility layer

Dunne Win32->POSIX shim headers in `compat/` zodat Lyra op Linux
gebouwd kan worden. Windows builds zijn volledig ongewijzigd.

| Bestand | Functie |
|---|---|
| `compat/win32_compat.h` | Master include |
| `compat/win32_socket.h` | WinSock2 -> POSIX sockets |
| `compat/win32_timer.h` | CreateWaitableTimerEx -> timerfd |
| `compat/win32_dynload.h` | LoadLibrary -> dlopen/dlsym |
| `compat/win32_stubs.h` | Diverse stubs |

---

## Bouwen op Windows

### Automatisch (aanbevolen)

```powershell
# Stap 1: vereisten controleren en installeren
powershell -ExecutionPolicy Bypass -File check_lyra_prereqs.ps1

# Stap 2: klonen en bouwen
powershell -ExecutionPolicy Bypass -File build_lyra.ps1

# Stap 3: portable ZIP maken
powershell -ExecutionPolicy Bypass -File make_portable.ps1
```

### Vereisten

- Visual Studio 2022/2026 Community met "Desktop development with C++" workload
- Qt 6.11.1 MSVC 2022 64-bit via Qt MaintenanceTool
- Git, CMake 3.21+, Ninja

### Build scripts

| Script | Functie |
|---|---|
| `check_lyra_prereqs.ps1` | Controleert en installeert alle vereisten |
| `build_lyra.ps1` | Clone + CMake configure + build |
| `make_portable.ps1` | Portable ZIP maken van de build |
| `sync_upstream.ps1` | Fork synchroniseren met N8SDR1 upstream |
| `github_release.ps1` | GitHub release aanmaken via API |

---

## Bouwen op Linux

```bash
# Ubuntu 24.04 / Fedora / Arch -- volledig automatisch
bash build_lyra_linux.sh
```

Het script installeert automatisch build tools, Qt6, FFTW3, RNNoise,
kloont de repo, configureert en buildt Lyra.

### WDSP bouwen voor Linux

```bash
sudo apt install libfftw3-dev
git clone https://github.com/g0orx/wdsp
cd wdsp
cmake -B build -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
mkdir -p /pad/naar/Lyra-SDR-cpp/_native_linux
cp build/libwdsp.so /pad/naar/Lyra-SDR-cpp/_native_linux/
```

---

## Portable release

De portable release is een ZIP zonder installatie.
Download via de [Releases pagina](https://github.com/pe5jw/Lyra-SDR-cpp/releases).

```
lyra-vX.Y.Z-win64-portable/
  lyra.exe
  _native/     WDSP + FFTW + RNNoise DLLs
  data/        DX prefixen, bandplannen, EiBi
  platforms/   Qt platform plugins
  *.dll        Qt6 runtime
  README.txt
```

---

## Synchroniseren met upstream

```powershell
powershell -ExecutionPolicy Bypass -File sync_upstream.ps1
```

---

## Releases

| Versie | Datum | Wijzigingen |
|---|---|---|
| v0.21.1 | 2026-08-05 | Linux compat layer + build scripts |
| v0.21.0 | 2026-08-05 | TCI mic-source auto-restore |

---

## Links

- **Upstream:** [N8SDR1/Lyra-SDR-cpp](https://github.com/N8SDR1/Lyra-SDR-cpp)
- **Discord:** [discord.gg/BwjsQvjcSc](https://discord.gg/BwjsQvjcSc)
- **Hermes Lite 2:** [hermeslite.com](http://hermeslite.com)
'@

[System.IO.File]::WriteAllText(
    (Join-Path $repoDir "README_PE5JW.md"),
    $readme,
    [System.Text.Encoding]::UTF8
)
Write-Host "  [OK] README_PE5JW.md geschreven" -ForegroundColor Green

git add README_PE5JW.md
$status = git status --porcelain 2>$null
if ($status) {
    git commit -m "Add README_PE5JW.md -- pe5jw fork documentatie"
    Write-Host "  [OK] Gecommit" -ForegroundColor Green
    git push origin main 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor Gray }
    Write-Host "  [OK] Gepusht" -ForegroundColor Green
} else {
    Write-Host "  [--] Niets te committen" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Klaar! README_PE5JW.md staat nu op GitHub:" -ForegroundColor Green
Write-Host "https://github.com/pe5jw/Lyra-SDR-cpp/blob/main/README_PE5JW.md"
Write-Host ""
Pop-Location
