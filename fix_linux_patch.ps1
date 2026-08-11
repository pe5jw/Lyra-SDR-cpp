# fix_linux_patch.ps1 -- past de 3 gemiste patches toe (CRLF-safe)
# Draai vanuit C:\project\lyra\Lyra-SDR-cpp
# powershell -ExecutionPolicy Bypass -File .\fix_linux_patch.ps1 -Token ghp_xxx

param([string]$Token = "")

$ErrorActionPreference = "Stop"
function Write-Ok  { param($m) Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Already { param($m) Write-Host "  [--] $m (al gepatcht)" -ForegroundColor Yellow }
function Write-Miss { param($m) Write-Host "  [!!] $m -- niet gevonden" -ForegroundColor Red }

function Patch-File {
    param($path, $old, $new, $label)
    $bytes   = [System.IO.File]::ReadAllBytes($path)
    $content = [System.Text.Encoding]::UTF8.GetString($bytes)
    # Normalize CRLF -> LF for matching
    $norm    = $content -replace "`r`n", "`n"
    $oldN    = $old     -replace "`r`n", "`n"
    $newN    = $new     -replace "`r`n", "`n"
    if ($norm.Contains($oldN)) {
        $patched = $norm.Replace($oldN, $newN)
        [System.IO.File]::WriteAllText($path, $patched, [System.Text.Encoding]::UTF8)
        Write-Ok $label
    } elseif ($norm.Contains($newN)) {
        Write-Already $label
    } else {
        Write-Miss $label
    }
}

$repoDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $repoDir

Write-Host ""
Write-Host "=== Linux compat patches toepassen ===" -ForegroundColor Cyan
Write-Host ""

#  src/main.cpp 
Patch-File "src\main.cpp" `
"#ifdef _WIN32`n#ifndef WIN32_LEAN_AND_MEAN`n#define WIN32_LEAN_AND_MEAN`n#endif`n#include <winsock2.h>`n#endif" `
"#ifdef _WIN32`n#  ifndef WIN32_LEAN_AND_MEAN`n#    define WIN32_LEAN_AND_MEAN`n#  endif`n#  include <winsock2.h>`n#else`n#  include ""compat/win32_compat.h""`n#endif" `
"src/main.cpp"

#  src/wdsp_native.cpp 
Patch-File "src\wdsp_native.cpp" `
"#ifndef NOMINMAX`n#define NOMINMAX`n#endif`n#ifndef WIN32_LEAN_AND_MEAN`n#define WIN32_LEAN_AND_MEAN`n#endif`n#include <windows.h>" `
"// pe5jw linux-compat`n#ifdef _WIN32`n#  ifndef NOMINMAX`n#    define NOMINMAX`n#  endif`n#  ifndef WIN32_LEAN_AND_MEAN`n#    define WIN32_LEAN_AND_MEAN`n#  endif`n#  include <windows.h>`n#else`n#  include ""compat/win32_compat.h""`n#endif" `
"src/wdsp_native.cpp"

#  CMakeLists.txt 
Patch-File "CMakeLists.txt" `
"    target_link_libraries(lyra PRIVATE ws2_32 winmm iphlpapi)`nendif()`n`n# Step 3a: bundle the WDSP DSP engine DLLs" `
"    target_link_libraries(lyra PRIVATE ws2_32 winmm iphlpapi)`nendif()`n`n# pe5jw linux-compat`nif(NOT WIN32)`n    target_include_directories(lyra PRIVATE`n        ""`${CMAKE_CURRENT_SOURCE_DIR}/compat"")`n    find_package(Threads REQUIRED)`n    target_link_libraries(lyra PRIVATE Threads::Threads dl)`n    set_target_properties(lyra PROPERTIES`n        INSTALL_RPATH ""`$ORIGIN/_native""`n        BUILD_RPATH   ""`$ORIGIN/_native"")`nendif()`nif(UNIX AND NOT APPLE)`n    set(_native_linux ""`${CMAKE_CURRENT_SOURCE_DIR}/_native_linux"")`n    if(EXISTS ""`${_native_linux}"")`n        add_custom_command(TARGET lyra POST_BUILD`n            COMMAND `${CMAKE_COMMAND} -E copy_directory`n                ""`${_native_linux}"" ""`$<TARGET_FILE_DIR:lyra>/_native""`n            COMMENT ""Bundling Linux WDSP .so files"" VERBATIM)`n    endif()`nendif()`n`n# Step 3a: bundle the WDSP DSP engine DLLs" `
"CMakeLists.txt"

#  Committen en pushen 
Write-Host ""
Write-Host "=== Committen en pushen ===" -ForegroundColor Cyan

git add src\main.cpp src\wdsp_native.cpp CMakeLists.txt compat\ build_lyra_linux.sh 2>$null
$status = git status --porcelain 2>$null
if ($status) {
    git commit -m "Linux/macOS compat layer (pe5jw)

Thin Win32->POSIX shim headers for Linux/macOS builds.
Windows builds completely unaffected.

New: compat/ (5 headers) + build_lyra_linux.sh
Modified: src/main.cpp, src/wdsp_native.cpp, CMakeLists.txt"
    Write-Ok "Gecommit"
} else {
    Write-Host "  Niets te committen" -ForegroundColor Yellow
}

git push origin main 2>&1 | ForEach-Object { Write-Host "  $_" -ForegroundColor Gray }
Write-Ok "Gepusht"

#  GitHub release 
if ($Token -eq "") {
    $Token = Read-Host "`nGitHub token voor release (Enter = overslaan)"
}

if ($Token -ne "") {
    Write-Host ""
    Write-Host "=== GitHub release aanmaken ===" -ForegroundColor Cyan

    $headers = @{
        "Authorization"        = "Bearer $Token"
        "Accept"               = "application/vnd.github+json"
        "X-GitHub-Api-Version" = "2022-11-28"
    }

    $cmake = Get-Content "CMakeLists.txt" -Raw
    $version = "0.21.1"
    if ($cmake -match 'project\([^)]*VERSION\s+(\d+\.\d+\.\d+)') { $version = $Matches[1] }
    $tag = "v$version"

    # Verwijder bestaande release/tag indien aanwezig
    try {
        $ex = Invoke-RestMethod "https://api.github.com/repos/pe5jw/Lyra-SDR-cpp/releases/tags/$tag" `
            -Headers $headers -ErrorAction SilentlyContinue
        Invoke-RestMethod "https://api.github.com/repos/pe5jw/Lyra-SDR-cpp/releases/$($ex.id)" `
            -Method Delete -Headers $headers | Out-Null
        try { Invoke-RestMethod "https://api.github.com/repos/pe5jw/Lyra-SDR-cpp/git/refs/tags/$tag" `
            -Method Delete -Headers $headers | Out-Null } catch {}
        Write-Host "  Bestaande release verwijderd" -ForegroundColor Yellow
    } catch {}

    $notes = @"
## Lyra SDR $tag -- pe5jw fork

### Wijzigingen

#### Linux/macOS compat layer
Dunne Win32->POSIX shim headers in compat/ map.
Windows builds volledig ongewijzigd.

Bouwen op Linux (Ubuntu/Fedora/Arch):
    bash build_lyra_linux.sh

#### TCI mic-source auto-restore
Settings -> TX -> Audio + Gain:
  [ ] Auto-restore mic source after TCI PTT release

### Bouwen op Windows
    powershell -ExecutionPolicy Bypass -File build_lyra.ps1
"@

    $body = @{
        tag_name = $tag; target_commitish = "main"
        name = "Lyra SDR $tag"; body = $notes
        draft = $false; prerelease = $false
    } | ConvertTo-Json -Depth 5

    $rel = Invoke-RestMethod "https://api.github.com/repos/pe5jw/Lyra-SDR-cpp/releases" `
        -Method Post -Headers $headers -ContentType "application/json" -Body $body

    Write-Ok "Release: $($rel.html_url)"
    $ans = Read-Host "Openen in browser? (j/n)"
    if ($ans -match "^[jJyY]$") { Start-Process $rel.html_url }
}

Write-Host ""
Write-Host "=== Klaar ===" -ForegroundColor Green
Pop-Location
