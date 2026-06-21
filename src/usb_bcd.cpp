// Lyra — USB-BCD band-data output.  See usb_bcd.h.

#include "usb_bcd.h"

#include "bands.h"

#include <QByteArray>
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
    if (enabled_ && libLoaded_ && !serial_.isEmpty()) {
        openDevice();
    }
}

UsbBcd::~UsbBcd() {
    closeDevice();
}

QStringList UsbBcd::devices() const {
    QStringList out;
#ifdef _WIN32
    Ftdi &f = ftdi();
    if (!f.ok()) {
        return out;
    }
    DWORD n = 0;
    if (f.createList(&n) != FT_OK) {
        return out;
    }
    for (DWORD i = 0; i < n; ++i) {
        DWORD flags = 0, type = 0, id = 0, loc = 0;
        char serial[16] = {0};
        char desc[64]   = {0};
        FT_HANDLE h = nullptr;
        if (f.infoDetail(i, &flags, &type, &id, &loc, serial, desc, &h) == FT_OK) {
            const QString s = QString::fromLatin1(serial);
            if (!s.isEmpty()) {
                out << s;
            }
        }
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
    if (on) {
        openDevice();
        reapply();
    } else {
        closeDevice();   // leaves the amp's BCD lines low
    }
    emit enabledChanged(on);
}

void UsbBcd::setSerial(const QString &serial) {
    if (serial == serial_) {
        return;
    }
    serial_ = serial;
    QSettings().setValue(QStringLiteral("hw/bcdSerial"), serial);
    closeDevice();
    if (enabled_) {
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

void UsbBcd::openDevice() {
#ifdef _WIN32
    Ftdi &f = ftdi();
    if (!f.ok() || serial_.isEmpty() || handle_) {
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
    f.setBitMode(h, 0xFF, FT_BITMODE_SYNC_BITBANG);
    f.setBaud(h, 921600);
    handle_ = h;
    lastValue_ = -1;   // force a real write next time
    writeByte(0);      // start with all lines low (amp bypassed)
#endif
}

void UsbBcd::closeDevice() {
#ifdef _WIN32
    if (!handle_) {
        return;
    }
    Ftdi &f = ftdi();
    if (f.ok()) {
        unsigned char z = 0;
        DWORD w = 0;
        if (f.write) f.write(handle_, &z, 1, &w);   // leave lines low
        if (f.close) f.close(handle_);
    }
    handle_ = nullptr;
    lastValue_ = -1;
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
