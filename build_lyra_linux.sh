#!/bin/bash
# Lyra-SDR-cpp -- Linux build script (pe5jw fork)
# Getest op: Ubuntu 24.04 LTS x86-64
# Gebruik: bash build_lyra_linux.sh [--branch feature/tci-mic-restore]
#
# Wat dit script doet:
#   1. Vereiste pakketten installeren
#   2. Qt 6.7+ ophalen (via aqtinstall of distro pakket)
#   3. WDSP en dependencies bouwen
#   4. Lyra klonen en bouwen
#   5. AppImage maken

set -e
BRANCH="${1:---branch}"
BRANCH_NAME="${2:-main}"
if [ "$BRANCH" != "--branch" ]; then BRANCH_NAME="$BRANCH" ; fi

REPO_URL="https://github.com/pe5jw/Lyra-SDR-cpp"
BUILD_ROOT="$HOME/lyra-build"
REPO_DIR="$BUILD_ROOT/Lyra-SDR-cpp"
WDSP_DIR="$BUILD_ROOT/wdsp"
NATIVE_DIR="$REPO_DIR/_native_linux"

RED='\033[0;31m' ; GREEN='\033[0;32m' ; CYAN='\033[0;36m'
YELLOW='\033[1;33m' ; NC='\033[0m'
step()  { echo -e "\n${CYAN}--> $1${NC}"; }
ok()    { echo -e "  ${GREEN}[OK]${NC} $1"; }
warn()  { echo -e "  ${YELLOW}[!!]${NC} $1"; }
fail()  { echo -e "\n${RED}[XX] $1${NC}"; exit 1; }

echo ""
echo "==================================================="
echo "   Lyra-SDR-cpp Linux build (pe5jw fork)"
echo "==================================================="
echo "  Branch  : $BRANCH_NAME"
echo "  Build   : $BUILD_ROOT"
echo ""

# -- Distro detectie -------------------------------------------------
if command -v apt-get &>/dev/null; then
    PKG_MANAGER="apt"
elif command -v dnf &>/dev/null; then
    PKG_MANAGER="dnf"
elif command -v pacman &>/dev/null; then
    PKG_MANAGER="pacman"
else
    warn "Onbekende distro -- handmatig pakketten installeren"
    PKG_MANAGER="unknown"
fi
ok "Distro: $PKG_MANAGER"

# -- Stap 1: System packages ------------------------------------------
step "System packages installeren"
case $PKG_MANAGER in
apt)
    sudo apt-get update -q
    sudo apt-get install -y \
        git cmake ninja-build pkg-config \
        build-essential g++ \
        libfftw3-dev librnnoise-dev \
        libasound2-dev libpulse-dev \
        libgl1-mesa-dev libvulkan-dev \
        libxcb-xinerama0 libxcb-icccm4 libxcb-image0 \
        libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 \
        libxcb-xkb1 libxkbcommon-x11-0 \
        python3-pip wget curl \
        patchelf fuse libfuse2
    ok "apt packages geinstalleerd"
    ;;
dnf)
    sudo dnf install -y \
        git cmake ninja-build pkgconfig \
        gcc-c++ \
        fftw-devel rnnoise-devel \
        alsa-lib-devel pulseaudio-libs-devel \
        mesa-libGL-devel vulkan-devel \
        python3-pip wget curl \
        patchelf fuse fuse-libs
    ok "dnf packages geinstalleerd"
    ;;
pacman)
    sudo pacman -Sy --noconfirm \
        git cmake ninja pkg-config \
        gcc \
        fftw \
        alsa-lib pulseaudio \
        mesa vulkan-headers \
        python-pip wget curl \
        patchelf fuse2
    ok "pacman packages geinstalleerd"
    ;;
esac

# -- Stap 2: Qt6 -----------------------------------------------------
step "Qt6 controleren / installeren"
QT_PREFIX=""

# Probeer eerst distro Qt6
if pkg-config --exists Qt6Core 2>/dev/null; then
    QT_VER=$(pkg-config --modversion Qt6Core)
    ok "Distro Qt6 gevonden: $QT_VER"
    QT_PREFIX=$(pkg-config --variable=prefix Qt6Core)
else
    warn "Distro Qt6 niet gevonden -- installeren via aqtinstall"
    pip3 install --user aqtinstall 2>/dev/null || true
    AQT="$HOME/.local/bin/aqt"
    if [ ! -f "$AQT" ]; then
        fail "aqtinstall niet beschikbaar. Installeer Qt6 handmatig:\n  sudo apt install qt6-base-dev qt6-multimedia-dev qt6-websockets-dev qt6-serialport-dev"
    fi

    QT_INSTALL="$HOME/Qt"
    QT_VER="6.7.3"
    if [ ! -d "$QT_INSTALL/$QT_VER/gcc_64" ]; then
        echo "  Qt $QT_VER downloaden (dit duurt enkele minuten)..."
        "$AQT" install-qt linux desktop "$QT_VER" gcc_64 \
            -m qtmultimedia qtwebsockets qtserialport qtshadertools \
            --outputdir "$QT_INSTALL"
    fi
    QT_PREFIX="$QT_INSTALL/$QT_VER/gcc_64"
    ok "Qt $QT_VER geinstalleerd op $QT_PREFIX"
fi

# -- Stap 3: WDSP bouwen ---------------------------------------------
step "WDSP DSP library bouwen"
mkdir -p "$WDSP_DIR" "$NATIVE_DIR"

if [ ! -f "$NATIVE_DIR/libwdsp.so" ]; then
    if [ ! -d "$WDSP_DIR/wdsp" ]; then
        echo "  WDSP broncode klonen..."
        git clone --depth=1 https://github.com/TAPR/OpenHPSDR-Thetis "$WDSP_DIR/thetis" 2>/dev/null || \
        git clone --depth=1 https://github.com/dl1ycf/wdsp "$WDSP_DIR/wdsp" 2>/dev/null || true
    fi

    # Probeer wdsp via cmake te bouwen als de source er is
    WDSP_SRC=$(find "$WDSP_DIR" -name "wdsp.h" -o -name "wdsp.cpp" 2>/dev/null | head -1 | xargs dirname 2>/dev/null || echo "")

    if [ -n "$WDSP_SRC" ]; then
        echo "  WDSP bouwen vanuit $WDSP_SRC..."
        cmake -B "$WDSP_DIR/build" -S "$WDSP_SRC" \
              -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DBUILD_SHARED_LIBS=ON \
              -DCMAKE_INSTALL_PREFIX="$WDSP_DIR/install"
        cmake --build "$WDSP_DIR/build" --parallel "$(nproc)"
        find "$WDSP_DIR/build" -name "*.so*" -exec cp {} "$NATIVE_DIR/" \;
        ok "WDSP gebouwd"
    else
        warn "WDSP broncode niet gevonden -- handmatig bouwen vereist"
        warn "Zie: https://github.com/g0orx/wdsp"
        warn "Of: sudo apt install libwdsp-dev (indien beschikbaar)"
        echo ""
        echo "  _native_linux/ map aangemaakt -- plaats hier libwdsp.so"
        echo "  Daarna dit script opnieuw draaien."
    fi
else
    ok "WDSP al aanwezig in _native_linux/"
fi

# Kopieer FFTW en RNNoise .so naar _native_linux
for lib in libfftw3-3 libfftw3f-3 librnnoise libspecbleach; do
    found=$(find /usr/lib /usr/local/lib -name "${lib}.so*" 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        cp -L "$found" "$NATIVE_DIR/" 2>/dev/null || true
        ok "$lib gekopieerd"
    fi
done

# -- Stap 4: Repo klonen / updaten -----------------------------------
step "Repository"
mkdir -p "$BUILD_ROOT"
if [ -d "$REPO_DIR/.git" ]; then
    warn "Repo bestaat al -- updaten"
    cd "$REPO_DIR"
    git fetch origin
    git checkout "$BRANCH_NAME"
    git pull origin "$BRANCH_NAME"
else
    git clone --branch "$BRANCH_NAME" --depth=1 "$REPO_URL" "$REPO_DIR"
fi
ok "Repository klaar: $REPO_DIR"

# -- Stap 5: CMake configure -----------------------------------------
step "CMake configureren"
cd "$REPO_DIR"

CMAKE_PREFIX=""
if [ -n "$QT_PREFIX" ]; then
    CMAKE_PREFIX="-DCMAKE_PREFIX_PATH=$QT_PREFIX"
fi

cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    $CMAKE_PREFIX \
    -DCMAKE_INSTALL_RPATH='$ORIGIN/_native' \
    -DCMAKE_BUILD_RPATH='$ORIGIN/_native'

ok "CMake configure klaar"

# -- Stap 6: Build ---------------------------------------------------
step "Bouwen"
cmake --build build --parallel "$(nproc)"
ok "Build geslaagd"

# -- Stap 7: Test starten --------------------------------------------
step "Resultaat"
EXE="$REPO_DIR/build/lyra"
if [ -f "$EXE" ]; then
    SIZE=$(du -sh "$EXE" | cut -f1)
    echo ""
    echo "==================================================="
    echo -e "   ${GREEN}BUILD GESLAAGD${NC}"
    echo "==================================================="
    echo "  Executable: $EXE ($SIZE)"
    echo ""
    echo "  Starten:"
    echo "    $EXE"
    echo ""
    read -p "  Nu starten? (j/n): " ans
    if [[ "$ans" =~ ^[jJyY]$ ]]; then
        "$EXE" &
    fi
else
    fail "lyra binary niet gevonden op $EXE"
fi

# -- Stap 8: AppImage (optioneel) ------------------------------------
step "AppImage maken (optioneel)"
if command -v linuxdeploy &>/dev/null || [ -f "$BUILD_ROOT/linuxdeploy-x86_64.AppImage" ]; then
    LDEPLOY="linuxdeploy"
    [ -f "$BUILD_ROOT/linuxdeploy-x86_64.AppImage" ] && LDEPLOY="$BUILD_ROOT/linuxdeploy-x86_64.AppImage"

    mkdir -p "$REPO_DIR/AppDir/usr/bin"
    cp "$EXE" "$REPO_DIR/AppDir/usr/bin/"
    cp -r "$REPO_DIR/build/_native" "$REPO_DIR/AppDir/usr/bin/" 2>/dev/null || true

    QMAKE_PATH=""
    [ -n "$QT_PREFIX" ] && QMAKE_PATH="--qmake=$QT_PREFIX/bin/qmake"

    "$LDEPLOY" --appdir "$REPO_DIR/AppDir" \
               --executable "$REPO_DIR/AppDir/usr/bin/lyra" \
               --plugin qt $QMAKE_PATH \
               --output appimage \
               --desktop-file "$REPO_DIR/data/lyra.desktop" 2>/dev/null || \
    "$LDEPLOY" --appdir "$REPO_DIR/AppDir" \
               --executable "$REPO_DIR/AppDir/usr/bin/lyra" \
               --plugin qt $QMAKE_PATH \
               --output appimage 2>/dev/null || true

    APPIMAGE=$(ls "$REPO_DIR/"*.AppImage 2>/dev/null | head -1)
    if [ -n "$APPIMAGE" ]; then
        ok "AppImage: $APPIMAGE"
    fi
else
    warn "linuxdeploy niet gevonden -- AppImage overgeslagen"
    echo "  Download: https://github.com/linuxdeploy/linuxdeploy/releases"
    echo "  En linuxdeploy-plugin-qt van:"
    echo "  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases"
fi
