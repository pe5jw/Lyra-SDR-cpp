// Lyra — HPSDR Protocol 1 stream implementation (EP6 receive + EP2
// keepalive).  See hl2_stream.h for the locked architecture +
// protocol reference.

#include "hl2_stream.h"

#include "bands.h"

// WinSock2 MUST be included before windows.h to avoid winsock 1.x
// being pulled in via windows.h transitively.  NOMINMAX keeps the
// windows.h macros from clobbering std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>     // timeBeginPeriod / timeEndPeriod (winmm)

#include <QByteArray>
#include <QMetaObject>
#include <QSettings>
#include <Qt>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace lyra::ipc {

namespace {

// 64-byte HPSDR P1 host→radio control packet.  start=true sends
// 0xEFFE 0x04 0x01 (start IQ); start=false sends 0xEFFE 0x04 0x00
// (stop everything).
QByteArray buildControlPacket(bool start) {
    QByteArray pkt(64, char{0});
    pkt[0] = static_cast<char>(0xEF);
    pkt[1] = static_cast<char>(0xFE);
    pkt[2] = static_cast<char>(0x04);
    pkt[3] = static_cast<char>(start ? 0x01 : 0x00);
    return pkt;
}

// 1032-byte EP2 host→radio keepalive datagram.  Frame-0 C&C carries
// the minimum-viable HL2 config (192 kHz IQ, nddc=4, MOX off,
// duplex bit set) consistent with the post-START gateware default
// — sending this every 2.6 ms satisfies the gateware watchdog
// without re-configuring anything.  Audio out + TX I/Q bytes are
// all zero (RX-only state).
//
// The sequence-number bytes [4..7] are placeholder zeros here;
// the writer thread updates them in-place per send to avoid
// allocating a new QByteArray 380 times per second.
QByteArray buildEp2KeepaliveTemplate() {
    QByteArray pkt(1032, char{0});
    auto *u = reinterpret_cast<std::uint8_t*>(pkt.data());
    // Metis header: magic + data + endpoint=0x02 (host→radio EP2)
    u[0] = 0xEF; u[1] = 0xFE; u[2] = 0x01; u[3] = 0x02;
    // bytes [4..7] = sequence (filled per send)

    // USB frame 1 (offset 8): sync + frame-0 C&C + 504 zero data
    u[ 8] = 0x7F; u[ 9] = 0x7F; u[10] = 0x7F;
    u[11] = 0x00;   // C0 = frame address 0, MOX off
    u[12] = 0x02;   // C1 = sample rate bits [1:0] = 10 = 192 kHz
    u[13] = 0x00;   // C2 = no OC pins, no 10MHz ref override
    u[14] = 0x00;   // C3 = no random, no dither, no preamp adjust
    u[15] = 0x1C;   // C4 = nddc=4 (0x18) | duplex bit (0x04)
    // bytes [16..519] = 504 bytes audio/IQ payload = zero

    // USB frame 2 (offset 520): frame-2 C&C = RX1 (DDC0) frequency
    // (Step 4 tuning).  C0 = addr 2 << 1 = 0x04 (MOX off); C1..C4 =
    // 32-bit freq in Hz, big-endian (reference: networkproto1.c
    // case 2).  The writer overwrites C1..C4 from rx1FreqHz_ every
    // send; the 7.074 MHz default here just makes the template sane
    // pre-first-send.  Frame-0 (USB frame 1 above) still carries the
    // duplex bit every datagram, which is what lets these RX-freq
    // updates take effect.
    u[520] = 0x7F; u[521] = 0x7F; u[522] = 0x7F;
    u[523] = 0x04;                 // C0 = address 2 (RX1 VFO), MOX off
    u[524] = (7074000u >> 24) & 0xFF;   // C1 = freq byte 0 (MSB)
    u[525] = (7074000u >> 16) & 0xFF;   // C2
    u[526] = (7074000u >>  8) & 0xFF;   // C3
    u[527] = (7074000u      ) & 0xFF;   // C4 = freq byte 3 (LSB)
    // bytes [528..1031] = 504 bytes audio/IQ payload = zero

    return pkt;
}

// Stringify a Windows error code (FormatMessage) for log lines.
QString winsockError(int code) {
    wchar_t *buf = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM     |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    QString descr;
    if (len && buf) {
        descr = QString::fromWCharArray(buf, len).trimmed();
        ::LocalFree(buf);
    }
    return descr.isEmpty()
        ? QStringLiteral("WSA=%1").arg(code)
        : QStringLiteral("WSA=%1: %2").arg(code).arg(descr);
}

// Open a native UDP socket bound to ANY:ephemeral.  Sets a 4 MiB
// receive buffer (5 MB/sec EP6 wire rate × ~0.8 sec headroom for
// brief GUI / scheduler stalls — much larger than the kernel
// default 64 KiB which can drop packets under contention).
SocketHandle openNativeUdpSocket(QString *errOut) {
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (errOut) *errOut = QStringLiteral("socket: %1")
                              .arg(winsockError(::WSAGetLastError()));
        return kInvalidSocket;
    }
    int rcvbuf = 4 * 1024 * 1024;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<const char*>(&rcvbuf),
                 sizeof(rcvbuf));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&local),
               sizeof(local)) != 0) {
        if (errOut) *errOut = QStringLiteral("bind: %1")
                              .arg(winsockError(::WSAGetLastError()));
        ::closesocket(s);
        return kInvalidSocket;
    }
    return static_cast<SocketHandle>(s);
}

quint16 localPortOf(SocketHandle sh) {
    sockaddr_in local{};
    int len = sizeof(local);
    if (::getsockname(static_cast<SOCKET>(sh),
                      reinterpret_cast<sockaddr*>(&local), &len) == 0) {
        return ntohs(local.sin_port);
    }
    return 0;
}

} // namespace

HL2Stream::HL2Stream(QObject *parent) : QObject(parent) {
    statsTimer_.setInterval(kStatPeriodMs);
    connect(&statsTimer_, &QTimer::timeout,
            this, &HL2Stream::onStatsTick);
    autoLnaTimer_.setInterval(kAutoLnaPeriodMs);
    connect(&autoLnaTimer_, &QTimer::timeout,
            this, &HL2Stream::onAutoLnaTick);
    // Radio memory: restore the last tuned RX1 frequency.  QSettings
    // resolves to %APPDATA%\N8SDR\Lyra-cpp\ (org/app set in main()
    // before this object is constructed).
    rx1FreqHz_.store(
        QSettings().value(QStringLiteral("rx/freqHz"), 7074000u).toUInt(),
        std::memory_order_relaxed);
    // External filter board: restore enable state + seed the OC pattern
    // for the restored band (so the board is correct from the first send).
    filterBoardEnabled_ =
        QSettings().value(QStringLiteral("hw/filterBoard"), false).toBool();
    updateOcPattern();
    // RX1 LNA gain (AD9866 PGA) — restore the operator's last setting.
    lnaGainDb_.store(
        std::clamp(QSettings().value(QStringLiteral("rx/lnaGainDb"), 31).toInt(),
                   kLnaMinDb, kLnaMaxDb),
        std::memory_order_relaxed);
    // Auto-LNA: restore enable + undo + hold time.  Fresh-install
    // defaults match the operator's Thetis station export
    // (chkAutoStepAttenuator=True, chkAutoAttUndoRX1=True,
    // nudAutoAttHoldRX1=4): Auto ON / undo ON / 4 s hold.
    autoLnaEnabled_ = QSettings().value(QStringLiteral("rx/autoLna"), true).toBool();
    autoLnaUndo_    = QSettings().value(QStringLiteral("rx/autoLnaUndo"), true).toBool();
    autoLnaHoldSec_ = std::clamp(
        QSettings().value(QStringLiteral("rx/autoLnaHoldSec"), 4).toInt(), 1, 60);
}

void HL2Stream::setLnaGainDb(int db)
{
    db = std::clamp(db, kLnaMinDb, kLnaMaxDb);
    if (db == lnaGainDb_.load(std::memory_order_relaxed)) {
        return;
    }
    lnaGainDb_.store(db, std::memory_order_relaxed);
    QSettings().setValue(QStringLiteral("rx/lnaGainDb"), db);
    emit lnaGainChanged();
    emit lnaSetByOperator(db);   // manual change → BandMemory saves per-band
    emit logLine(QStringLiteral("[hl2] LNA gain %1 dB").arg(db));
}

void HL2Stream::applyLnaGainNoPersist(int db)
{
    db = std::clamp(db, kLnaMinDb, kLnaMaxDb);
    if (db == lnaGainDb_.load(std::memory_order_relaxed)) {
        return;
    }
    lnaGainDb_.store(db, std::memory_order_relaxed);
    emit lnaGainChanged();   // slider / dB readout / S-meter comp all track
}

void HL2Stream::setAutoLna(bool on)
{
    if (on == autoLnaEnabled_) {
        return;
    }
    autoLnaEnabled_ = on;
    QSettings().setValue(QStringLiteral("rx/autoLna"), on);
    if (on) {
        overloadLevel_ = 0;
        recovering_ = false;
        holdClock_.restart();
        emit logLine(QStringLiteral("[hl2] Auto-LNA on"));
    } else {
        // Auto roamed the gain freely — hand the LNA back to the
        // operator's stored manual set point.
        const int manual = std::clamp(
            QSettings().value(QStringLiteral("rx/lnaGainDb"), 31).toInt(),
            kLnaMinDb, kLnaMaxDb);
        applyLnaGainNoPersist(manual);
        emit logLine(QStringLiteral("[hl2] Auto-LNA off, restored %1 dB").arg(manual));
    }
    emit autoLnaChanged();
}

void HL2Stream::setAutoLnaUndo(bool on)
{
    if (on == autoLnaUndo_) {
        return;
    }
    autoLnaUndo_ = on;
    QSettings().setValue(QStringLiteral("rx/autoLnaUndo"), on);
    emit autoLnaChanged();
}

void HL2Stream::setAutoLnaHoldSec(int sec)
{
    sec = std::clamp(sec, 1, 60);
    if (sec == autoLnaHoldSec_) {
        return;
    }
    autoLnaHoldSec_ = sec;
    QSettings().setValue(QStringLiteral("rx/autoLnaHoldSec"), sec);
    emit autoLnaChanged();
}

// 400 ms overload monitor + Auto-LNA loop (main thread).  Always runs
// while the stream is up so the overload lamp tracks regardless of
// Auto; gain is only touched when Auto is enabled.  Gain-sense mirror
// of the reference HERMESLITE auto-attenuator: confirm sustained
// overload (>3 cycles), back off 3 dB; when clear, creep +1 dB per
// hold interval up to the +48 ceiling (ride the overload edge).
void HL2Stream::onAutoLnaTick()
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    // Sample overload INSTANTANEOUSLY at poll time (most-recent
    // address-0 frame), matching the reference HL2 poll — a window-OR
    // over-triggered on a strong front end and pinned gain to the floor.
    const bool ov = adcOverloadNow_.load(std::memory_order_relaxed);
    overloadLevel_ = ov ? std::min(overloadLevel_ + 1, 5)
                        : std::max(overloadLevel_ - 1, 0);
    // Lamp + back-off both key on SUSTAINED overload (level > 3 ≈ 1.6 s
    // of genuine clipping), matching the reference "red after 3 cycles".
    const bool sustained = ov && overloadLevel_ > 3;
    if (sustained != adcOverload_) {
        adcOverload_ = sustained;
        emit adcOverloadChanged();
    }
    if (!autoLnaEnabled_) {
        return;
    }

    const int g = lnaGainDb_.load(std::memory_order_relaxed);
    if (sustained) {
        // Sustained overload — back off 3 dB to protect the ADC.  Reset
        // recovery so the next pull-up waits the full hold time.
        const int ng = std::max(g - 3, kLnaMinDb);
        if (ng != g) {
            applyLnaGainNoPersist(ng);
            emit logLine(QStringLiteral("[hl2] Auto-LNA back-off → %1 dB").arg(ng));
        }
        recovering_ = false;
        holdClock_.restart();
    } else if (ov) {
        // Transient overload (not yet sustained) — hold, don't recover.
        recovering_ = false;
    } else if (autoLnaUndo_) {
        // Band clear.  The FIRST pull-up waits the operator's full hold
        // time (so a brief lull doesn't yank gain back up); subsequent
        // steps creep at the brisker recover cadence so we climb back
        // quickly.  Pull-downs above are unaffected by this.
        const qint64 need = recovering_
            ? static_cast<qint64>(kAutoLnaRecoverMs)
            : static_cast<qint64>(autoLnaHoldSec_) * 1000;
        if (holdClock_.elapsed() >= need) {
            if (g < kLnaMaxDb) {
                applyLnaGainNoPersist(g + 1);
            }
            recovering_ = true;
            holdClock_.restart();
        }
    }
}

HL2Stream::~HL2Stream() {
    close();
}

void HL2Stream::open(const QString &ip) {
    if (running_.load(std::memory_order_acquire)) {
        emit logLine(QStringLiteral("open: stream already running, ignored"));
        return;
    }
    // Defensive: if a previous worker exited on its own (e.g. fatal
    // error path) the jthread may still be joinable.  Join before
    // reassigning so std::jthread move-assign doesn't terminate().
    if (rxWorker_.joinable()) { rxWorker_.request_stop(); rxWorker_.join(); }
    if (txWorker_.joinable()) { txWorker_.request_stop(); txWorker_.join(); }
    if (socket_ != kInvalidSocket) {
        ::closesocket(static_cast<SOCKET>(socket_));
        socket_ = kInvalidSocket;
    }

    QString err;
    socket_ = openNativeUdpSocket(&err);
    if (socket_ == kInvalidSocket) {
        emit logLine(QStringLiteral("open: %1").arg(err));
        return;
    }
    const quint16 lport = localPortOf(socket_);

    targetIp_ = ip;
    totalDg_.store(0);  seqErrors_.store(0);
    framingErrors_.store(0);  windowDg_.store(0);
    txTotalDg_.store(0);  txWindowDg_.store(0);
    txSendErrors_.store(0);  txSeq_.store(0);
    rx1DbFs_.store(-200.0, std::memory_order_relaxed);
    dgPerSec_   = 0.0;
    txDgPerSec_ = 0.0;
    running_.store(true, std::memory_order_release);
    emit runningChanged();
    emit statsChanged();
    emit logLine(QStringLiteral(
        "opening EP6 stream to %1:%2 (local port %3) ...")
        .arg(ip).arg(kRadioPort).arg(lport));

    statsTimer_.start();
    statsClock_.start();   // baseline for actual-elapsed dg/s
    // Auto-LNA / overload monitor: fresh accumulator + hold clock.
    overloadLevel_ = 0;
    recovering_ = false;
    adcOverloadNow_.store(false, std::memory_order_relaxed);
    holdClock_.start();
    autoLnaTimer_.start();

    // Radio memory: remember this radio so the next launch can
    // auto-connect without a Discover (read in main()).
    QSettings().setValue(QStringLiteral("radio/lastIp"), ip);

    // Spawn RX first so it's already listening when TX sends START.
    // Native UDP sendto + recvfrom on one socket from different
    // threads is documented thread-safe at the OS level.
    const SocketHandle sh = socket_;
    rxWorker_ = std::jthread([this, sh](std::stop_token stop) {
        rxWorkerLoop(std::move(stop), sh);
    });
    txWorker_ = std::jthread([this, sh, ip](std::stop_token stop) {
        txWorkerLoop(std::move(stop), sh, ip);
    });
}

void HL2Stream::close() {
    if (!running_.load(std::memory_order_acquire) &&
        !rxWorker_.joinable() &&
        !txWorker_.joinable() &&
        socket_ == kInvalidSocket) {
        return;
    }
    emit logLine(QStringLiteral("closing EP6 stream ..."));

    // Request stop on both workers BEFORE joining either so they
    // wind down in parallel (RX: bounded by recv timeout 100 ms,
    // TX: bounded by waitable-timer wait cap 100 ms).
    if (rxWorker_.joinable()) rxWorker_.request_stop();
    if (txWorker_.joinable()) txWorker_.request_stop();
    if (rxWorker_.joinable()) rxWorker_.join();
    if (txWorker_.joinable()) txWorker_.join();

    // Both workers stopped — main thread sends STOP, then closes
    // the socket.  Best-effort; if STOP doesn't reach the gateware
    // its own watchdog will idle the stream within ~13 sec anyway.
    if (socket_ != kInvalidSocket) {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port   = htons(kRadioPort);
        ::inet_pton(AF_INET,
                    targetIp_.toLatin1().constData(),
                    &dest.sin_addr);
        const QByteArray stopPkt = buildControlPacket(false);
        ::sendto(static_cast<SOCKET>(socket_),
                 stopPkt.constData(), stopPkt.size(), 0,
                 reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        ::closesocket(static_cast<SOCKET>(socket_));
        socket_ = kInvalidSocket;
    }

    statsTimer_.stop();
    autoLnaTimer_.stop();
    if (adcOverload_) { adcOverload_ = false; emit adcOverloadChanged(); }
    onStatsTick();  // flush the final window so the UI shows true totals
    running_.store(false, std::memory_order_release);
    emit runningChanged();
    emit logLine(QStringLiteral(
        "stream closed: RX %1 dg (%2 seq errs, %3 framing), "
        "TX %4 keepalives (%5 send errors)")
        .arg(totalDg_.load())
        .arg(seqErrors_.load())
        .arg(framingErrors_.load())
        .arg(txTotalDg_.load())
        .arg(txSendErrors_.load()));
}

void HL2Stream::onStatsTick() {
    const qint64 rxWin = windowDg_.exchange(0,   std::memory_order_acq_rel);
    const qint64 txWin = txWindowDg_.exchange(0, std::memory_order_acq_rel);
    // Divide by the ACTUAL elapsed interval (not the assumed period),
    // so timer jitter from main-thread load doesn't inflate the rate.
    qint64 elapsedMs = statsClock_.restart();
    if (elapsedMs <= 0) {
        elapsedMs = kStatPeriodMs;
    }
    const double scale = 1000.0 / static_cast<double>(elapsedMs);
    dgPerSec_   = static_cast<double>(rxWin) * scale;
    txDgPerSec_ = static_cast<double>(txWin) * scale;
    emit statsChanged();

    // TX-0a raw-telemetry probe — env-gated, 5 Hz, main thread.  Logs
    // the decoded raw ADC slots + converted values so the operator can
    // confirm/correct the gateware-rev-specific AINx→slot map against a
    // known reference on the bench (verify-don't-guess).
    static const bool kTelemDebug =
        qEnvironmentVariableIsSet("LYRA_TELEM_DEBUG") &&
        qgetenv("LYRA_TELEM_DEBUG") != "0";
    if (kTelemDebug && running_.load(std::memory_order_relaxed)) {
        // qWarning (not the logLine signal) so it lands in LogBuffer →
        // View → Log + the log file regardless of verbose, exactly like
        // the [wx-diag] diagnostics.  (A windowed build has no console
        // stdout, so a terminal will never show it.)
        // Raw-rotation dump so the ak4951v4 slot map can be read off the
        // operator's hardware.  a0/a8/a10 show both raw 16-bit BE pairs
        // (C1:C2, C3:C4) of those addresses; a10 C3:C4 is now PA bias
        // current (re-pointed from the dead 0x18), supplyRaw is 0x00
        // C1:C2 >>4.  T/V are the confirmed-good decodes.
        const QString s = QStringLiteral(
            "[telem] a0=(%1,%2) a8=(%3,%4) a10=(%5,%6) "
            "supplyRaw=%7 | T=%8C V=%9 PA=%10A")
            .arg(telA0c12Raw_.load()).arg(telA0c34Raw_.load())
            .arg(telTempRaw_.load()).arg(telFwdRaw_.load())
            .arg(telRevRaw_.load()).arg(telPaCurRaw_.load())
            .arg(telSupplyRaw_.load())
            .arg(hl2TempC(), 0, 'f', 1)
            .arg(hl2SupplyV(), 0, 'f', 2)
            .arg(paCurrentA(), 0, 'f', 2);
        qWarning("%s", qUtf8Printable(s));
    }
}

void HL2Stream::onFatalError(QString reason) {
    emit logLine(QStringLiteral("FATAL: %1").arg(reason));
    close();
}

// ---- TX-0a: HL2 telemetry getters (main thread) ------------------
// Convert the raw 12-bit EP6 ADC slots written by the RX worker.
// Formulas per the documented HL2 status rotation; the AINx→slot map
// and these scale constants are gateware-rev-specific on HL2+ and MUST
// be bench-verified (LYRA_TELEM_DEBUG=1 logs the raw rotation).  A
// known-good HL2+ reads ≈12-13 V supply/VDD, board-ambient temp, and
// (during full tune into a dummy) ≈1.8 A PA current.
namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}
double HL2Stream::hl2TempC() const {
    const int raw = telTempRaw_.load(std::memory_order_relaxed);
    if (raw < 0) return kNaN;
    return (3.26 * (raw / 4096.0) - 0.5) / 0.01;
}
double HL2Stream::hl2SupplyV() const {
    const int raw = telSupplyRaw_.load(std::memory_order_relaxed);
    if (raw < 0) return kNaN;
    return (raw / 4095.0) * 5.0 * (23.0 / 1.1);
}
double HL2Stream::paCurrentA() const {
    const int raw = telPaCurRaw_.load(std::memory_order_relaxed);
    if (raw < 0) return kNaN;
    // 0x10 C3:C4 (PA bias current). Sense-amp formula per the HL2
    // current-shunt path; magnitude pending TX-phase validation.
    return ((3.26 * (raw / 4096.0)) / 50.0) / 0.04 / (1000.0 / 1270.0);
}
double HL2Stream::fwdPowerW() const {
    const int raw = telFwdRaw_.load(std::memory_order_relaxed);
    if (raw < 0) return kNaN;
    // Provisional + UNCALIBRATED — real watts need the per-band 3-point
    // forward-power cal (a later TX-3 step).  Bridge/ref per HL2 case.
    const double v = (raw - 6.0) / 4095.0 * 3.3;
    return (v > 0.0) ? (v * v) / 1.5 : 0.0;
}
double HL2Stream::revPowerW() const {
    const int raw = telRevRaw_.load(std::memory_order_relaxed);
    if (raw < 0) return kNaN;
    const double v = (raw - 6.0) / 4095.0 * 3.3;
    return (v > 0.0) ? (v * v) / 1.5 : 0.0;
}

// ----------------------------------------------------------------
// RX worker — dedicated OS thread, drains EP6 datagrams at line
// rate (~5040 dg/sec at 192 kHz nddc=4), verifies Metis header +
// USB sync, counts dropouts via sequence-number gaps.

void HL2Stream::rxWorkerLoop(std::stop_token stop, SocketHandle sh) {
    const SOCKET s = static_cast<SOCKET>(sh);

    // 100 ms recv timeout — bounds stop-token check latency.  The
    // healthy wire rate is ~5040 dg/sec so recvfrom returns
    // immediately almost every call; the timeout only fires during
    // the brief window between Open and the first EP6 datagram.
    DWORD timeout = 100;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));

    quint32 expectedSeq = 0;
    bool    firstPacket = true;
    QByteArray buf;
    buf.resize(2048);  // generous; Metis EP6 datagrams are 1032 bytes

    // ---- Step 2c: RX1 dBFS accumulator -----------------------
    // Local-to-this-thread state — re-initialized per-open since
    // each open() spawns a fresh worker.  The accumulator sums
    // magnitude² (I²+Q²) across kRmsWindowSamples DDC0 samples,
    // then computes dBFS = 10·log₁₀(meanSq) and atomically
    // publishes to rx1DbFs_ for the QML banner.  At 192 kHz IQ
    // rate, 9600 samples = 50 ms — updates 20× per second so the
    // 5 Hz QML stats tick always sees the most recent ~50 ms RMS.
    //
    // Wire format for each 26-byte slot (nddc=4, gateware default
    // verified ak4951v4 RTL + Thetis networkproto1.c:527-541):
    //   bytes [0..2]   DDC0 I — 24-bit signed BE
    //   bytes [3..5]   DDC0 Q — 24-bit signed BE
    //   bytes [6..23]  DDC1/2/3 I/Q  — Step 5 RX2 work, ignored here
    //   bytes [24..25] mic sample    — Step 3+ TX work, ignored here
    //
    // 24-bit normalization: pack the 3 bytes into the TOP 24 bits
    // of an int32 with the low byte zero, then divide by 2³¹.
    // bytes[0]'s high bit naturally lands at bit 31 = the sign
    // bit of int32, so the sign extends correctly without any
    // explicit cast.  Range: [-1.0, +1.0).
    double rmsAcc       = 0.0;
    int    rmsAccCount  = 0;
    constexpr int kRmsWindowSamples = 9600;        // 50 ms @ 192 kHz
    constexpr double kInv2Pow31      = 1.0 / 2147483648.0;

    while (!stop.stop_requested()) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = ::recvfrom(s, buf.data(), buf.size(), 0,
                                 reinterpret_cast<sockaddr*>(&from),
                                 &fromLen);
        if (n == SOCKET_ERROR) {
            const int err = ::WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;          // expected
            if (err == WSAEINTR     ||
                err == WSAESHUTDOWN ||
                err == WSAENOTSOCK)   break;            // teardown
            framingErrors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (n != kMetisDgSize) {
            framingErrors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto *u =
            reinterpret_cast<const std::uint8_t*>(buf.constData());

        // Metis header: magic + data + endpoint=0x06 (radio→host)
        if (u[0] != 0xEF || u[1] != 0xFE ||
            u[2] != 0x01 || u[3] != 0x06) {
            framingErrors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const quint32 seq =
            (static_cast<quint32>(u[4]) << 24) |
            (static_cast<quint32>(u[5]) << 16) |
            (static_cast<quint32>(u[6]) <<  8) |
             static_cast<quint32>(u[7]);
        // The HL2 gateware (ak4951v4 variant + every other HL2
        // gateware that shares this Verilog) implements the EP6
        // sequence counter as a 20-bit register, NOT 32-bit:
        //
        //   logic [19:0] ep6_seq_no = 20'h0;
        //   ...
        //   3'h3: udp_data_next = 8'h00;                       // hi byte
        //   3'h2: udp_data_next = {4'h00, ep6_seq_no[19:16]};  // hi 4b=0
        //   3'h1: udp_data_next = ep6_seq_no[15:8];
        //   3'h0: udp_data_next = ep6_seq_no[7:0];
        //         ep6_seq_no_next = ep6_seq_no + 'h1;          // wraps 2^20
        //
        // (gateware source verified 2026-05-20 — RTL of the HL2+
        // ak4951v4 variant running on the operator's bench.)
        //
        // At ~5053 dg/sec the counter wraps every 207.5 sec
        // (= 3 min 27 sec).  Treating the sequence as a 32-bit
        // monotonic field (per the generic HPSDR P1 spec) would
        // therefore flag every legitimate wrap as a "seq error",
        // burying real diagnostic value in deterministic noise —
        // operator HL2+ bench caught this on the first long run
        // (10 false alarms over 35 min = exactly 10 wraps).  We
        // mask to 20 bits when computing the next-expected so a
        // wrap from 0xFFFFF -> 0x00000 produces no false alarm
        // and every counted seqError is a REAL packet-loss event.
        // The mask is also forward-safe for the v0.4 ANAN family:
        // a hypothetical 32-bit-counter radio's wrap is one event
        // per 2^32 packets ≈ 5 days at typical rates, negligible.
        constexpr quint32 kSeqMask20 = 0x000FFFFF;

        if (firstPacket) {
            expectedSeq = (seq + 1) & kSeqMask20;
            firstPacket = false;
        } else {
            if (seq != expectedSeq) {
                seqErrors_.fetch_add(1, std::memory_order_relaxed);
            }
            expectedSeq = (seq + 1) & kSeqMask20;
        }
        // Both USB frames must begin 0x7F 0x7F 0x7F.
        if (u[ 8] != 0x7F || u[ 9] != 0x7F || u[ 10] != 0x7F ||
            u[520] != 0x7F || u[521] != 0x7F || u[522] != 0x7F) {
            framingErrors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        totalDg_.fetch_add(1, std::memory_order_relaxed);
        windowDg_.fetch_add(1, std::memory_order_relaxed);

        // ---- ADC-overload telemetry (EP6 status C&C) ---------
        // Each USB frame's status bytes sit at frame-offset 3..7
        // (C0..C4) → absolute offsets 11 and 523.  The gateware
        // rotates C0's address field (bits [7:3]) through 0x00 /
        // 0x08 / 0x10 / 0x18; ADC0 overload lives in C1 bit 0 ONLY
        // at address 0.  At the other addresses C1 carries power /
        // voltage telemetry whose bit 0 is usually set, so reading
        // it there reports "always clipping".  Also skip I2C-readback
        // frames (C0 bit 7).  Store the bit by DIRECT OVERWRITE (most-
        // recent address-0 frame wins) — NOT a window-OR — so the
        // 400 ms tick samples it instantaneously, exactly like the
        // reference HL2 read loop (networkproto1.c:502).
        // The address field (C0 bits [7:3] = c0 & 0xF8) rotates through
        // 0x00 / 0x08 / 0x10 / 0x18.  Address 0 carries ADC0 overload
        // (C1 bit 0); the others carry 12-bit ADC telemetry as two
        // big-endian byte pairs (C1:C2, C3:C4).  TX-0a decodes those into
        // raw atomics (direct overwrite, most-recent wins) — RF-safe,
        // pure read.  I2C-readback frames (C0 bit 7) are skipped.
        for (int f = 0; f < 2; ++f) {
            const std::uint8_t *st = u + (f == 0 ? 11 : 523);  // C0..C4
            const std::uint8_t c0 = st[0];
            if (c0 & 0x80) {
                continue;  // I2C readback, not a status frame
            }
            const int p12 = (static_cast<int>(st[1]) << 8) | st[2];  // C1:C2
            const int p34 = (static_cast<int>(st[3]) << 8) | st[4];  // C3:C4
            switch (c0 & 0xF8) {
            case 0x00:  // ADC0 overload (C1 bit 0) + supply volts (C1:C2 >>4)
                adcOverloadNow_.store((st[1] & 0x01) != 0,
                                      std::memory_order_relaxed);
                telA0c12Raw_.store(p12, std::memory_order_relaxed);
                telA0c34Raw_.store(p34, std::memory_order_relaxed);
                // Supply (Hermes Volts) lives in this frame's C1:C2 as a
                // 12-bit value left-shifted into 16 bits on the HL2+
                // ak4951v4 gateware — recover the raw count with >>4.
                // Bench-verified: 7680>>4=480 -> (480/4095)*5*(23/1.1)=12.25 V.
                telSupplyRaw_.store(p12 >> 4, std::memory_order_relaxed);
                break;
            case 0x08:  // AIN5 temp (C1:C2) + fwd power AIN1 (C3:C4)
                telTempRaw_.store(p12, std::memory_order_relaxed);
                telFwdRaw_.store(p34, std::memory_order_relaxed);
                break;
            case 0x10:  // rev power (C1:C2) + PA bias current (C3:C4)
                telRevRaw_.store(p12, std::memory_order_relaxed);
                // PA bias current lives in this slot's C3:C4 (12-bit) on
                // the HL2+ ak4951v4 gateware (control.v iresp 4-slot
                // rotation, addr2 C3:C4) — NOT a PA-volts/VDD slot. There
                // is no PA-volts telemetry on this gateware. Current
                // magnitude calibration is pending TX-phase validation
                // (idle bias ~0.2 A, full tune ~1.8 A on a known unit).
                telPaCurRaw_.store(p34, std::memory_order_relaxed);
                break;
            case 0x18:  // dead/junk on HL2+ ak4951v4 (bench: a18=(0,0);
                        // gateware addr3 = C1:C2 zero, C3:C4 debug). No
                        // telemetry — supply is 0x00 C1:C2 >>4, PA current
                        // is 0x10 C3:C4 above.
                break;
            default:
                break;
            }
        }

        // ---- Step 2c/3d: parse DDC0 IQ -----------------------
        // Two USB frames per datagram at offsets 8 and 520; each
        // carries 19 sample slots starting at frame-offset 8 = 38
        // DDC0 IQ frames per datagram.  We (a) accumulate RMS for the
        // RX1 dBFS readout (Step 2c) and (b) pack interleaved
        // (I,Q,…) doubles in [-1,1) to hand the DSP engine once per
        // datagram (Step 3d).  Scaling (b0<<24|b1<<16|b2<<8) * 1/2^31
        // is byte-identical to the reference HL2 receive parse.
        constexpr int kFramesPerDatagram = 2 * 19;   // 38
        double iqScratch[2 * kFramesPerDatagram];
        int    iqIdx = 0;
        for (int usbFrame = 0; usbFrame < 2; ++usbFrame) {
            const std::uint8_t *frame = u + (usbFrame == 0 ? 8 : 520);
            for (int slot = 0; slot < 19; ++slot) {
                const std::uint8_t *sb = frame + 8 + slot * 26;
                // DDC0 I — bytes 0..2, packed into top 24 bits of int32
                const std::int32_t iRaw = static_cast<std::int32_t>(
                    (static_cast<std::uint32_t>(sb[0]) << 24) |
                    (static_cast<std::uint32_t>(sb[1]) << 16) |
                    (static_cast<std::uint32_t>(sb[2]) <<  8));
                // DDC0 Q — bytes 3..5
                const std::int32_t qRaw = static_cast<std::int32_t>(
                    (static_cast<std::uint32_t>(sb[3]) << 24) |
                    (static_cast<std::uint32_t>(sb[4]) << 16) |
                    (static_cast<std::uint32_t>(sb[5]) <<  8));
                const double iVal = static_cast<double>(iRaw) * kInv2Pow31;
                const double qVal = static_cast<double>(qRaw) * kInv2Pow31;
                iqScratch[iqIdx++] = iVal;
                iqScratch[iqIdx++] = qVal;
                rmsAcc += iVal * iVal + qVal * qVal;
                if (++rmsAccCount >= kRmsWindowSamples) {
                    const double meanSq =
                        rmsAcc / static_cast<double>(kRmsWindowSamples);
                    const double db = (meanSq > 0.0)
                        ? 10.0 * std::log10(meanSq)
                        : -200.0;
                    rx1DbFs_.store(db, std::memory_order_relaxed);
                    rmsAcc      = 0.0;
                    rmsAccCount = 0;
                }
            }
        }
        // Step 3d: hand this datagram's IQ to the DSP engine inline on
        // this RX worker thread (no Qt signal / no cross-thread queue).
        if (iqSink_) {
            iqSink_(iqScratch, kFramesPerDatagram);
        }
    }
}

void HL2Stream::setRx1FreqHz(quint32 hz) {
    const quint32 prev = rx1FreqHz_.exchange(hz, std::memory_order_relaxed);
    if (prev != hz) {
        QSettings().setValue(QStringLiteral("rx/freqHz"), hz);
        emit rx1FreqChanged();
        emit logLine(QStringLiteral("RX1 -> %1 Hz (%2 MHz)")
                     .arg(hz).arg(hz / 1.0e6, 0, 'f', 6));
        // Band may have changed -> re-apply the filter-board OC pattern.
        updateOcPattern();
    }
}

// ---------------------------------------------------------------
// TX-0b: RF-transmit state setters (foundation; WIRE-INERT).
// Each clamps + stores the HL2+ byte encoding for its TX C&C frame
// (Thetis WriteMainLoop_HL2 + ak4951v4 gateware decode; see
// hl2_stream.h for the per-register map + cites).  NONE are read by the EP2 emission loop
// yet, so at MOX=0 / PA-off the datagram stays byte-identical to RX.
// A later TX phase wires these into the C&C round-robin under MOX gating.

void HL2Stream::setMox(bool on) {
    // Low-level: forces the wire MOX atomic directly, bypassing the
    // TR-sequenced FSM.  Prefer requestMox() for operator/CAT paths.
    // This stays in place for the FSM's own internal use + future
    // dev/diag tools.
    const bool prev = mox_.exchange(on, std::memory_order_relaxed);
    if (prev != on) {
        emit logLine(QStringLiteral("TX: MOX bit (raw setter) -> %1")
                     .arg(on ? QStringLiteral("ON") : QStringLiteral("off")));
    }
}

void HL2Stream::setTxFreqHz(quint32 hz) {
    const quint32 prev = txFreqHz_.exchange(hz, std::memory_order_relaxed);
    if (prev != hz) {
        emit logLine(QStringLiteral("TX freq -> %1 Hz (%2 MHz)")
                     .arg(hz).arg(hz / 1.0e6, 0, 'f', 6));
    }
}

void HL2Stream::setTxDriveLevel(int level) {
    txDriveLevel_.store(std::clamp(level, 0, 255), std::memory_order_relaxed);
}

void HL2Stream::setTxStepAttnDb(int db) {
    txStepAttnDb_.store(std::clamp(db, 0, 31), std::memory_order_relaxed);
}

void HL2Stream::setPaEnabled(bool on) {
    // Lands C2 bit 3 (0x08) of slot 10 (frame 0x12) — gateware
    // PA-enable, active-high.  Default false; TX-0c-pa-debug will add
    // the operator-gated Settings checkbox + the dummy-load bench
    // safety wrap.  Wire-inert today regardless of value because no
    // MOX edge has been driven from the UI yet.
    const bool prev = paOn_.exchange(on, std::memory_order_relaxed);
    if (prev != on) {
        emit logLine(QStringLiteral("TX: PA enable -> %1")
                     .arg(on ? QStringLiteral("ON") : QStringLiteral("off")));
    }
}

// ---------------------------------------------------------------
// TX-0c-fsm: MOX/PTT sequencer — single funnel `requestMox(bool)`,
// internal TR-sequenced state machine.  All steps run on this
// QObject's thread via QTimer::singleShot (no sleeps, no blocking).
// Wire workers see the resulting mox_ / txStepAttnDb_ atomic flips
// the same way they did before.
//
// State is captured by three plain bool/int fields owned by this
// thread (requestedMox_ / fsmRunning_ / savedTxStepAttn_), plus
// moxActive_ for the UI-visible "on the air" truth.  Re-entrancy
// is handled by re-reading requestedMox_ at every timer boundary —
// an operator cancel mid-keydown unwinds cleanly, a re-key during
// the keyup space window collapses to stay TX (Thetis pattern).

void HL2Stream::requestMox(bool on) {
    requestedMox_ = on;
    if (!fsmRunning_) {
        fsmAdvance();
    }
    // If fsmRunning_ is true the chain in flight will see the new
    // intent at its next timer boundary — no race with the wire
    // worker because all FSM bookkeeping is single-thread.
}

void HL2Stream::fsmAdvance() {
    // Decide what (if anything) to do based on intent vs wire state.
    const bool wireOn = mox_.load(std::memory_order_relaxed);
    if (requestedMox_ && !wireOn) {
        // Keydown: raise ATT-on-TX, then schedule MOX bit set after
        // mox_delay (gives the RX path time to attenuate before the
        // gateware T/R relay engages).
        fsmRunning_ = true;
        savedTxStepAttn_ = txStepAttnDb_.load(std::memory_order_relaxed);
        setTxStepAttnDb(kAttOnTxDb);
        emit logLine(QStringLiteral(
            "TX: keydown — ATT-on-TX %1 dB, mox_delay %2 ms")
            .arg(kAttOnTxDb).arg(kMoxDelayMs));
        QTimer::singleShot(kMoxDelayMs, this,
                           [this]() { fsmKeydownPostMox(); });
    } else if (!requestedMox_ && wireOn) {
        // Keyup: schedule the space_mox re-key window.  Don't touch
        // mox_ yet — kSpaceMoxDelayMs gives the operator a chance to
        // re-key (CW dot tail, mic re-key) before the wire flip.
        fsmRunning_ = true;
        emit logLine(QStringLiteral(
            "TX: keyup — space_mox_delay %1 ms (re-key window)")
            .arg(kSpaceMoxDelayMs));
        QTimer::singleShot(kSpaceMoxDelayMs, this,
                           [this]() { fsmKeyupPostSpace(); });
    } else {
        // Nothing to do: intent already matches wire state.
        fsmRunning_ = false;
    }
}

void HL2Stream::fsmKeydownPostMox() {
    if (!requestedMox_) {
        // Operator cancelled mid-keydown (before the wire MOX bit
        // even went on) — restore the saved step-att and exit clean.
        setTxStepAttnDb(savedTxStepAttn_);
        emit logLine(QStringLiteral(
            "TX: keydown cancelled mid-mox_delay; ATT-on-TX restored"));
        fsmRunning_ = false;
        return;
    }
    // Flip the wire MOX bit — visible on the next EP2 datagram
    // (~2.6 ms worst case at 380 Hz cadence).  C0 bit 0 on both USB
    // frames of the next datagram (composeCC snapshots mox_ once
    // per send) — coherent.
    mox_.store(true, std::memory_order_release);
    QTimer::singleShot(kRfDelayMs, this,
                       [this]() { fsmKeydownSettled(); });
}

void HL2Stream::fsmKeydownSettled() {
    if (!requestedMox_) {
        // Cancelled mid-rf_delay — wire MOX already went on, must
        // unwind through a real keyup (space → clear → ptt_out →
        // restore att).  Schedule the keyup chain directly.
        emit logLine(QStringLiteral(
            "TX: keydown cancelled mid-rf_delay; initiating keyup"));
        QTimer::singleShot(kSpaceMoxDelayMs, this,
                           [this]() { fsmKeyupPostSpace(); });
        return;
    }
    moxActive_ = true;
    emit moxActiveChanged(true);
    emit logLine(QStringLiteral("TX: MOX_TX (wire MOX bit settled)"));
    fsmRunning_ = false;
    // If the operator already requested off again during rf_delay
    // (rare), pick it up now.
    if (!requestedMox_) {
        fsmAdvance();
    }
}

void HL2Stream::fsmKeyupPostSpace() {
    if (requestedMox_) {
        // Re-key during the space window — collapse: stay TX, do not
        // flip the wire bit.  mox_ is still true on the wire,
        // moxActive_ remains true, no events emitted.
        emit logLine(QStringLiteral(
            "TX: re-keyed during space_mox window — staying TX"));
        fsmRunning_ = false;
        return;
    }
    mox_.store(false, std::memory_order_release);
    QTimer::singleShot(kPttOutDelayMs, this,
                       [this]() { fsmKeyupSettled(); });
}

void HL2Stream::fsmKeyupSettled() {
    if (requestedMox_) {
        // Operator re-keyed in the ~5 ms ptt_out window — the wire
        // bit cleared briefly, now needs to go back on.  Unwind to
        // the keydown path through fsmAdvance.
        emit logLine(QStringLiteral(
            "TX: re-keyed during ptt_out_delay — re-keying"));
        fsmRunning_ = false;
        fsmAdvance();
        return;
    }
    // Restore the operator's pre-keydown step-att (today always 0;
    // forward-compat for a future operator-tunable RX step-att).
    setTxStepAttnDb(savedTxStepAttn_);
    moxActive_ = false;
    emit moxActiveChanged(false);
    emit logLine(QStringLiteral(
        "TX: RX (wire MOX cleared, ATT-on-TX restored to %1 dB)")
        .arg(savedTxStepAttn_));
    fsmRunning_ = false;
}

void HL2Stream::setSampleRate(int hz) {
    std::uint8_t bits;
    switch (hz) {
    case 96000:  bits = 0x01; break;
    case 192000: bits = 0x02; break;
    case 384000: bits = 0x03; break;
    default:     return;   // 48k excluded (EP2 cadence)
    }
    sampleRateBits_.store(bits, std::memory_order_relaxed);
    emit logLine(QStringLiteral("IQ sample rate -> %1 kHz (wire C1)")
                 .arg(hz / 1000));
}

void HL2Stream::setFilterBoardEnabled(bool on) {
    if (on == filterBoardEnabled_) {
        return;
    }
    filterBoardEnabled_ = on;
    QSettings().setValue(QStringLiteral("hw/filterBoard"), on);
    updateOcPattern();
    emit filterBoardChanged(on);
    emit logLine(QStringLiteral("Filter board %1")
                 .arg(on ? QStringLiteral("ENABLED") : QStringLiteral("off")));
}

// ----------------------------------------------------------------
// TX-0c-emit: compose one HL2 C&C frame for cycle slot `idx` (0..18).
// Pure function (no allocs, no locks); writes 5 bytes to `out[0..4]`
// = C0..C4.  C0's bit 0 carries MOX (caller snapshots it once per
// datagram so both USB frames are coherent).  See header doc block
// for the verified slot map + cites (Thetis WriteMainLoop_HL2 +
// ak4951v4 gateware control.v).
//
// WIRE-IDENTICAL TO PRE-TX-0c AT MOX=0 / PA-OFF:
//   * MOX bit clears (mox=false) -> C0 bit 0 = 0 as before.
//   * Slot 10 (0x12) C2 PA-enable bit (0x08) stays clear with paOn_=false.
//   * Slot 11 (0x14) RX-branch matches the old emitter exactly:
//     C4 = 0x40 | ((lnaGainDb_+12) & 0x3F).
//   * New slots (1/3/4/5/6/7..18) emit valid addresses with bytes the
//     gateware caches inert (no MOX edge -> no PA enable -> no RF).
void HL2Stream::composeCC(int idx, bool mox, std::uint8_t out[5]) const {
    const std::uint8_t mb = mox ? std::uint8_t{0x01} : std::uint8_t{0x00};
    auto setAddr = [&out, mb](std::uint8_t addr_shifted) {
        out[0] = static_cast<std::uint8_t>(addr_shifted | mb);
        out[1] = out[2] = out[3] = out[4] = 0;
    };

    switch (idx) {
    case 0: {
        // 0x00 general — rate (C1), OC pins (C2), nddc|duplex (C4).
        setAddr(0x00);
        out[1] = sampleRateBits_.load(std::memory_order_relaxed);
        out[2] = ocC2_.load(std::memory_order_relaxed);
        out[3] = 0x00;
        out[4] = 0x1C;  // nddc=4 (0x18) | duplex bit (0x04)
        break;
    }
    case 1: {
        // 0x02 TX freq (BE u32) — cases 1/5/6 all load tx[0].frequency
        // per networkproto1.c; slots 5/6 below mirror it on DDC2/DDC3.
        setAddr(0x02);
        const quint32 f = txFreqHz_.load(std::memory_order_relaxed);
        out[1] = static_cast<std::uint8_t>((f >> 24) & 0xFF);
        out[2] = static_cast<std::uint8_t>((f >> 16) & 0xFF);
        out[3] = static_cast<std::uint8_t>((f >>  8) & 0xFF);
        out[4] = static_cast<std::uint8_t>( f        & 0xFF);
        break;
    }
    case 2: {
        // 0x04 RX1 freq (DDC0) — BE u32.
        setAddr(0x04);
        const quint32 f = rx1FreqHz_.load(std::memory_order_relaxed);
        out[1] = static_cast<std::uint8_t>((f >> 24) & 0xFF);
        out[2] = static_cast<std::uint8_t>((f >> 16) & 0xFF);
        out[3] = static_cast<std::uint8_t>((f >>  8) & 0xFF);
        out[4] = static_cast<std::uint8_t>( f        & 0xFF);
        break;
    }
    case 3: {
        // 0x06 RX2 freq (DDC1) — RX1 freq until RX2 lands.
        setAddr(0x06);
        const quint32 f = rx1FreqHz_.load(std::memory_order_relaxed);
        out[1] = static_cast<std::uint8_t>((f >> 24) & 0xFF);
        out[2] = static_cast<std::uint8_t>((f >> 16) & 0xFF);
        out[3] = static_cast<std::uint8_t>((f >>  8) & 0xFF);
        out[4] = static_cast<std::uint8_t>( f        & 0xFF);
        break;
    }
    case 4: {
        // 0x1C TX step-att — C3 = (31 - db) & 0x1F (HL2 inverted range;
        // networkproto1.c:1019 + console.cs:10658 SetTxAttenData(31-x)).
        setAddr(0x1C);
        const int db = txStepAttnDb_.load(std::memory_order_relaxed);
        out[3] = static_cast<std::uint8_t>((31 - db) & 0x1F);
        break;
    }
    case 5: {
        // 0x08 DDC2 freq = TX freq mirror.
        setAddr(0x08);
        const quint32 f = txFreqHz_.load(std::memory_order_relaxed);
        out[1] = static_cast<std::uint8_t>((f >> 24) & 0xFF);
        out[2] = static_cast<std::uint8_t>((f >> 16) & 0xFF);
        out[3] = static_cast<std::uint8_t>((f >>  8) & 0xFF);
        out[4] = static_cast<std::uint8_t>( f        & 0xFF);
        break;
    }
    case 6: {
        // 0x0a DDC3 freq = TX freq mirror.
        setAddr(0x0a);
        const quint32 f = txFreqHz_.load(std::memory_order_relaxed);
        out[1] = static_cast<std::uint8_t>((f >> 24) & 0xFF);
        out[2] = static_cast<std::uint8_t>((f >> 16) & 0xFF);
        out[3] = static_cast<std::uint8_t>((f >>  8) & 0xFF);
        out[4] = static_cast<std::uint8_t>( f        & 0xFF);
        break;
    }
    case 7:  setAddr(0x0c); break;  // DDC4 unused on HL2
    case 8:  setAddr(0x0e); break;  // DDC5 unused
    case 9:  setAddr(0x10); break;  // DDC6 unused
    case 10: {
        // 0x12 drive (C1) + PA-enable (C2 bit 3 = 0x08, active-high).
        // C2 bit 7 (0x80, VNA) must stay clear or PA won't key
        // (control.v:359 pwr_envpa = int_tx_on & ~vna & pa_enable).
        setAddr(0x12);
        out[1] = static_cast<std::uint8_t>(
            txDriveLevel_.load(std::memory_order_relaxed) & 0xFF);
        const bool pa = paOn_.load(std::memory_order_relaxed);
        out[2] = static_cast<std::uint8_t>(pa ? 0x08 : 0x00);
        out[3] = 0x00;
        out[4] = 0x00;
        break;
    }
    case 11: {
        // 0x14 LNA + MOX-gated TX step-att — C4 encoding follows the
        // existing emitter's RX branch exactly (override bit 0x40 +
        // 6-bit value) so RX cadence is byte-identical to pre-TX-0c.
        // TX branch lands the inverted (31-db) step-att during keydown.
        setAddr(0x14);
        if (mox) {
            const int db = txStepAttnDb_.load(std::memory_order_relaxed);
            out[4] = static_cast<std::uint8_t>(0x40 | ((31 - db) & 0x3F));
        } else {
            const int g = std::clamp(
                lnaGainDb_.load(std::memory_order_relaxed),
                kLnaMinDb, kLnaMaxDb);
            const int v = g + 12;  // HPSDR P1 0x14 +12 bias
            out[4] = static_cast<std::uint8_t>(0x40 | (v & 0x3F));
        }
        break;
    }
    case 12: setAddr(0x16); break;  // ADC step-att (Hermes-II) / CW state
    case 13: setAddr(0x18); break;  // placeholder
    case 14: setAddr(0x1a); break;  // PWM / EER
    case 15: setAddr(0x1e); break;  // CW key / EER pulse width
    case 16: setAddr(0x20); break;  // placeholder
    case 17: setAddr(0x22); break;  // placeholder
    case 18: setAddr(0x24); break;  // placeholder
    default: setAddr(0x00); break;  // defensive; idx is 0..18 by contract
    }
}

void HL2Stream::updateOcPattern() {
    // RX pattern for the current band when the board is enabled, else 0.
    int pattern = 0;
    if (filterBoardEnabled_) {
        const int bi = lyra::bandIndexForFreq(
            static_cast<int>(rx1FreqHz_.load(std::memory_order_relaxed)));
        pattern = lyra::n2adrOcPattern(bi, /*transmitting=*/false) & 0x7F;
    }
    if (pattern == ocPattern_) {
        return;
    }
    ocPattern_ = pattern;
    // C2[7:1] = OC pins; C2[0] (CW key bit) stays 0.  Atomic store so the
    // EP2 writer thread picks it up on the next send.
    ocC2_.store(static_cast<std::uint8_t>((pattern << 1) & 0xFE),
                std::memory_order_relaxed);
    emit ocBitsChanged(pattern);
}

// ----------------------------------------------------------------
// Step 5: producer — the DSP engine hands decoded 48 kHz stereo int16
// here (from the RX worker thread).  Lazily sizes a ~100 ms ring; on
// overflow it drops the oldest frame (the EP2 writer is the clock, so
// a transient producer burst can't stall it).
void HL2Stream::pushAudio(const qint16 *lr, int nframes) {
    {
        std::lock_guard<std::mutex> lk(audioMtx_);
        if (audioCap_ == 0) {
            audioCap_ = 4800;                   // 100 ms @ 48 kHz
            audioBuf_.assign(static_cast<size_t>(audioCap_) * 2, 0);
            audioRd_ = audioWr_ = audioCount_ = 0;
        }
        for (int f = 0; f < nframes; ++f) {
            if (audioCount_ >= audioCap_) {      // overflow: drop oldest
                audioRd_ = (audioRd_ + 1) % audioCap_;
                --audioCount_;
            }
            audioBuf_[static_cast<size_t>(audioWr_) * 2 + 0] = lr[f * 2 + 0];
            audioBuf_[static_cast<size_t>(audioWr_) * 2 + 1] = lr[f * 2 + 1];
            audioWr_ = (audioWr_ + 1) % audioCap_;
            ++audioCount_;
        }
    }
    // Wake the EP2 writer — it sends a datagram as soon as a full 126-
    // frame chunk is ready, so the send cadence follows the HL2 crystal
    // (the audio is derived from the HL2's own IQ) instead of a free-
    // running PC timer.  Single-crystal = no two-clock drift = no pops.
    audioCv_.notify_one();
}

// TX worker — dedicated OS thread, sends one START packet on
// entry then a 1032-byte EP2 keepalive every 2.6 ms (380 Hz)
// using a Win32 HIGH_RESOLUTION waitable timer with drift-
// corrected absolute scheduling.

void HL2Stream::txWorkerLoop(std::stop_token stop, SocketHandle sh,
                              QString ip) {
    const SOCKET s = static_cast<SOCKET>(sh);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(kRadioPort);
    ::inet_pton(AF_INET, ip.toLatin1().constData(), &dest.sin_addr);

    // Send START.  Done from the TX thread (not from main or RX)
    // so every host→radio packet originates from one operation
    // context.  The radio responds with EP6 to whatever socket the
    // start came from — that's the shared socket the RX thread is
    // listening on, so EP6 routing is preserved.
    const QByteArray startPkt = buildControlPacket(true);
    if (::sendto(s, startPkt.constData(), startPkt.size(), 0,
                 reinterpret_cast<sockaddr*>(&dest),
                 sizeof(dest)) != startPkt.size()) {
        const int err = ::WSAGetLastError();
        QMetaObject::invokeMethod(this, [this, err]() {
            onFatalError(QStringLiteral("START: %1")
                         .arg(winsockError(err)));
        }, Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(this, [this]() {
        emit logLine(QStringLiteral(
            "  START sent (0xEFFE 0x04 0x01 + 60 zeros), "
            "EP2 keepalive engaging @380 Hz"));
    }, Qt::QueuedConnection);

    // EP2 writer pacing — Thetis's sendProtocol1Samples model: sends are
    // driven by AUDIO PRODUCTION, not a free-running PC timer.  RX audio
    // is decoded from the HL2's own IQ (its crystal), so sending one
    // datagram per 126-frame chunk AS IT'S PRODUCED locks the EP2 cadence
    // to the HL2 clock — single crystal, no two-clock drift, no codec
    // FIFO under/overrun (the crackle/pop the timer approach caused).
    // When no audio flows (PC-output mode, or a real underrun) a silence
    // keepalive maintains the gateware's EP2 watchdog.
    //
    // HIGHEST priority + a 1 ms scheduler tick keep send jitter low
    // (Thetis runs this thread at MMCSS "Pro Audio").
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    ::timeBeginPeriod(1);

    // Pre-allocated template — per-send we patch seq + freq + OC + audio.
    QByteArray pkt = buildEp2KeepaliveTemplate();
    auto *pktBytes = reinterpret_cast<std::uint8_t*>(pkt.data());

    // Pack the per-datagram fields and send one EP2 datagram.  `audio` is
    // 126 stereo frames, or nullptr for a silence keepalive.
    auto sendDatagram = [&](const qint16 *audio) {
        const quint32 seq = txSeq_.fetch_add(1, std::memory_order_relaxed);
        pktBytes[4] = static_cast<std::uint8_t>((seq >> 24) & 0xFF);
        pktBytes[5] = static_cast<std::uint8_t>((seq >> 16) & 0xFF);
        pktBytes[6] = static_cast<std::uint8_t>((seq >>  8) & 0xFF);
        pktBytes[7] = static_cast<std::uint8_t>( seq        & 0xFF);

        // TX-0c-emit: HL2 19-slot round-robin C&C cycle, one slot per
        // USB frame (two slots per datagram).  Matches Thetis
        // WriteMainLoop_HL2 cadence (1 slot per USB frame, ~760/s on EP2);
        // each slot revisits at ~40 Hz, plenty responsive for VFO/LNA/PA
        // state.  ccIdx_ is single-thread owned by the EP2 writer thread
        // (no atomic).  MOX is snapshotted once per datagram so both USB
        // frames carry coherent C0 bit-0.  WIRE-IDENTICAL TO PRE-TX-0c
        // AT MOX=0 / PA-off (see composeCC header doc).
        const bool moxBit = mox_.load(std::memory_order_relaxed);
        std::uint8_t cc[5];

        // USB frame 1 C&C slot (offsets 11..15 = C0..C4).
        composeCC(ccIdx_, moxBit, cc);
        pktBytes[11] = cc[0];
        pktBytes[12] = cc[1];
        pktBytes[13] = cc[2];
        pktBytes[14] = cc[3];
        pktBytes[15] = cc[4];
        ccIdx_ = (ccIdx_ + 1) % 19;

        // USB frame 2 C&C slot (offsets 523..527 = C0..C4).
        composeCC(ccIdx_, moxBit, cc);
        pktBytes[523] = cc[0];
        pktBytes[524] = cc[1];
        pktBytes[525] = cc[2];
        pktBytes[526] = cc[3];
        pktBytes[527] = cc[4];
        ccIdx_ = (ccIdx_ + 1) % 19;

        // Audio L/R into the 126 LRIQ tuples (BE 16-bit); TX I/Q stay 0.
        for (int i = 0; i < 126; ++i) {
            const qint16 L = audio ? audio[i * 2 + 0] : 0;
            const qint16 R = audio ? audio[i * 2 + 1] : 0;
            const int base = (i < 63) ? (16 + i * 8) : (528 + (i - 63) * 8);
            pktBytes[base + 0] = static_cast<std::uint8_t>((L >> 8) & 0xFF);
            pktBytes[base + 1] = static_cast<std::uint8_t>( L       & 0xFF);
            pktBytes[base + 2] = static_cast<std::uint8_t>((R >> 8) & 0xFF);
            pktBytes[base + 3] = static_cast<std::uint8_t>( R       & 0xFF);
        }

        const int n = ::sendto(s, pkt.constData(), pkt.size(), 0,
                               reinterpret_cast<sockaddr*>(&dest),
                               sizeof(dest));
        if (n != pkt.size()) {
            txSendErrors_.fetch_add(1, std::memory_order_relaxed);
        } else {
            txTotalDg_.fetch_add(1, std::memory_order_relaxed);
            txWindowDg_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    qint16 chunk[126 * 2];
    while (!stop.stop_requested()) {
        const bool injecting = injectAudio_.load(std::memory_order_relaxed);
        bool haveChunk = false;
        {
            std::unique_lock<std::mutex> lk(audioMtx_);
            // When injecting, wait LONGER than the gap between DSP audio
            // blocks so a normal inter-block gap never trips a (silence)
            // keepalive into the stream — only a genuine underrun does.
            // That gap is rate-dependent: a 1024-frame IQ block arrives
            // every ~10.7 ms at 96 k (worst case), ~5.3 ms at 192 k,
            // ~2.7 ms at 384 k.  22 ms clears the 96 k gap with margin
            // (was 10 ms → 96 k injected silence every block = clicks).
            // When NOT injecting (PC out / idle), wait ~2.6 ms so the
            // silence keepalive holds ~380 Hz.
            audioCv_.wait_for(
                lk,
                injecting ? std::chrono::microseconds(22000)
                          : std::chrono::microseconds(2600),
                [&] {
                    return stop.stop_requested() ||
                           (injectAudio_.load(std::memory_order_relaxed) &&
                            audioCount_ >= 126);
                });
            if (stop.stop_requested()) break;
            if (injectAudio_.load(std::memory_order_relaxed) &&
                audioCount_ >= 126) {
                for (int i = 0; i < 126; ++i) {
                    chunk[i * 2 + 0] =
                        audioBuf_[static_cast<size_t>(audioRd_) * 2 + 0];
                    chunk[i * 2 + 1] =
                        audioBuf_[static_cast<size_t>(audioRd_) * 2 + 1];
                    audioRd_ = (audioRd_ + 1) % audioCap_;
                    --audioCount_;
                }
                haveChunk = true;
            }
        }
        // Audio chunk (producer-paced) or silence keepalive (timeout).
        sendDatagram(haveChunk ? chunk : nullptr);
    }

    ::timeEndPeriod(1);
}

} // namespace lyra::ipc
