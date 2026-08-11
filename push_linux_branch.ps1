# Lyra Linux compat branch pushen + nieuwe release aanmaken
# Draai vanuit C:\project\lyra\Lyra-SDR-cpp
# powershell -ExecutionPolicy Bypass -File .\push_linux_branch.ps1 -Token ghp_xxx

param(
    [string]$Token      = "",
    [string]$RepoOwner  = "pe5jw",
    [string]$RepoName   = "Lyra-SDR-cpp"
)

$ErrorActionPreference = "Stop"
function Write-Step { param($m) Write-Host "" ; Write-Host "--> $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Err  { param($m) Write-Host "  [XX] $m" -ForegroundColor Red ; exit 1 }
function Write-Info { param($m) Write-Host "       $m" -ForegroundColor Gray }

Write-Host ""
Write-Host "===================================================" -ForegroundColor Magenta
Write-Host "   Lyra Linux compat -- branch pushen + release   " -ForegroundColor Magenta
Write-Host "===================================================" -ForegroundColor Magenta

$repoDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Test-Path (Join-Path $repoDir ".git"))) {
    Write-Err "Draai vanuit C:\project\lyra\Lyra-SDR-cpp"
}
Push-Location $repoDir

# Token ophalen
if ($Token -eq "") {
    $Token = Read-Host "  GitHub token (ghp_...)"
    if ($Token -eq "") { Write-Err "Geen token" }
}
$headers = @{
    "Authorization"        = "Bearer $Token"
    "Accept"               = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
}

# Stap 1: apply_patch.ps1 uitvoeren (tci-mic-restore patch)
Write-Step "TCI mic-restore patch controleren"
$applyPatch = Join-Path (Split-Path $repoDir -Parent) "apply_patch.ps1"
if (Test-Path $applyPatch) {
    $hasPatch = Select-String -Path "src\prefs.h" -Pattern "tciRestoreMicSource" -Quiet
    if (-not $hasPatch) {
        Write-Info "Patch nog niet toegepast -- apply_patch.ps1 uitvoeren..."
        powershell -ExecutionPolicy Bypass -File $applyPatch
        Write-Ok "TCI patch toegepast"
    } else {
        Write-Ok "TCI patch al aanwezig"
    }
} else {
    Write-Info "apply_patch.ps1 niet gevonden -- TCI patch overgeslagen"
}

# Stap 2: compat/ map aanmaken + bestanden schrijven
Write-Step "Linux compat bestanden aanmaken"
$compatDir = Join-Path $repoDir "compat"
New-Item -ItemType Directory -Path $compatDir -Force | Out-Null

# win32_compat.h
@"
// compat/win32_compat.h -- master Linux/macOS compatibility header
// On Windows this header does nothing.
// On Linux/macOS the shims replace Win32 APIs with POSIX equivalents.
#pragma once
#ifndef _WIN32
#  include "win32_socket.h"
#  include "win32_timer.h"
#  include "win32_dynload.h"
#  include "win32_stubs.h"
#endif
"@ | Out-File (Join-Path $compatDir "win32_compat.h") -Encoding utf8
Write-Ok "compat/win32_compat.h"

# win32_socket.h
@"
// compat/win32_socket.h -- WinSock2 -> POSIX sockets shim
#pragma once
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>
using SOCKET  = int;
using DWORD   = unsigned long;
using WORD    = unsigned short;
using BOOL    = int;
using HANDLE  = void*;
using HMODULE = void*;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int    SOCKET_ERROR   = -1;
inline int closesocket(SOCKET s) { return ::close(s); }
inline int ioctlsocket(SOCKET, long, unsigned long*) { return 0; }
struct WSAData { int dummy; };
inline int WSAStartup(WORD, WSAData*) { return 0; }
inline int WSACleanup()              { return 0; }
inline int WSAGetLastError()         { return errno; }
inline DWORD FormatMessageW(DWORD, void*, DWORD code, DWORD,
                             wchar_t* buf, DWORD size, void*) {
    std::string msg = std::strerror((int)code);
    std::mbstowcs(buf, msg.c_str(), size);
    return (DWORD)msg.size();
}
struct MIB_UDPSTATS { DWORD dwInDatagrams; DWORD dwNoBufRecv; DWORD dwOutDatagrams; };
inline int GetUdpStatisticsEx(MIB_UDPSTATS* s, int) { if (s) *s = {}; return 0; }
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE SO_REUSEADDR
#endif
#endif // !_WIN32
"@ | Out-File (Join-Path $compatDir "win32_socket.h") -Encoding utf8
Write-Ok "compat/win32_socket.h"

# win32_dynload.h
@"
// compat/win32_dynload.h -- LoadLibrary/GetProcAddress -> dlopen/dlsym
#pragma once
#ifndef _WIN32
#include <dlfcn.h>
using HMODULE = void*;
using FARPROC = void(*)();
inline HMODULE LoadLibraryExW(const wchar_t* p, void*, unsigned long) {
    char buf[4096]{}; std::wcstombs(buf, p, sizeof(buf)-1);
    return ::dlopen(buf, RTLD_LAZY | RTLD_LOCAL);
}
inline HMODULE LoadLibraryW(const wchar_t* p) { return LoadLibraryExW(p,nullptr,0); }
inline FARPROC GetProcAddress(HMODULE h, const char* n) {
    return reinterpret_cast<FARPROC>(::dlsym(h, n));
}
inline int FreeLibrary(HMODULE h) { return (::dlclose(h) == 0) ? 1 : 0; }
using DLL_DIRECTORY_COOKIE = void*;
inline DLL_DIRECTORY_COOKIE AddDllDirectory(const wchar_t*) { return nullptr; }
inline int RemoveDllDirectory(DLL_DIRECTORY_COOKIE) { return 1; }
constexpr unsigned long LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x1000;
#endif // !_WIN32
"@ | Out-File (Join-Path $compatDir "win32_dynload.h") -Encoding utf8
Write-Ok "compat/win32_dynload.h"

# win32_timer.h
@"
// compat/win32_timer.h -- CreateWaitableTimerEx -> timerfd + pthread shim
#pragma once
#ifndef _WIN32
#include <sys/timerfd.h>
#include <sys/select.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
constexpr int CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0;
constexpr unsigned long INFINITE = 0xFFFFFFFF;
union LARGE_INTEGER {
    struct { unsigned long LowPart; long HighPart; };
    long long QuadPart;
};
inline void* CreateWaitableTimerExW(void*,void*,int,int) {
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
    return reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}
inline int SetWaitableTimerEx(void* h, const LARGE_INTEGER* due,
                               long period_ms, void*,void*,void*,unsigned long) {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(h));
    long long due_ns = (due && due->QuadPart < 0) ? (-due->QuadPart * 100LL)
                                                    : (period_ms * 1000000LL);
    long period_ns = period_ms * 1000000L;
    struct itimerspec ts{};
    ts.it_value.tv_sec    = due_ns / 1000000000LL;
    ts.it_value.tv_nsec   = due_ns % 1000000000LL;
    ts.it_interval.tv_sec = period_ns / 1000000000L;
    ts.it_interval.tv_nsec= period_ns % 1000000000L;
    return (::timerfd_settime(fd, 0, &ts, nullptr) == 0) ? 1 : 0;
}
inline unsigned long WaitForSingleObject(void* h, unsigned long timeout_ms) {
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(h));
    fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
    struct timeval tv{}, *tvp = nullptr;
    if (timeout_ms != 0xFFFFFFFF) {
        tv.tv_sec = timeout_ms/1000; tv.tv_usec = (timeout_ms%1000)*1000; tvp=&tv;
    }
    int r = ::select(fd+1, &rfds, nullptr, nullptr, tvp);
    if (r > 0) { uint64_t e; ::read(fd,&e,8); return 0; }
    return (r == 0) ? 0x00000102UL : 0xFFFFFFFFUL;
}
inline int CloseHandle(void* h) {
    return (::close(static_cast<int>(reinterpret_cast<intptr_t>(h)))==0)?1:0;
}
inline int timeBeginPeriod(unsigned int) { return 0; }
inline int timeEndPeriod(unsigned int)   { return 0; }
#ifndef __stdcall
#define __stdcall
#endif
struct _bte_args { void*(*fn)(void*); void* arg; };
inline void* _bte_run(void* p) {
    auto* a=static_cast<_bte_args*>(p); auto f=a->fn; auto g=a->arg;
    delete a; return f(g);
}
inline uintptr_t _beginthreadex(void*,unsigned,
    unsigned(__stdcall* fn)(void*), void* arg, unsigned, unsigned*) {
    pthread_t t;
    auto* a=new _bte_args{reinterpret_cast<void*(*)(void*)>(fn), arg};
    if (pthread_create(&t, nullptr, _bte_run, a)!=0){delete a;return 0;}
    pthread_detach(t); return static_cast<uintptr_t>(t);
}
#endif // !_WIN32
"@ | Out-File (Join-Path $compatDir "win32_timer.h") -Encoding utf8
Write-Ok "compat/win32_timer.h"

# win32_stubs.h
@"
// compat/win32_stubs.h -- misc Windows stubs for Linux/macOS
#pragma once
#ifndef _WIN32
#include <cstdint>
#include <chrono>
using UINT = unsigned int; using LONG = long; using ULONG = unsigned long;
constexpr int ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000;
constexpr int HIGH_PRIORITY_CLASS         = 0x00000080;
inline int  SetPriorityClass(void*, unsigned long) { return 1; }
inline void* GetCurrentProcess()                   { return nullptr; }
inline int  CoInitializeEx(void*, unsigned long)   { return 0; }
inline void CoUninitialize() {}
inline unsigned long timeGetTime() {
    using namespace std::chrono;
    return (unsigned long)(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count() & 0xFFFFFFFF);
}
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#endif // !_WIN32
"@ | Out-File (Join-Path $compatDir "win32_stubs.h") -Encoding utf8
Write-Ok "compat/win32_stubs.h"

# Stap 3: Bronbestanden patchen
Write-Step "Bronbestanden patchen voor Linux"

function Patch-Source {
    param($file, $old, $new, $label)
    $content = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)
    if ($content.Contains($old)) {
        [System.IO.File]::WriteAllText($file, $content.Replace($old, $new), [System.Text.Encoding]::UTF8)
        Write-Ok $label
    } elseif ($content.Contains($new)) {
        Write-Host "  [--] $label (al gepatcht)" -ForegroundColor Yellow
    } else {
        Write-Host "  [!!] $label -- niet gevonden" -ForegroundColor Red
    }
}

# main.cpp
Patch-Source "src\main.cpp" `
'#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif' `
'#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#else
#  include "compat/win32_compat.h"
#endif' `
'src/main.cpp'

# wdsp_native.cpp
Patch-Source "src\wdsp_native.cpp" `
'#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>' `
'// pe5jw linux-compat
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include "compat/win32_compat.h"
#endif' `
'src/wdsp_native.cpp'

# wdsp_native.cpp library name
Patch-Source "src\wdsp_native.cpp" `
'QStringLiteral("/wdsp.dll"));' `
'#ifdef _WIN32
                              QStringLiteral("/wdsp.dll"));
#else
                              QStringLiteral("/_native/libwdsp.so"));
#endif' `
'src/wdsp_native.cpp (libwdsp.so)'

# CMakeLists.txt
Patch-Source "CMakeLists.txt" `
'    target_link_libraries(lyra PRIVATE ws2_32 winmm iphlpapi)
endif()

# Step 3a: bundle the WDSP DSP engine DLLs' `
'    target_link_libraries(lyra PRIVATE ws2_32 winmm iphlpapi)
endif()

# pe5jw linux-compat
if(NOT WIN32)
    target_include_directories(lyra PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/compat")
    find_package(Threads REQUIRED)
    target_link_libraries(lyra PRIVATE Threads::Threads dl)
    set_target_properties(lyra PROPERTIES
        INSTALL_RPATH "$ORIGIN/_native"
        BUILD_RPATH   "$ORIGIN/_native")
endif()
if(UNIX AND NOT APPLE)
    set(_native_linux "${CMAKE_CURRENT_SOURCE_DIR}/_native_linux")
    if(EXISTS "${_native_linux}")
        add_custom_command(TARGET lyra POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_native_linux}" "$<TARGET_FILE_DIR:lyra>/_native"
            COMMENT "Bundling Linux WDSP .so files" VERBATIM)
    endif()
endif()

# Step 3a: bundle the WDSP DSP engine DLLs' `
'CMakeLists.txt'

# Stap 4: build_lyra_linux.sh toevoegen (download URL)
Write-Step "build_lyra_linux.sh controleren"
if (-not (Test-Path "build_lyra_linux.sh")) {
    Write-Host "  Download build_lyra_linux.sh van de Claude chat" -ForegroundColor Yellow
    Write-Host "  en kopieer het naar $repoDir" -ForegroundColor Yellow
} else {
    Write-Ok "build_lyra_linux.sh aanwezig"
}

# Stap 5: committen en pushen
Write-Step "Committen en pushen"
git add compat\ src\hl2_stream.cpp src\wdsp_native.cpp src\main.cpp CMakeLists.txt build_lyra_linux.sh 2>$null
git add compat\ src\main.cpp src\wdsp_native.cpp CMakeLists.txt 2>$null
$status = git status --porcelain 2>$null
if ($status) {
    git commit -m "Linux/macOS compat layer (pe5jw)

Adds thin Win32->POSIX shim headers so Lyra builds on Linux/macOS.
Windows builds completely unaffected.

New: compat/win32_compat.h, win32_socket.h, win32_timer.h,
     win32_dynload.h, win32_stubs.h, build_lyra_linux.sh
Modified: src/hl2_stream.cpp, src/wdsp_native.cpp, src/main.cpp,
          CMakeLists.txt" 2>$null
    Write-Ok "Gecommit"
} else {
    Write-Host "  [--] Niets te committen" -ForegroundColor Yellow
}

git push origin main 2>&1 | ForEach-Object { Write-Host "       $_" -ForegroundColor Gray }
Write-Ok "Gepusht naar pe5jw/Lyra-SDR-cpp"

# Stap 6: nieuwe release aanmaken
Write-Step "Nieuwe release aanmaken (v0.21.1)"

$cmakeContent = Get-Content "CMakeLists.txt" -Raw
$version = "0.21.1"
if ($cmakeContent -match 'project\([^)]*VERSION\s+(\d+\.\d+\.\d+)') {
    $version = $Matches[1]
}
$tagName = "v$version"

$releaseNotes = @"
## Lyra SDR $tagName -- pe5jw fork

### Wijzigingen

#### Linux/macOS compat layer
Dunne Win32->POSIX shim headers zodat Lyra op Linux gebouwd kan worden.
Windows builds zijn volledig ongewijzigd.

Nieuw: compat/ map met 5 headers + build_lyra_linux.sh build script.
Zie build_lyra_linux.sh voor automatische installatie op Ubuntu/Fedora/Arch.

#### TCI mic-source auto-restore na PTT release
Settings -> TX -> Audio + Gain:
  [ ] Auto-restore mic source after TCI PTT release

### Bouwen op Linux
    bash build_lyra_linux.sh

### Bouwen op Windows
    powershell -ExecutionPolicy Bypass -File build_lyra.ps1
"@

try {
    $existing = Invoke-RestMethod -Uri "https://api.github.com/repos/$RepoOwner/$RepoName/releases/tags/$tagName" `
        -Headers $headers -ErrorAction SilentlyContinue
    Invoke-RestMethod -Uri "https://api.github.com/repos/$RepoOwner/$RepoName/releases/$($existing.id)" `
        -Method Delete -Headers $headers | Out-Null
    try {
        Invoke-RestMethod -Uri "https://api.github.com/repos/$RepoOwner/$RepoName/git/refs/tags/$tagName" `
            -Method Delete -Headers $headers | Out-Null
    } catch {}
    Write-Host "  Bestaande release verwijderd" -ForegroundColor Yellow
} catch {}

$body = @{
    tag_name         = $tagName
    target_commitish = "main"
    name             = "Lyra SDR $tagName"
    body             = $releaseNotes
    draft            = $false
    prerelease       = $false
} | ConvertTo-Json -Depth 5

$release = Invoke-RestMethod `
    -Uri "https://api.github.com/repos/$RepoOwner/$RepoName/releases" `
    -Method Post -Headers $headers -ContentType "application/json" -Body $body

Write-Ok "Release: $($release.html_url)"

Write-Host ""
Write-Host "===================================================" -ForegroundColor Green
Write-Host "   KLAAR                                          " -ForegroundColor Green
Write-Host "===================================================" -ForegroundColor Green
Write-Host "  $($release.html_url)"
Write-Host ""

$ans = Read-Host "  Openen in browser? (j/n)"
if ($ans -match "^[jJyY]$") { Start-Process $release.html_url }

Pop-Location
