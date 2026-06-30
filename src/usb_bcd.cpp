// Lyra — USB-BCD band-data output.  See usb_bcd.h.

#include "usb_bcd.h"

#include "bands.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QSettings>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lyra::ui {

#ifdef _WIN32
namespace {

// FTDI D2XX subset, runtime-loaded from ftd2xx.dll (no build-time SDK).
using FT_STATUS = unsigned long;
using FT_HANDLE = void *;
constexpr FT_STATUS FT_OK = 0;
constexpr DWORD FT_OPEN_BY_SERIAL_NUMBER = 1;
constexpr UCHAR FT_BITMODE_SYNC_BITBANG = 0x04;

using PFN_CreateList = FT_STATUS(WINAPI *)(LPDWORD);
using PFN_InfoDetail = FT_STATUS(WINAPI *)(DWORD, LPDWORD, LPDWORD, LPDWORD,
                                           LPDWORD, void *, void *, FT_HANDLE *);
using PFN_OpenEx     = FT_STATUS(WINAPI *)(void *, DWORD, FT_HANDLE *);
using PFN_SetBitMode = FT_STATUS(WINAPI *)(FT_HANDLE, UCHAR, UCHAR);
using PFN_SetBaud    = FT_STATUS(WINAPI *)(FT_HANDLE, DWORD);
using PFN_Write      = FT_STATUS(WINAPI *)(FT_HANDLE, void *, DWORD, LPDWORD);
using PFN_Close      = FT_STATUS(WINAPI *)(FT_HANDLE);

struct Ftdi {
    HMODULE        lib        = nullptr;
    PFN_CreateList createList = nullptr;
    PFN_InfoDetail infoDetail = nullptr;
    PFN_OpenEx     openEx     = nullptr;
    PFN_SetBitMode setBitMode = nullptr;
    PFN_SetBaud    setBaud     = nullptr;
    PFN_Write      write      = nullptr;
    PFN_Close      close      = nullptr;
    bool ok() const {
        return lib && createList && infoDetail && openEx && setBitMode
               && setBaud && write && close;
    }
};

// Loaded once, lazily.  Absent ftd2xx.dll => ok()==false => feature
// reports "unavailable" and never touches a device.
Ftdi &ftdi() {
    static Ftdi f = []() {
        Ftdi x;
        x.lib = ::LoadLibraryW(L"ftd2xx.dll");
        if (x.lib) {
            auto g = [&](const char *n) { return ::GetProcAddress(x.lib, n); };
            x.createList = reinterpret_cast<PFN_CreateList>(g("FT_CreateDeviceInfoList"));
            x.infoDetail = reinterpret_cast<PFN_InfoDetail>(g("FT_GetDeviceInfoDetail"));
            x.openEx     = reinterpret_cast<PFN_OpenEx>(g("FT_OpenEx"));
            x.setBitMode = reinterpret_cast<PFN_SetBitMode>(g("FT_SetBitMode"));
            x.setBaud    = reinterpret_cast<PFN_SetBaud>(g("FT_SetBaudRate"));
            x.write      = reinterpret_cast<PFN_Write>(g("FT_Write"));
            x.close      = reinterpret_cast<PFN_Close>(g("FT_Close"));
        }
        return x;
    }();
    return f;
}

} // namespace
#endif // _WIN32

UsbBcd::UsbBcd(QObject *parent) : QObject(parent) {
#ifdef _WIN32
    libLoaded_ = ftdi().ok();
#endif
    QSettings s;
    enabled_      = s.value(QStringLiteral("hw/bcdEnabled"), false).toBool();
    serial_       = s.value(QStringLiteral("hw/bcdSerial"), QString()).toString();
    sixtyAsForty_ = s.value(QStringLiteral("hw/bcd60as40"), true).toBool();
    elevenAsTen_  = s.value(QStringLiteral("hw/bcd11as10"), true).toBool();
    // Restore a previously-selected cable on startup (the equivalent of
    // re-selecting it).  Enabling with no serial opens nothing.
    if (enabled_ && libLoaded_ && !serial_.isEmpty()) {
        openDevice();
    }
    // CRITICAL — release the FTDI handle on app shutdown. Lyra's teardown
    // hard-exits (TerminateProcess/_Exit watchdog, see main.cpp) which
    // BYPASSES ~UsbBcd(), so FT_Close would never run and the exclusive
    // D2XX lock would leak — the cable then stays locked (no other app,
    // not even another HPSDR program, can open it) until a full cold
    // reboot or a cable unplug/replug. Every other must-release resource
    // (cmaster / TX worker / mic source) is freed by an aboutToQuit handler
    // for exactly this reason; the BCD cable must be too. closeDevice()
    // drives the lines low (amp → bypass) then FT_Close, and is idempotent
    // so the dtor calling it again on a clean exit is a safe no-op.
    if (auto *a = QCoreApplication::instance()) {
        connect(a, &QCoreApplication::aboutToQuit, this, [this]() {
            qInfo("[usb-bcd] aboutToQuit -> releasing FTDI handle before shutdown");
            closeDevice();
        });
    }
}

UsbBcd::~UsbBcd() {
    closeDevice();
}

QStringList UsbBcd::devices() const {
    QStringList out;
#ifdef _WIN32
    Ftdi &f = ftdi();
    if (f.ok()) {
        DWORD n = 0;
        if (f.createList(&n) == FT_OK) {
            for (DWORD i = 0; i < n; ++i) {
                DWORD flags = 0, type = 0, id = 0, loc = 0;
                char serial[16] = {0};
                char desc[64]   = {0};
                FT_HANDLE h = nullptr;
                if (f.infoDetail(i, &flags, &type, &id, &loc, serial, desc, &h)
                        == FT_OK) {
                    const QString s = QString::fromLatin1(serial);
                    if (!s.isEmpty()) {
                        out << s;
                    }
                }
            }
        }
    }
    // The info-list enumeration above NEVER opens a cable, so a device that is
    // not currently held shows its serial normally.  But the driver reports a
    // BLANK serial for any device that IS open, so the cable we are actively
    // holding would otherwise vanish from its own picker (= the "(none)"
    // symptom).  We know the serial we opened: always include it (deduped) so
    // the dropdown reflects the cable actually in use — never hide the device.
    if (!serial_.isEmpty() && !out.contains(serial_)) {
        out << serial_;
    }
#endif
    return out;
}

void UsbBcd::setEnabled(bool on) {
    if (on == enabled_) {
        return;
    }
    enabled_ = on;
    QSettings().setValue(QStringLiteral("hw/bcdEnabled"), on);
    // Enabling only ARMS the feature.  The exclusive cable handle is taken
    // when a serial is actually selected (openDevice below is the restore of
    // an already-chosen cable — the equivalent of re-selecting it).  Disabling
    // drives the band-data lines low and releases the handle immediately, so
    // any other program can open the cable with no cold reboot.
    if (on) {
        if (!serial_.isEmpty()) {
            openDevice();
            reapply();
        }
    } else {
        closeDevice();
    }
    emit enabledChanged(on);
}

void UsbBcd::setSerial(const QString &serial) {
    if (serial == serial_) {
        return;
    }
    // A different cable is wanted: release the one currently held FIRST so we
    // never hold two exclusive handles (and so the old cable is freed cleanly).
    if (deviceOpen_) {
        closeDevice();
    }
    serial_ = serial;
    QSettings().setValue(QStringLiteral("hw/bcdSerial"), serial);
    if (enabled_ && !serial_.isEmpty()) {
        openDevice();
        reapply();
    }
    emit serialChanged(serial);
}

void UsbBcd::setSixtyAsForty(bool on) {
    if (on == sixtyAsForty_) {
        return;
    }
    sixtyAsForty_ = on;
    QSettings().setValue(QStringLiteral("hw/bcd60as40"), on);
    reapply();
    emit sixtyAsFortyChanged(on);
}

void UsbBcd::setElevenAsTen(bool on) {
    if (on == elevenAsTen_) {
        return;
    }
    elevenAsTen_ = on;
    QSettings().setValue(QStringLiteral("hw/bcd11as10"), on);
    reapply();
    emit elevenAsTenChanged(on);
}

void UsbBcd::applyForFreq(quint32 hz) {
    lastFreqHz_ = hz;
    reapply();
}

void UsbBcd::reapply() {
    const int hz = static_cast<int>(lastFreqHz_);
    int bi       = lyra::bandIndexForFreq(hz);
    // 11m / CB isn't in the amateur table (bi == -1).  If the operator
    // opts in, route it through the 10m filter code (10m's BCD is 9) —
    // the appropriate adjacent filter, mirroring the 60m->40m option.
    if (bi < 0 && elevenAsTen_ && lyra::cbBandIndexForFreq(hz) >= 0) {
        bi = lyra::bandIndexForFreq(28400000);   // 10m amateur index
    }
    const int code = lyra::bcdForBand(bi, sixtyAsForty_);
    if (code != currentBcd_) {
        currentBcd_ = code;
        emit currentBcdChanged(code);
    }
    if (enabled_) {
        writeByte(code);
    }
}

// Take the exclusive cable handle for the configured serial and put it into
// synchronous bit-bang mode: one byte write then drives the 8 output pins that
// carry the amp's band-data lines.  Held open for as long as the cable stays
// selected; the amp reads the static lines continuously, so we write only on a
// real band change (see writeByte's dedup).  Idempotent — a second call while
// already open is a no-op (deviceOpen_ guard).
void UsbBcd::openDevice() {
#ifdef _WIN32
    Ftdi &f = ftdi();
    if (!f.ok() || serial_.isEmpty() || deviceOpen_ || handle_) {
        return;
    }
    FT_HANDLE h = nullptr;
    QByteArray ser = serial_.toLatin1();
    if (f.openEx(ser.data(), FT_OPEN_BY_SERIAL_NUMBER, &h) != FT_OK || !h) {
        emit statusMessage(
            QStringLiteral("USB-BCD: could not open FTDI device '%1'")
                .arg(serial_));
        return;
    }
    // Baud first, then the bit-bang mode + full-output mask (0xFF = all 8 pins
    // are outputs), matching the configure order the cable expects.
    f.setBaud(h, 921600);
    f.setBitMode(h, 0xFF, FT_BITMODE_SYNC_BITBANG);
    handle_     = h;
    deviceOpen_ = true;
    lastValue_  = -1;   // force a real write next time
    writeByte(0);       // start with all band-data lines low (amp bypassed)
    qInfo("[usb-bcd] cable '%s' opened (sync bit-bang 921600) — handle now HELD",
          qUtf8Printable(serial_));
#endif
}

// Drive the band-data lines low (amp → bypass) THEN release the exclusive
// handle, so another program — or a later Lyra run — can open the cable with no
// cold reboot.  The one reliable release point: callers are disable, re-select,
// and the aboutToQuit shutdown hook (the runtime hard-exits past destructors).
void UsbBcd::closeDevice() {
#ifdef _WIN32
    if (!handle_) {
        deviceOpen_ = false;
        qInfo("[usb-bcd] closeDevice: no cable handle held (nothing to release)");
        return;
    }
    Ftdi &f = ftdi();
    FT_STATUS st = 1;   // non-OK unless FT_Close says otherwise
    if (f.ok()) {
        unsigned char z = 0;
        DWORD w = 0;
        if (f.write) f.write(handle_, &z, 1, &w);   // lines low → amp bypass
        if (f.close) st = f.close(handle_);
    }
    qInfo("[usb-bcd] cable '%s' FT_Close -> status=%lu (0=OK) — handle RELEASED",
          qUtf8Printable(serial_), static_cast<unsigned long>(st));
    handle_     = nullptr;
    deviceOpen_ = false;
    lastValue_  = -1;
#endif
}

void UsbBcd::writeByte(int value) {
#ifdef _WIN32
    if (!handle_) {
        return;
    }
    value &= 0xFF;
    if (value == lastValue_) {
        return;
    }
    lastValue_ = value;
    Ftdi &f = ftdi();
    if (f.write) {
        unsigned char b = static_cast<unsigned char>(value);
        DWORD written = 0;
        f.write(handle_, &b, 1, &written);
    }
#else
    (void)value;
#endif
}

} // namespace lyra::ui
