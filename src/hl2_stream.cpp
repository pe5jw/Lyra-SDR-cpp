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
#include <iphlpapi.h>    // GetUdpStatisticsEx (Task #48 diagnostic)

#include <QByteArray>
#include <QDebug>           // qInfo / qCritical for safety-event mirror
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
    // 32-bit freq in Hz, big-endian (per the HL2 wire-protocol
    // reference, case 2; see hl2_stream.h header doc for cites).
    // The writer overwrites C1..C4 from rx1FreqHz_ every
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

// Task #48 — snapshot the Windows system-wide UDPv4 RX counters
// via GetUdpStatisticsEx.  Returns true on success and fills the
// three outputs; returns false on any failure (caller writes
// sentinel -1).  Read-only, ~microsecond cost — safe to call from
// start/close.  Counters are SYSTEM-WIDE (Windows doesn't expose
// per-socket UDP drop counts), so other UDP apps on the box (DNS
// lookups, mDNS, browser, etc.) move them too — interpret deltas
// IN CONTEXT, alongside Lyra's per-session totalDg_/seqErrors_.
//
// Field mapping (per Microsoft docs + Windows TCP/IP-stack
// behaviour):
//   dwInDatagrams — total UDP datagrams delivered by the IP layer
//                   to the kernel UDP code (system-wide).
//   dwNoPorts     — datagrams arriving for an unlistened port.
//                   For a healthy Lyra session this should be ~0
//                   for HL2 traffic; a spike could mean HL2 sent
//                   after we closed the socket (timing diagnostic
//                   for clean shutdown).
//   dwInErrors    — datagrams that arrived BUT the kernel could
//                   not deliver to any socket for reasons OTHER
//                   than no-listener.  On Windows this INCLUDES
//                   socket-receive-buffer-full discards.  THIS is
//                   the counter that should move if our 4 MiB
//                   SO_RCVBUF is overflowing because Lyra's RX
//                   thread stalled.
//
// Interpreting the delta at close():
//   * Δ dwInErrors  > 0  AND  seqErrors > 0
//     → drops in the kernel UDP layer.  Lyra RX thread stalled,
//       socket buffer filled, kernel dropped packets.  Look at
//       Lyra-side: thread starvation, scheduler stall, paint
//       storm, etc.
//   * Δ dwInErrors == 0  AND  seqErrors > 0
//     → kernel saw NO problem; drops happened UPSTREAM of the
//       kernel.  Look at: NIC driver receive ring (Device
//       Manager → NIC → Properties → Advanced → "Receive
//       Buffers" → bump to max; "Interrupt Moderation" → off),
//       NIC hardware, switch in the path, EMI on the cable.
//   * Δ dwInDatagrams ≈ Lyra's totalDg_  (within a few % from
//     unrelated apps' traffic): sanity check that the system
//     stats are coherent with our session.
bool snapshotUdpStatsV4(qint64 *inDatagrams,
                        qint64 *inNoPorts,
                        qint64 *inErrors) {
    MIB_UDPSTATS stats{};
    const DWORD rc = ::GetUdpStatisticsEx(&stats, AF_INET);
    if (rc != NO_ERROR) {
        return false;
    }
    if (inDatagrams) *inDatagrams = static_cast<qint64>(stats.dwInDatagrams);
    if (inNoPorts)   *inNoPorts   = static_cast<qint64>(stats.dwNoPorts);
    if (inErrors)    *inErrors    = static_cast<qint64>(stats.dwInErrors);
    return true;
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
    const quint32 persistedRxHz =
        QSettings().value(QStringLiteral("rx/freqHz"), 7074000u).toUInt();
    rx1FreqHz_.store(persistedRxHz, std::memory_order_relaxed);
    // TX-0c-tune — simplex TX follows RX1 from launch (not just on
    // operator dial moves).  Without this, the persistence restore
    // bypasses setRx1FreqHz() so txFreqHz_ sits at its 7,074,000
    // default until the operator nudges the dial — and TUN lands the
    // carrier "several kHz below" the marker (operator-caught, 2026-05-28).
    // Single source of truth: every other write to rx1FreqHz_ goes
    // through setRx1FreqHz(), which mirrors there too.
    txFreqHz_.store(persistedRxHz, std::memory_order_relaxed);
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
    // defaults match the operator's working station export
    // (auto-step-att=True, auto-att-undo-RX1=True,
    // hold-RX1=4): Auto ON / undo ON / 4 s hold.
    autoLnaEnabled_ = QSettings().value(QStringLiteral("rx/autoLna"), true).toBool();
    autoLnaUndo_    = QSettings().value(QStringLiteral("rx/autoLnaUndo"), true).toBool();
    autoLnaHoldSec_ = std::clamp(
        QSettings().value(QStringLiteral("rx/autoLnaHoldSec"), 4).toInt(), 1, 60);

    // TX-0c-pa-debug — host-side safety timeout state.  Persisted so an
    // operator-set 5-min timeout survives restarts.  Bypass starts false
    // every launch unless explicitly persisted as true (no surprise
    // "safety is off" on a fresh launch).
    txTimeoutSec_ = std::clamp(
        QSettings().value(QStringLiteral("tx/timeoutSeconds"),
                          kTxTimeoutDefaultSec).toInt(),
        kTxTimeoutMinSec, kTxTimeoutMaxSec);
    txTimeoutBypass_ = QSettings().value(
        QStringLiteral("tx/timeoutBypass"), false).toBool();
    // PA enable is a PERSISTENT OPERATOR PREFERENCE (operator decision
    // 2026-05-29).  Restored across Lyra launches AND across stream
    // Stop/Start cycles within a session — what the operator last
    // explicitly chose on the Settings checkbox is what comes up.  The
    // cb58bcb come-up-not-keyed safety still applies to MOX, tune,
    // requestedMox_, and FSM state (those address a real come-up-
    // keyed bug); PA bias alone — without MOX — emits no carrier, so
    // a relaunch with PA preserved is safe (the operator must still
    // take a MOX action to put RF on the air, and the safety timer +
    // gateware watchdog cover the held-MOX and crashed-mid-key cases
    // that the original PA defensive clear was over-conservatively
    // also guarding against).
    paOn_.store(
        QSettings().value(QStringLiteral("tx/paEnabled"), false).toBool(),
        std::memory_order_relaxed);
    // TX-0c-pa-drive — operator-tunable drive DAC level (raw 0..255,
    // UI exposes 0..100 %).  Persisted across launches; NOT cleared on
    // stream open/close — the "volume" knob carries the operator's
    // intentional set point.  PA enable is the separate safety gate
    // (defensively cleared on every open/close); drive at any value
    // without PA + MOX produces zero RF, so persisting it is safe.
    // Default 0 on first-ever launch (operator must explicitly raise it
    // to emit a carrier).
    txDriveLevel_.store(
        std::clamp(QSettings().value(QStringLiteral("tx/driveLevel"),
                                     0).toInt(), 0, 255),
        std::memory_order_relaxed);
    // TX-1 component 5a — load operator-tuned TR-sequencing + fade
    // durations from QSettings (tx/trSeq/<key>).  Defaults match the
    // operator's working-station HL2+ DB export (15/50/13/5 ms) which
    // is hot-switch-safe for typical external SS HF linears; fade
    // defaults (50 ms in / 13 ms out) align with rfDelayMs_ and
    // spaceMoxDelayMs_ respectively.  Clamped to [kMinFsmDelayMs,
    // kMaxFsmDelayMs] on load to defend against stale / corrupt
    // QSettings (e.g. an operator hand-edited the file).
    moxDelayMs_ = std::clamp(
        QSettings().value(QStringLiteral("tx/trSeq/moxDelayMs"),
                          kDefaultMoxDelayMs).toInt(),
        kMinFsmDelayMs, kMaxFsmDelayMs);
    rfDelayMs_ = std::clamp(
        QSettings().value(QStringLiteral("tx/trSeq/rfDelayMs"),
                          kDefaultRfDelayMs).toInt(),
        kMinFsmDelayMs, kMaxFsmDelayMs);
    spaceMoxDelayMs_ = std::clamp(
        QSettings().value(QStringLiteral("tx/trSeq/spaceMoxDelayMs"),
                          kDefaultSpaceMoxDelayMs).toInt(),
        kMinFsmDelayMs, kMaxFsmDelayMs);
    pttOutDelayMs_ = std::clamp(
        QSettings().value(QStringLiteral("tx/trSeq/pttOutDelayMs"),
                          kDefaultPttOutDelayMs).toInt(),
        kMinFsmDelayMs, kMaxFsmDelayMs);
    txStopDelayMs_ = std::clamp(
        QSettings().value(QStringLiteral("tx/trSeq/txStopDelayMs"),
                          kDefaultTxStopDelayMs).toInt(),
        kMinFsmDelayMs, kMaxFsmDelayMs);
    fade_.setFadeInMs(
        QSettings().value(QStringLiteral("tx/trSeq/fadeInMs"),
                          lyra::dsp::MoxEdgeFade::kDefaultFadeInMs).toInt());
    fade_.setFadeOutMs(
        QSettings().value(QStringLiteral("tx/trSeq/fadeOutMs"),
                          lyra::dsp::MoxEdgeFade::kDefaultFadeOutMs).toInt());
    // TX-1 component 8a — operator-tunable WDSP TXA gain stages.
    // micGainDb defaults to 0 dB = WDSP unity (no change vs the lyra-
    // cpp ship-no-setters posture at TxChannel::open()).  alcMaxGainDb
    // defaults to +3 dB which mirrors the verified reference's
    // Setup-load default — THE smoking-gun fix for the 2026-05-31
    // 0.2 W first-SSB bench (WDSP create-time is 0 dB, which pins the
    // TXA output chain at the ALC ceiling regardless of mic level).
    // Push-to-channel happens in registerTxControl() once the
    // TxControl callbacks are wired (the channel is open by then; the
    // operator's persisted/default values land on the channel before
    // the first keydown).  Clamped on load to defend against stale /
    // corrupt QSettings (hand-edited file).
    micGainDb_ = std::clamp(
        QSettings().value(QStringLiteral("tx/micGainDb"),
                          kDefaultMicGainDb).toDouble(),
        kMinMicGainDb, kMaxMicGainDb);
    alcMaxGainDb_ = std::clamp(
        QSettings().value(QStringLiteral("tx/alcMaxGainDb"),
                          kDefaultAlcMaxGainDb).toDouble(),
        kMinAlcMaxGainDb, kMaxAlcMaxGainDb);
    txSafetyTimer_ = new QTimer(this);
    txSafetyTimer_->setSingleShot(true);
    connect(txSafetyTimer_, &QTimer::timeout,
            this, &HL2Stream::onTxSafetyTimeout);
    // Self-wire keydown/keyup edges to arm/cancel the safety timer.
    // Same-thread queued not needed — moxActiveChanged fires from this
    // QObject's thread (the FSM steps run via QTimer::singleShot here).
    connect(this, &HL2Stream::moxActiveChanged, this, [this](bool on) {
        if (on)  armTxSafetyTimer();
        else     cancelTxSafetyTimer();
    });
    // TX-0c-tune — auto-disarm the tune-tone on every wire-MOX-off edge.
    // Whatever caused the keyup (operator click, safety timer, FSM
    // re-key collapse), the next time MOX rises the operator must
    // explicitly re-click TUN to emit a carrier.  Prevents a stray
    // MOX-only click from putting unintended RF on the antenna.
    connect(this, &HL2Stream::moxActiveChanged, this, [this](bool on) {
        if (!on && tuneEnabled_.load(std::memory_order_relaxed)) {
            setTuneEnabled(false);
        }
    });
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

// ── Safety-event log mirror ─────────────────────────────────────────
// emit logLine() (UI log dock) AND qInfo() / qCritical() (stderr +
// any installed file handler).  Used at the TX-safety surface so an
// operator-visible "what happened, when?" record survives a crash or
// a session where the in-app log dock was never opened.  The "[hl2]"
// tag is kept distinct from the existing "[hl2]" prefix on LNA logs
// (those stay logLine-only — not TX-safety).
void HL2Stream::safetyLog(const QString& msg) {
    emit logLine(msg);
    qInfo().noquote() << "[hl2-safety]" << msg;
}

void HL2Stream::fatalLog(const QString& msg) {
    emit logLine(msg);
    qCritical().noquote() << "[hl2-fatal]" << msg;
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

    // Task #48 — baseline the system-wide UDPv4 counters so the
    // close() log line can subtract and print the per-session
    // delta.  Snapshot is read-only + microsecond-cost; never
    // fails on any sane Windows install but we keep the -1
    // sentinel for paranoia / future cross-platform builds.
    {
        qint64 rx = -1, np = -1, er = -1;
        if (snapshotUdpStatsV4(&rx, &np, &er)) {
            udpStartInDatagrams_.store(rx);
            udpStartInNoPorts_.store(np);
            udpStartInErrors_.store(er);
        } else {
            udpStartInDatagrams_.store(-1);
            udpStartInNoPorts_.store(-1);
            udpStartInErrors_.store(-1);
        }
    }
    txTotalDg_.store(0);  txWindowDg_.store(0);
    txSendErrors_.store(0);  txSeq_.store(0);
    rx1DbFs_.store(-200.0, std::memory_order_relaxed);
    dgPerSec_   = 0.0;
    txDgPerSec_ = 0.0;
    // cb58bcb-style come-up-not-keyed safety: every stream open starts
    // with MOX = off, FSM idle, tune disarmed.  Fixes the original
    // lock-MOX → Stop → Start → come-up-keyed bug.  PA enable is NO
    // LONGER force-cleared here (operator decision 2026-05-29): it is
    // a persistent operator preference, restored from QSettings in the
    // ctor.  PA bias alone without MOX produces no carrier — a fresh
    // open with paOn_ preserved emits the gateware bias bits but no
    // RF until the operator takes a deliberate MOX action.  Safety
    // timer + gateware watchdog cover the held-MOX and crashed-mid-
    // key cases that the original PA defensive clear was over-
    // conservatively also guarding against.
    mox_.store(false, std::memory_order_release);
    // TX-0c-tune — fresh stream always starts disarmed (tune is per-
    // session, not persisted).  DC-injection carrier means no NCO
    // state to reset beyond this flag.
    tuneEnabled_.store(false, std::memory_order_relaxed);
    requestedMox_  = false;
    fsmRunning_    = false;
    if (moxActive_) { moxActive_ = false; emit moxActiveChanged(false); }
    if (txSafetyTimer_ && txSafetyTimer_->isActive()) txSafetyTimer_->stop();
    emit tuneEnabledChanged(false);// TX panel UI follows
    // Mirror the persisted PA-enable into the UI on every open, so the
    // Settings checkbox accurately reflects the live atomic state even
    // after a previous close() that pre-2026-05-29 used to clobber it.
    // (Idempotent: setChecked checks differ-from-current before firing.)
    emit paEnabledChanged(paOn_.load(std::memory_order_relaxed));
    running_.store(true, std::memory_order_release);
    emit runningChanged();
    emit statsChanged();
    emit logLine(QStringLiteral(
        "opening EP6 stream to %1:%2 (local port %3) ...")
        .arg(ip).arg(kRadioPort).arg(lport));
    // Record the cb58bcb come-up-not-keyed defensive clears (above) so
    // a captured stderr log reconstructs "session N came up RX, MOX +
    // tune cleared, PA at <persisted-state>" without needing the in-app
    // log dock.  Per the 2026-05-29 posture relax, PA enable is a
    // persistent preference — the log records what it came up as so an
    // operator reading the captured log can tell whether the PA bias
    // would engage on the first MOX keydown.
    safetyLog(QStringLiteral(
        "come-up-not-keyed: MOX + tune force-cleared on stream open; "
        "PA enable preserved at %1")
        .arg(paOn_.load(std::memory_order_relaxed)
             ? QStringLiteral("ON")
             : QStringLiteral("off")));

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
    // Task #40 — TX-triggered zombie shutdown investigation.  qWarning
    // brackets so the next bench shows in lyra-log.txt which step (if
    // any) wedged: rxWorker_.join, txWorker_.join, sendto STOP, or the
    // closesocket itself.
    qWarning("[shutdown] HL2Stream::close ENTRY (running=%d rxJoin=%d txJoin=%d sockOpen=%d)",
             running_.load() ? 1 : 0,
             rxWorker_.joinable() ? 1 : 0,
             txWorker_.joinable() ? 1 : 0,
             socket_ != kInvalidSocket ? 1 : 0);
    if (!running_.load(std::memory_order_acquire) &&
        !rxWorker_.joinable() &&
        !txWorker_.joinable() &&
        socket_ == kInvalidSocket) {
        qWarning("[shutdown] HL2Stream::close NO-OP (nothing alive)");
        return;
    }
    emit logLine(QStringLiteral("closing EP6 stream ..."));
    // Record the force-release-all (below) so a captured stderr log
    // shows "session N ended RX, MOX/tune dropped before the workers
    // were joined."  PA enable is preserved (operator preference) and
    // the gateware watchdog drops PA bias ~13 s after the EP2 stream
    // stops, so this is not a stuck-on-bias concern.
    safetyLog(QStringLiteral(
        "force-release-all: MOX + tune cleared before workers stop "
        "(PA preserved; gateware watchdog drops bias on EP2 timeout)"));

    // Force MOX off + tune off BEFORE the workers die so the final
    // EP2 frames carry MOX=0 + step-att restored + TX I/Q silent — no
    // stuck carrier even if the operator was mid-key when Stop was
    // clicked.  PA enable is intentionally NOT touched here (2026-05-
    // 29 posture relax — it's a persistent operator preference; bias
    // alone without MOX produces no carrier, and the gateware watchdog
    // drops bias on EP2 timeout if the operator wants the radio fully
    // safed they uncheck PA in Settings).  mox_ atomic is set directly
    // here (bypassing the FSM's TR-delay chain) because the workers
    // are about to die — there's no time for a graceful keyup.
    mox_.store(false, std::memory_order_release);
    // TX-0c-tune — force-disarm in lockstep with the MOX clear so the
    // final EP2 frames carry silent TX I/Q.
    tuneEnabled_.store(false, std::memory_order_relaxed);
    requestedMox_  = false;
    fsmRunning_    = false;
    if (moxActive_) {
        moxActive_ = false;
        emit moxActiveChanged(false);
        // OC pattern back to RX: forced close mid-TX would otherwise
        // leave the filter board in TX configuration after stop.  The
        // final EP2 frame carries the RX pattern alongside MOX=0 + PA
        // disarmed — symmetric with the FSM keyup restore in
        // fsmKeyupSettled.
        updateOcPattern(/*transmitting=*/false);
    }
    if (txSafetyTimer_ && txSafetyTimer_->isActive()) txSafetyTimer_->stop();
    emit tuneEnabledChanged(false);// TX panel UI follows
    // paEnabledChanged is intentionally NOT emitted here — paOn_ is
    // not touched in close() (per the 2026-05-29 persistence posture),
    // so the Settings checkbox stays at the operator's last explicit
    // setting across a Stop → next Start cycle.

    // Request stop on both workers BEFORE joining either so they
    // wind down in parallel (RX: bounded by recv timeout 100 ms,
    // TX: bounded by waitable-timer wait cap 100 ms).
    qWarning("[shutdown] HL2Stream::close request_stop on rx+tx workers");
    if (rxWorker_.joinable()) rxWorker_.request_stop();
    if (txWorker_.joinable()) txWorker_.request_stop();
    qWarning("[shutdown] HL2Stream::close rxWorker_.join() - start");
    if (rxWorker_.joinable()) rxWorker_.join();
    qWarning("[shutdown] HL2Stream::close rxWorker_.join() - done");
    qWarning("[shutdown] HL2Stream::close txWorker_.join() - start");
    if (txWorker_.joinable()) txWorker_.join();
    qWarning("[shutdown] HL2Stream::close txWorker_.join() - done");

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

    // Task #48 — Windows system-wide UDPv4 delta for this session.
    // Lets the operator (and us) tell whether seq-error drops are
    // happening in the kernel UDP layer (Δ udpInErrors > 0 → Lyra
    // RX thread stalled, rcvbuf overflowed) or upstream of the
    // kernel (Δ udpInErrors == 0 while seqErrors > 0 → drops are
    // in the NIC ring / driver / hardware — operator-side fix via
    // Device Manager → NIC → Advanced → Receive Buffers ↑ +
    // Interrupt Moderation off).  Counters are SYSTEM-WIDE; other
    // UDP apps (DNS, mDNS, browser) inflate the totals — interpret
    // alongside Lyra-side numbers, not in isolation.
    {
        const qint64 startRx = udpStartInDatagrams_.load();
        const qint64 startNp = udpStartInNoPorts_.load();
        const qint64 startEr = udpStartInErrors_.load();
        qint64 endRx = -1, endNp = -1, endEr = -1;
        const bool ok = snapshotUdpStatsV4(&endRx, &endNp, &endEr);
        if (ok && startRx >= 0 && startNp >= 0 && startEr >= 0) {
            const qint64 dRx = endRx - startRx;
            const qint64 dNp = endNp - startNp;
            const qint64 dEr = endEr - startEr;
            emit logLine(QStringLiteral(
                "udp4 stats Δ (system-wide): inDatagrams=%1  "
                "noPorts=%2  inErrors=%3   (vs Lyra seqErrors=%4)")
                .arg(dRx).arg(dNp).arg(dEr)
                .arg(seqErrors_.load()));
            // Mirror to qInfo so it lands in the captured stderr
            // log too (the operator's lyra-log.txt path).
            qInfo("[udp4 stats] Δ inDatagrams=%lld noPorts=%lld "
                  "inErrors=%lld  (vs Lyra seqErrors=%lld)",
                  static_cast<long long>(dRx),
                  static_cast<long long>(dNp),
                  static_cast<long long>(dEr),
                  static_cast<long long>(seqErrors_.load()));
        } else {
            emit logLine(QStringLiteral(
                "udp4 stats Δ: snapshot unavailable "
                "(start=%1/%2/%3 end ok=%4)")
                .arg(startRx).arg(startNp).arg(startEr)
                .arg(ok ? 1 : 0));
        }
    }
    qWarning("[shutdown] HL2Stream::close EXIT");
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
    fatalLog(QStringLiteral("FATAL: %1").arg(reason));
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
    // verified against ak4951v4 RTL + the HL2 wire-protocol C
    // reference; see hl2_stream.h header doc for file:line cites):
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

    // ---- Q6.5 mic bench instrument -----------------------------
    // Mirrors the rx1DbFs accumulator for the EP6 mic byte stream
    // (slot bytes [24..25] at nddc=4 = 16-bit BE signed PCM, see
    // design doc §3.2).  Decode-only: we already iterate every
    // slot for the IQ extraction above, so reading the trailing
    // 2 bytes is free.  Window = 9600 mic samples = 50 ms at the
    // nddc=4 wire mic rate (~192 k mic samples/sec; the AK4951's
    // 48 kHz codec audio is delivered repeated to fill each slot,
    // so the wire rate equals the per-DDC sample rate).  At this
    // accumulator scale a quiet shack reads near -90 dBFS, normal
    // speech ~-30 to -10 dBFS, hot mic ~0 dBFS.  Operator runs
    // with a mic plugged in, watches the periodic "mic = ..." log
    // line below, talks → number rises = Q6.5 bench PASS.  No DSP
    // wiring; TX-1 will hook this into Hl2Ep6MicSource later.
    double micAcc                    = 0.0;
    int    micAccCount               = 0;
    constexpr int kMicWindowSamples  = 9600;       // 50 ms @ ~192 k
    constexpr double kInv2Pow15      = 1.0 / 32768.0;
    // Throttle the operator-visible mic-level log to ~1 Hz so it
    // doesn't drown the safety log.  rx1 dBFS publishes 20×/sec
    // to the QML banner; the mic instrument only needs a slower
    // human-readable trace for the bench check.
    int    micLogCounter             = 0;
    constexpr int kMicLogEveryWindows = 20;        // 20 × 50 ms = ~1 s

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
        // HL2 wire-protocol read-loop reference.
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

        // ---- Task #36: Hardware PTT input forwarder ---------------
        // EP6 status C0 bit 0 = `ptt_in` (the HL2's HW PTT pin
        // state — foot switch / hand-mic PTT / etc.).  Decoded
        // every datagram from frame-0 C0 (the address rotation in
        // bits[7:3] doesn't affect bit 0; the I2C-readback flag
        // bit 7 leaves bit 0 still meaningful per the verified
        // HL2 read-loop semantics).
        //
        // The forwarder is GATED on the operator opt-in atomic.
        // When disabled (default), we pure-decode the level into
        // `lastPttIn_` (so the edge detector is hot for an instant
        // enable) but never emit — wire-identical to a pre-Task-#36
        // build with respect to MOX state changes.
        //
        // When enabled, an edge (low→high or high→low) on ptt_in
        // dispatches `requestMox(bool)` via QueuedConnection so
        // the FSM runs on its owning thread (this QObject's),
        // exactly like the QML MOX button.  Level-driven semantics
        // match the verified HL2 reference + every working host
        // app — debounce is the C&C cadence (~5 kHz EP6 frames at
        // 192 k/nddc=4) + the set_source idempotency in the FSM,
        // NOT a host-side timer; if a real chatter problem shows
        // up on a specific foot-switch the fix lands at the Radio
        // adapter layer, never inside the FSM.
        {
            const std::uint8_t c0_f0 = u[11];  // frame 0 C0
            const bool pttNow = (c0_f0 & 0x01) != 0;
            if (pttNow != lastPttIn_) {
                lastPttIn_ = pttNow;
                if (hwPttEnabled_.load(std::memory_order_acquire)) {
                    // QueuedConnection: the rx-loop thread is NOT
                    // this QObject's owning thread (rx worker is a
                    // std::jthread spawned in open()).  Qt routes
                    // the call to the FSM's thread; no race with
                    // the FSM's single-thread state mutations.
                    // Task #33 — Thetis-faithful PTT-source tagging:
                    // route HW PTT through the source-tagged wrapper
                    // so subscribers know "this MOX edge came from
                    // the foot switch / hand mic / mic button" vs
                    // the operator's software MOX button.
                    QMetaObject::invokeMethod(
                        this, "requestMoxFromHwPtt",
                        Qt::QueuedConnection,
                        Q_ARG(bool, pttNow));
                }
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
        // TX Component 3 — per-datagram decimated mic scratch.
        // Max possible decimated count = 38 (when decimation_factor=1
        // at 48 k wire — Lyra doesn't ship 48 k IQ today, but we size
        // for the worst case so a future config change can't overrun).
        float  micScratch[kFramesPerDatagram];
        int    micIdx = 0;
        // Snapshot the decimation factor once per datagram.  The atomic
        // is written by setSampleRate() (main thread); reading it once
        // up front keeps the per-slot inner loop pure scalar arithmetic.
        const int micDecFactor = micDecimationFactor_.load(
            std::memory_order_relaxed);
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

                // Q6.5 mic bench — bytes [24..25] of this 26-byte
                // slot at nddc=4 (per design v2 §3.2; verified vs
                // ak4951v4 RTL + the HL2 wire-protocol reference).
                // 16-bit BE signed PCM, normalized to [-1, +1).
                const std::int16_t micRaw = static_cast<std::int16_t>(
                    (static_cast<std::uint16_t>(sb[24]) << 8) |
                     static_cast<std::uint16_t>(sb[25]));
                const double micVal =
                    static_cast<double>(micRaw) * kInv2Pow15;
                micAcc += micVal * micVal;
                // TX Component 3 — decimate wire-rate mic samples to
                // 48 kHz and stage in micScratch.  Matches the C
                // reference's `mic_decimation_count`/`mic_decimation_factor`
                // pattern verbatim (counter persists across datagrams,
                // resets only on rate change).  Emit only every Nth
                // wire sample where N = factor (96k→2, 192k→4, 384k→8).
                if (++micDecimationCount_ >= micDecFactor) {
                    micDecimationCount_ = 0;
                    if (micIdx < kFramesPerDatagram) {
                        micScratch[micIdx++] = static_cast<float>(micVal);
                    }
                }
                if (++micAccCount >= kMicWindowSamples) {
                    const double meanSqM =
                        micAcc / static_cast<double>(kMicWindowSamples);
                    const double micDb = (meanSqM > 0.0)
                        ? 10.0 * std::log10(meanSqM)
                        : -200.0;
                    micDbFs_.store(micDb, std::memory_order_relaxed);
                    micAcc      = 0.0;
                    micAccCount = 0;
                    if (++micLogCounter >= kMicLogEveryWindows) {
                        // Use safetyLog so it's both in-session log + qInfo
                        // (operator can see it in the dock OR in the
                        // Windows event/console stream).  Throttled to
                        // ~1 Hz to avoid drowning the safety log.
                        safetyLog(QStringLiteral("Q6.5 mic bench: %1 dBFS")
                                  .arg(micDb, 0, 'f', 1));
                        micLogCounter = 0;
                    }
                }
            }
        }
        // Step 3d: hand this datagram's IQ to the DSP engine inline on
        // this RX worker thread (no Qt signal / no cross-thread queue).
        if (iqSink_) {
            iqSink_(iqScratch, kFramesPerDatagram);
        }
        // TX Component 3 — analogue of the C reference's `Inbound(inid(1,0),
        // mic_sample_count, prn->TxReadBufp)` call at end-of-datagram.
        // Synchronous on this RX worker thread; the consumer (Component 4
        // worker, eventually) is responsible for being lock-free / fast.
        //
        // micConsumerMtx_ is the C reference's csIN analog (cmbuffs.c:97):
        // held around the WHOLE read+call so a concurrent setMicConsumer
        // (e.g. teardown clearing the consumer) cannot tear the captured
        // state out from under an in-flight call.  The lock is held only
        // for the duration of the consumer call, which pushes a small
        // sample block into a lock-free ring downstream — bounded
        // microseconds, no contention in steady state.
        {
            std::lock_guard<std::mutex> lk(micConsumerMtx_);
            if (micConsumer_ && micIdx > 0) {
                micConsumer_(micScratch, micIdx);
            }
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
        // TX-0c-tune — simplex TX follows RX1.  Until SPLIT mode lands,
        // TX freq mirrors RX1 freq, so the operator's TUN carrier lands
        // ~1 kHz above the dial freq instead of the 7,074,000 default
        // that txFreqHz_ would otherwise sit at forever.  When SPLIT
        // arrives this auto-mirror gets gated on !split (VFO B drives
        // TX freq in split mode).  Single source of truth: setRx1FreqHz
        // is the only operator-facing tuner, so this captures every
        // dial gesture, band button, memory recall, TCI spot click, etc.
        txFreqHz_.store(hz, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------
// TX-0b: RF-transmit state setters (foundation; WIRE-INERT).
// Each clamps + stores the HL2+ byte encoding for its TX C&C frame
// (per the HL2 wire-protocol C reference + ak4951v4 gateware decode;
// see hl2_stream.h header doc for the per-register map + file:line
// cites).  NONE are read by the EP2 emission loop
// yet, so at MOX=0 / PA-off the datagram stays byte-identical to RX.
// A later TX phase wires these into the C&C round-robin under MOX gating.

void HL2Stream::setMox(bool on) {
    // Low-level: forces the wire MOX atomic directly, bypassing the
    // TR-sequenced FSM.  Prefer requestMox() for operator/CAT paths.
    // This stays in place for the FSM's own internal use + future
    // dev/diag tools.
    const bool prev = mox_.exchange(on, std::memory_order_relaxed);
    if (prev != on) {
        // Raw setter bypasses the FSM's TR-delay chain — if anything
        // ever calls this in production, the operator needs to know.
        // Diag/test tools use it deliberately; nothing else should.
        safetyLog(QStringLiteral("TX: MOX bit (raw setter) -> %1")
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
    // Lands C1 of slot 10 (frame 0x12) — the drive DAC level.  Gateware
    // uses the top 4 bits → 16 coarse steps; wire 0..255 maps the
    // operator's 0..100 % linearly via the UI before reaching here.
    //
    // At drive=0 the wire is byte-identical to TX-0c-pa-debug B-pa
    // regardless of PA enable / MOX state: zero amplitude on the DAC
    // means no carrier even when the PA is biased.  Above 0, with PA
    // enabled and MOX keyed, the operator gets actual RF — this is the
    // slice that takes the first-RF bench from bias-only to real
    // emissions.  Use a dummy load + watt-meter for the first session.
    const int clamped = std::clamp(level, 0, 255);
    const int prev    = txDriveLevel_.exchange(clamped, std::memory_order_relaxed);
    if (prev == clamped) return;
    QSettings().setValue(QStringLiteral("tx/driveLevel"), clamped);
    emit txDriveLevelChanged(clamped);
    // Operator-facing percent for the log line (gateware actually
    // resolves to 16 coarse steps, but the percent is the operator's
    // mental model — quoting raw 0..255 in the log would be noise).
    const int pct = static_cast<int>(std::lround(clamped * 100.0 / 255.0));
    emit logLine(QStringLiteral("TX: drive -> %1 %  (raw %2/255)")
                 .arg(pct).arg(clamped));
}

void HL2Stream::setTxStepAttnDb(int db) {
    txStepAttnDb_.store(std::clamp(db, 0, 31), std::memory_order_relaxed);
}

void HL2Stream::setPaEnabled(bool on) {
    // Lands C2 bit 3 (0x08) of slot 10 (frame 0x12) — gateware
    // PA-enable, active-high.  Operator-gated via the Settings checkbox
    // (default OFF on every fresh stream open / close cycle for safety,
    // but persisted across clean Lyra exit-and-relaunch).
    //
    // When this is TRUE *and* MOX is on the wire *and* the gateware
    // sees the round-robin C2-bit-3 update, the PA bias engages — the
    // operator should see PA-current rise on the banner.  TX I/Q stays
    // zero until a future TX DSP commit, so PA bias rises but no
    // modulated carrier reaches the antenna (~0 W into dummy load).
    const bool prev = paOn_.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    QSettings().setValue(QStringLiteral("tx/paEnabled"), on);
    emit paEnabledChanged(on);
    safetyLog(QStringLiteral("TX: PA enable -> %1")
              .arg(on ? QStringLiteral("ON  (RF possible on next key)")
                      : QStringLiteral("off (PA bias disarmed)")));
}

void HL2Stream::setTuneEnabled(bool on) {
    // Arms / disarms the host-streamed tune-carrier generator.  When
    // ON and the wire-MOX bit is high, the EP2 writer fills each LRIQ
    // tuple's TX-I bytes with a constant ~0.95-full-scale value and
    // TX-Q with zero — DC injection that produces a pure carrier at
    // LO exactly (zero-beat, at the dial freq, universal HF-rig TUN
    // convention).  This is the carrier the HL2+ ak4951v4 gateware
    // requires for any RF (per the gateware RTL, there is no internal
    // tune carrier; the host must stream non-zero TX I/Q for the DAC
    // to emit anything regardless of drive level).
    //
    // Drive % scales the on-air power; this just provides the I/Q
    // amplitude for it to scale.  NOT persisted (TUN is an explicit
    // operator gesture, not a configured state) and auto-cleared on
    // every wire-MOX-off edge by the ctor's self-wired safety.
    const bool prev = tuneEnabled_.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    emit tuneEnabledChanged(on);
    safetyLog(QStringLiteral("TX: tune-carrier -> %1")
              .arg(on ? QStringLiteral("ARMED  (zero-beat @ 0.95 fs — emits while MOX active)")
                      : QStringLiteral("disarmed (TX I/Q back to silent)")));
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
// the keyup space window collapses to stay TX (standard HF-rig
// PTT-FSM pattern).

void HL2Stream::requestMox(bool on) {
    requestMox(on, PttSource::Manual);
}

void HL2Stream::requestMoxFromHwPtt(bool on) {
    requestMox(on, PttSource::HwPtt);
}

void HL2Stream::requestMoxFromTci(bool on) {
    requestMox(on, PttSource::Tci);
}

void HL2Stream::requestMox(bool on, PttSource source) {
    requestedMox_ = on;
    // Task #33 — Thetis-faithful PTT-source tracking (working
    // reference at TCIServer.cs:3537 routes TCI MOX through a
    // separate TCIPTT property; Lyra records the source as metadata
    // on the SAME FSM funnel, wire-identical, so subscribers like
    // TciServer can branch on it in their moxActiveChanged handler
    // — e.g. clearQueuedTxAudio only when PttSource::Tci releases).
    //
    // Record the source on the rising edge AND on a re-key (source
    // change mid-TX = operator handed off, e.g. operator MOX during
    // a TCI session); cleared back to Manual when the FSM fully
    // settles to RX (fsmKeyupSettled → resetPttSource()).  This
    // matches the working reference's TCIPTT lifecycle.
    if (on) {
        pttSource_.store(source, std::memory_order_release);
    }
    // Fire the press-intent pulse on EVERY keydown intent — even when
    // the FSM is already running (re-key during ptt_out_delay), and
    // even when the press is shorter than the TR delay (FSM cancels
    // mid-mox_delay).  The UI uses this to show the operator that
    // their press was acknowledged, separate from whether the wire
    // MOX bit ever settles.  Keyup releases (on=false) do NOT pulse —
    // the operator already saw the red light if they were keyed.
    if (on) {
        emit moxIntentPulse();
    }
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
        // Switch the OC pattern to the TX-side per-band bits FIRST so
        // the external filter board's relays have the mox_delay window
        // (~15 ms) to settle into TX configuration before the MOX bit
        // hits the wire and any RF starts.  On N2ADR this is a no-op at
        // the LPF level (RX/TX use the same per-band LPF) but it does
        // drop the RX-only 3 MHz HPF; on other filter boards where TX
        // LPF and RX BPF are on different relays, this gives them time
        // to settle before RF appears.  Hot-switch safety.
        updateOcPattern(/*transmitting=*/true);
        emit logLine(QStringLiteral(
            "TX: keydown — ATT-on-TX %1 dB, mox_delay %2 ms")
            .arg(kAttOnTxDb).arg(moxDelayMs_));
        QTimer::singleShot(moxDelayMs_, this,
                           [this]() { fsmKeydownPostMox(); });
    } else if (!requestedMox_ && wireOn) {
        // Keyup: schedule the space_mox re-key window.  Don't touch
        // mox_ yet — spaceMoxDelayMs_ gives the operator a chance to
        // re-key (CW dot tail, mic re-key) before the wire flip.
        fsmRunning_ = true;
        emit logLine(QStringLiteral(
            "TX: keyup — space_mox_delay %1 ms (re-key window)")
            .arg(spaceMoxDelayMs_));
        QTimer::singleShot(spaceMoxDelayMs_, this,
                           [this]() { fsmKeyupPostSpace(); });
    } else {
        // Nothing to do: intent already matches wire state.
        fsmRunning_ = false;
    }
}

void HL2Stream::fsmKeydownPostMox() {
    if (!requestedMox_) {
        // Operator cancelled mid-keydown (before the wire MOX bit
        // even went on) — restore the saved step-att + OC RX pattern
        // and exit clean.  Symmetric with the fsmAdvance keydown raise.
        setTxStepAttnDb(savedTxStepAttn_);
        updateOcPattern(/*transmitting=*/false);
        safetyLog(QStringLiteral(
            "TX: keydown cancelled mid-mox_delay; ATT-on-TX restored"));
        fsmRunning_ = false;
        return;
    }
    // Flip the wire MOX bit — visible on the next EP2 datagram
    // (~2.6 ms worst case at 380 Hz cadence).  C0 bit 0 on both USB
    // frames of the next datagram (composeCC snapshots mox_ once
    // per send) — coherent.
    mox_.store(true, std::memory_order_release);

    // TX-1 component 7 — bring up the TX-DSP + envelope + injection
    // gate, all at this same moment (wire MOX bit just went hot).
    //
    // Reference parity: the verified reference's keydown chain runs
    // `rf_delay` THEN `SetChannelState(id(1,0), 1, 0)` THEN audio-on
    // — TX DSP starts AFTER rf_delay.  Lyra moves the TX-on hooks
    // HERE (before rf_delay) so the WDSP TXA accumulator has the
    // ~50ms rfDelayMs window to pre-fill the 126-sample EP2 hand-off
    // buffer (producer needs 2-3 fexchange0 outputs to fill one
    // wire datagram); otherwise the first SSB datagram after fade
    // ramps in arrives several ms late and creates a hard step from
    // silence to a non-zero sample = audible click.  The fade-in
    // ramps the AMPLITUDE smoothly 0→1 over the rfDelayMs window,
    // so on-air rise is still cos²-shaped + hot-switch-protected.
    fade_.notifyMoxState(true);
    {
        std::function<void()> startFn;
        {
            std::lock_guard<std::mutex> lk(txControlMtx_);
            startFn = txControl_.start;
        }
        if (startFn) startFn();
    }
    setInjectTxIq(true);

    QTimer::singleShot(rfDelayMs_, this,
                       [this]() { fsmKeydownSettled(); });
}

void HL2Stream::fsmKeydownSettled() {
    if (!requestedMox_) {
        // Cancelled mid-rf_delay — wire MOX already went on, must
        // unwind through a real keyup (space → clear → ptt_out →
        // restore att).  Schedule the keyup chain directly.
        safetyLog(QStringLiteral(
            "TX: keydown cancelled mid-rf_delay; initiating keyup"));
        QTimer::singleShot(spaceMoxDelayMs_, this,
                           [this]() { fsmKeyupPostSpace(); });
        return;
    }
    moxActive_ = true;
    emit moxActiveChanged(true);
    safetyLog(QStringLiteral("TX: MOX_TX (wire MOX bit settled)"));
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

    // TX-1 component 7 — §5.7 keyup ordering invariant:
    //   (1) MoxEdgeFade fade-out reaches zero
    //   (2) Clear inject_tx_iq (SSB EP2 branch goes silent)
    //   (3) Clear wire MOX bit
    //
    // Step (1) is started here; we then schedule fsmKeyupFadeOut
    // after fade_.fadeOutMs() to give the cos² ramp time to reach
    // zero BEFORE we stop the TX channel (so the gateware DAC sees
    // a smoothly-faded tail, not a hard chop, on the way out).
    //
    // Step (2) is done here-and-now: setInjectTxIq(false) stops the
    // producer from accumulating new SSB samples.  The fade ramps
    // existing/in-flight buffer content down to zero independently.
    fade_.notifyMoxState(false);
    setInjectTxIq(false);

    QTimer::singleShot(fade_.fadeOutMs(), this,
                       [this]() { fsmKeyupFadeOut(); });
}

void HL2Stream::fsmKeyupFadeOut() {
    // TX-1 component 7 — fade-out has reached zero; now we can stop
    // the WDSP TXA channel (blocking flush) without any audible chop
    // on the gateware side.  Matches the verified reference's
    //   WDSP.SetChannelState(WDSP.id(1,0), 0, 1)
    // at the equivalent point in the keyup chain.  BLOCKING (≤100 ms
    // for WDSP internal drain) — same Qt-main-thread block the
    // reference's own UI thread takes via Thread.Sleep.
    {
        std::function<void()> stopFn;
        {
            std::lock_guard<std::mutex> lk(txControlMtx_);
            stopFn = txControl_.stop;
        }
        if (stopFn) stopFn();
    }
    // Schedule the in-flight clear wait BEFORE the wire MOX clears.
    // Reference-faithful match to:
    //   Thread.Sleep(mox_delay);  // default 10, allows in-flight
    //                                samples to clear
    // Lets UDP datagrams already-sent or in-OS-buffer (with MOX=1
    // + non-zero samples) actually reach + be processed by the HL2
    // BEFORE we flip the wire state — so the gateware processes
    // them as the keyed samples they were when sent, not as bogus
    // "MOX=0 with TX I/Q" samples.
    QTimer::singleShot(txStopDelayMs_, this,
                       [this]() { fsmKeyupInFlight(); });
}

void HL2Stream::fsmKeyupInFlight() {
    // TX-1 component 7 — in-flight UDP datagrams have cleared; now
    // safe to flip the wire MOX bit.  Next composeCC pass on each
    // EP2 datagram will read this as 0 → C0 bit-0 = 0 → gateware
    // T/R transition begins.
    mox_.store(false, std::memory_order_release);
    QTimer::singleShot(pttOutDelayMs_, this,
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
    // Switch the OC pattern back to the RX-side per-band bits.  This
    // happens AFTER the wire MOX bit has cleared (in fsmKeyupPostSpace)
    // AND ptt_out_delay has elapsed — so the filter board only returns
    // to RX configuration when no RF is being emitted.  Mirror of the
    // fsmAdvance keydown raise; symmetric hot-switch safety.
    updateOcPattern(/*transmitting=*/false);
    moxActive_ = false;
    emit moxActiveChanged(false);
    safetyLog(QStringLiteral(
        "TX: RX (wire MOX cleared, ATT-on-TX restored to %1 dB)")
        .arg(savedTxStepAttn_));
    // Task #33 — release PTT-source tracking back to Manual now that
    // the FSM has fully settled to RX.  Order matters: AFTER emitting
    // moxActiveChanged(false) so subscribers reading pttSource() in
    // their slot see the source that just released, not the default
    // post-release state.  Equivalent to the working reference's
    // m_tciPttActive clearing in SyncTciPttToMox / TRX:0,false.
    pttSource_.store(PttSource::Manual, std::memory_order_release);
    fsmRunning_ = false;
}

// ---------------------------------------------------------------
// TX-0c-pa-debug: host-side TX safety timeout.
//
// QTimer-based auto-MOX-off so a stuck transmitter (held key, asleep
// at the desk, runaway PTT) doesn't hold the air for hours.  Arms on
// the wire MOX-on edge (moxActiveChanged(true)), cancels on the
// MOX-off edge.  On fire we route through requestMox(false) — same
// path as a normal key-up, so the FSM runs the keyup TR-delay chain
// cleanly + ATT-on-TX is restored.  Bypass switches the safety off
// entirely (long-form AM ragchews, CW beacons, bench debugging).

void HL2Stream::setTxTimeoutSec(int sec) {
    sec = std::clamp(sec, kTxTimeoutMinSec, kTxTimeoutMaxSec);
    if (sec == txTimeoutSec_) return;
    txTimeoutSec_ = sec;
    QSettings().setValue(QStringLiteral("tx/timeoutSeconds"), sec);
    emit txTimeoutSecChanged(sec);
    safetyLog(QStringLiteral("TX safety timeout = %1 s (%2 min)")
              .arg(sec).arg(sec / 60.0, 0, 'f', 1));
    // If currently keyed, re-arm with the new full duration.  Operator
    // intent ("I want N more seconds from now") — symmetric with the
    // bypass-toggle behaviour below.
    if (moxActive_ && !txTimeoutBypass_) {
        armTxSafetyTimer();
    }
}

void HL2Stream::setTxTimeoutBypass(bool on) {
    if (on == txTimeoutBypass_) return;
    txTimeoutBypass_ = on;
    QSettings().setValue(QStringLiteral("tx/timeoutBypass"), on);
    emit txTimeoutBypassChanged(on);
    safetyLog(QStringLiteral("TX safety timeout bypass -> %1")
              .arg(on ? QStringLiteral("ON (no auto-release)")
                      : QStringLiteral("off (auto-release armed)")));
    if (moxActive_) {
        if (on)  cancelTxSafetyTimer();    // safety just turned OFF
        else     armTxSafetyTimer();       // safety just turned ON;
                                           // fresh full duration
    }
}

// ---- Task #36: Hardware PTT input forwarder opt-in ----------------
//
// Per project-memory §10 Q#1 + §15.25 RESOLVED-CORRECTION, the EP6
// C0 bit 0 (`ptt_in`) is NOT guaranteed to be a clean 0 at RX rest
// across HL2 gateware revs — N8SDR's HL2+/AK4951 unit empirically
// shows a non-zero level, which means an always-on forwarder would
// mis-read it as a foot-switch press → spurious requestMox(true) →
// phantom-TX surge.  The forwarder in rxWorkerLoop is therefore
// HARD-GATED on this atomic; default OFF; operator opts in via
// Settings → TX → Advanced AFTER bench-verifying ptt_in on their
// own unit (or after wiring a real foot-switch / mic-button).
//
// Persistence lives in Prefs (tx/hw_ptt_enabled); main.cpp mirrors
// Prefs.hwPttEnabled into this setter so the in-flight wire path
// always sees the operator's current intent without a stream
// restart.
//
// Turning OFF clears lastPttIn_ so the next ON-edge after a
// re-enable doesn't fire on whatever stale level the wire happened
// to be reading.  This is wire-thread state mutated from the Qt
// main thread — safe because the RX worker only READS lastPttIn_
// when hwPttEnabled_ is true (and we clear it under hwPttEnabled_=
// false, so the worker can't be reading it at the same moment).
void HL2Stream::setHwPttEnabled(bool on) {
    const bool prev = hwPttEnabled_.exchange(on, std::memory_order_release);
    if (prev == on) return;
    if (!on) {
        // Reset the edge-detect memory so a future re-enable
        // doesn't trip on a wire level that drifted while gated off.
        lastPttIn_ = false;
    }
    safetyLog(QStringLiteral("HW PTT input forwarder -> %1")
              .arg(on ? QStringLiteral("ENABLED (foot-switch will key)")
                      : QStringLiteral("disabled (default-safe)")));
    emit hwPttEnabledChanged(on);
}

// ---- TX-1 component 5a: TR-sequencing + cos² envelope setters ----
//
// Each setter clamps, persists to QSettings, and emits the matching
// change signal so Settings UI bindings refresh.  All run on the Qt
// main thread (Settings UI / FSM thread — same QObject).  The FSM's
// next QTimer::singleShot scheduling reads the live member, so any
// change made while the radio is keyed takes effect on the NEXT MOX
// edge (no mid-transition perturbation).
//
// ⚠ HOT-SWITCH SAFETY: rfDelayMs_ in particular is load-bearing
// protection for external solid-state HF linears.  The setter
// honours the operator's value (clamped to [kMinFsmDelayMs,
// kMaxFsmDelayMs]) — no internal floor beyond that — because the
// operator's specific amp's switching spec is the authority, not a
// generic safe default.  Settings UI tooltips carry the hot-switch
// warning so a low value is an informed operator choice.

void HL2Stream::setMoxDelayMs(int ms) {
    const int clamped = std::clamp(ms, kMinFsmDelayMs, kMaxFsmDelayMs);
    if (clamped == moxDelayMs_) return;
    moxDelayMs_ = clamped;
    QSettings().setValue(QStringLiteral("tx/trSeq/moxDelayMs"), clamped);
    emit moxDelayMsChanged(clamped);
}

void HL2Stream::setRfDelayMs(int ms) {
    const int clamped = std::clamp(ms, kMinFsmDelayMs, kMaxFsmDelayMs);
    if (clamped == rfDelayMs_) return;
    rfDelayMs_ = clamped;
    QSettings().setValue(QStringLiteral("tx/trSeq/rfDelayMs"), clamped);
    emit rfDelayMsChanged(clamped);
}

void HL2Stream::setSpaceMoxDelayMs(int ms) {
    const int clamped = std::clamp(ms, kMinFsmDelayMs, kMaxFsmDelayMs);
    if (clamped == spaceMoxDelayMs_) return;
    spaceMoxDelayMs_ = clamped;
    QSettings().setValue(QStringLiteral("tx/trSeq/spaceMoxDelayMs"), clamped);
    emit spaceMoxDelayMsChanged(clamped);
}

void HL2Stream::setPttOutDelayMs(int ms) {
    const int clamped = std::clamp(ms, kMinFsmDelayMs, kMaxFsmDelayMs);
    if (clamped == pttOutDelayMs_) return;
    pttOutDelayMs_ = clamped;
    QSettings().setValue(QStringLiteral("tx/trSeq/pttOutDelayMs"), clamped);
    emit pttOutDelayMsChanged(clamped);
}

void HL2Stream::setFadeInMs(int ms) {
    // MoxEdgeFade::setFadeInMs clamps internally to [kMinFadeMs,
    // kMaxFadeMs]; we read back the post-clamp live value to emit +
    // persist the actual stored quantity (avoids drift if the
    // operator typed an out-of-range value).
    const int before = fade_.fadeInMs();
    fade_.setFadeInMs(ms);
    const int after  = fade_.fadeInMs();
    if (after == before) return;
    QSettings().setValue(QStringLiteral("tx/trSeq/fadeInMs"), after);
    emit fadeInMsChanged(after);
}

void HL2Stream::setFadeOutMs(int ms) {
    const int before = fade_.fadeOutMs();
    fade_.setFadeOutMs(ms);
    const int after  = fade_.fadeOutMs();
    if (after == before) return;
    QSettings().setValue(QStringLiteral("tx/trSeq/fadeOutMs"), after);
    emit fadeOutMsChanged(after);
}

void HL2Stream::setTxStopDelayMs(int ms) {
    const int clamped = std::clamp(ms, kMinFsmDelayMs, kMaxFsmDelayMs);
    if (clamped == txStopDelayMs_) return;
    txStopDelayMs_ = clamped;
    QSettings().setValue(QStringLiteral("tx/trSeq/txStopDelayMs"), clamped);
    emit txStopDelayMsChanged(clamped);
}

// TX-1 component 8a — operator-tunable WDSP TXA gain stages.  Each
// setter: (1) clamps to its declared range, (2) compares vs the
// current value (no-op on no-change to avoid signal storms when QML
// bindings re-fire on a Restore-Defaults click), (3) persists to
// QSettings under tx/<key>, (4) emits the changed signal for UI
// readback, (5) forwards to the registered TxControl callback OUTSIDE
// the registration lock so TxChannel's channelMtx_ does the WDSP
// setter serialisation (matches the registerTxControl push pattern).
//
// Live-apply contract: when the operator drags the slider, the WDSP
// setter call lands on the next ~2.6 ms fexchange0 block (TxChannel's
// channelMtx_ guards the setter ↔ process() race; the operator-setter
// call simply waits at most one block period if process() is
// in-flight, which is fine for an infrequent operator click rate).
void HL2Stream::setMicGainDb(double db) {
    const double clamped = std::clamp(db, kMinMicGainDb, kMaxMicGainDb);
    if (clamped == micGainDb_) return;
    micGainDb_ = clamped;
    QSettings().setValue(QStringLiteral("tx/micGainDb"), clamped);
    emit micGainDbChanged(clamped);
    std::function<void(double)> fwd;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setMicGainDb;
    }
    if (fwd) fwd(clamped);
}

void HL2Stream::setAlcMaxGainDb(double db) {
    const double clamped = std::clamp(db, kMinAlcMaxGainDb, kMaxAlcMaxGainDb);
    if (clamped == alcMaxGainDb_) return;
    alcMaxGainDb_ = clamped;
    QSettings().setValue(QStringLiteral("tx/alcMaxGainDb"), clamped);
    emit alcMaxGainDbChanged(clamped);
    std::function<void(double)> fwd;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setAlcMaxGainDb;
    }
    if (fwd) fwd(clamped);
}

// TX-1 component 8a-tx-mode — relay the operator's WDSP TXA mode to the
// TX channel via the registered TxControl.setMode callback.  No
// persistence here (RX mode in WdspEngine is the operator-driven source
// of truth + already QSettings-persisted; TX mode is the slave that
// tracks it).  No no-op guard on (mode == cached) — the TxChannel side
// (post-fix to tx_channel.cpp::setMode) deliberately propagates every
// call to WDSP, so this layer doesn't need to second-guess either.  The
// wdspMode argument is the raw WDSP TXA mode integer (0=LSB, 1=USB);
// clamped to [0, 1] here as a defence against translator bugs.
void HL2Stream::setTxMode(int wdspMode) {
    const int clamped = std::clamp(wdspMode, 0, 1);
    std::function<void(int)> fwd;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setMode;
    }
    // TX-1 component 8a-tx-mode — diagnostic log so we can verify the
    // mode-push chain is firing end-to-end on bench.  Operator's
    // 2026-05-31 follow-up bench showed "no change" after the initial
    // 8a-tx-mode commit; one possibility is the path was never reached
    // (stale build, callback unwired).  This log answers "did
    // setTxMode get called and was the callback registered" in the
    // bench [tx] log lines so we don't have to guess next time.
    qInfo("[tx] setTxMode(wdsp=%d) -> %s",
          clamped,
          fwd ? "forwarded to TxControl.setMode"
              : "NO-OP (TxControl.setMode not registered)");
    if (fwd) fwd(clamped);
}

// TX-1 component 8c + Task #53 — operator TX bandpass.  Forwards
// (low, high) Hz to the registered TxControl.setBandpass callback.
// TxChannel internally sign-codes per the current WDSP mode (USB
// pass-through, LSB negate-and-swap), so we always pass positive
// edges.  Both edges are pulled from Prefs at the call site:
//   low  = Prefs.filterLow   (shared with the RX bandpass; Task #53)
//   high = Prefs.txBandwidth (per-mode TX BW combo; component 8c)
// No-op if high<=0 or low<0 or low>=high (defence — Prefs clamps
// both sides into sane ranges but the safety check is cheap).
void HL2Stream::setTxBandpass(int lowHz, int highHz) {
    if (highHz <= 0 || lowHz < 0 || lowHz >= highHz) return;
    const double low  = static_cast<double>(lowHz);
    const double high = static_cast<double>(highHz);
    std::function<void(double, double)> fwd;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setBandpass;
    }
    qInfo("[tx] setTxBandpass(low=%d, high=%d) -> %s",
          lowHz, highHz,
          fwd ? "forwarded to TxControl.setBandpass"
              : "NO-OP (TxControl.setBandpass not registered)");
    if (fwd) fwd(low, high);
}

// ─────────────────────────────────────────────────────────────
// TX-1 component 6: SSB I/Q injection gate + source registration
// ─────────────────────────────────────────────────────────────
//
// setInjectTxIq is the FSM hand-off point.  Wire-inert default
// (false) means the EP2 packer's SSB branch is never taken, so
// no SSB I/Q can land on the wire even with MOX keyed.  The FSM
// (follow-up commit) flips this TRUE at keydown AFTER the WDSP
// TXA channel is started + rf_delay has elapsed, and FALSE at
// keyup BEFORE the wire MOX bit clears (per §5.7 keyup ordering
// invariant).
//
// NOT persisted to QSettings — operator intent is captured in
// the FSM gestures (PTT, MOX, TUN buttons), not in a "default
// SSB armed" preference.  Stream stop / restart always comes
// up with injectTxIq_=false (matches the cb58bcb-class
// come-up-not-keyed safety posture for MOX + tune).

void HL2Stream::setInjectTxIq(bool on) {
    const bool prev = injectTxIq_.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    safetyLog(QStringLiteral(
        "TX: inject_tx_iq -> %1 (EP2 packer %2 SSB samples on MOX)")
        .arg(on ? QStringLiteral("ARMED") : QStringLiteral("disarmed"))
        .arg(on ? QStringLiteral("pulling") : QStringLiteral("ignoring")));
    // TX-1 component 7 — forward to the registered TxControl callback
    // so the TxDspWorker producer-side flag flips in lockstep with the
    // EP2-packer consumer-side flag.  Single point of control from
    // the FSM.  Brief lock; uncontested (registration at app boot only).
    {
        std::function<void(bool)> fwd;
        {
            std::lock_guard<std::mutex> lk(txControlMtx_);
            fwd = txControl_.setInjectTxIq;
        }
        if (fwd) fwd(on);
    }
    emit injectTxIqChanged(on);
}

// TX-1 component 7 — register the TX channel lifecycle callbacks.
// Empty TxControl = clear all five (start/stop/setInjectTxIq +
// component-8a setMicGainDb/setAlcMaxGainDb).  See TxControl struct
// doc in the header for the contract.  Same mutex pattern as
// registerTxIqSource.
//
// TX-1 component 8a — registration ALSO pushes the autoloaded
// micGainDb_ + alcMaxGainDb_ to the channel ONCE so the freshly-
// opened WDSP TXA channel doesn't sit at WDSP create-time defaults
// (especially the load-bearing ALC max-gain = 0 dB trap).  This is
// the architectural equivalent of the verified reference's Setup
// first-load pushing the operator's persisted profile values onto
// the channel.  Capture the callbacks INSIDE the lock, release the
// lock, then call OUTSIDE the lock — TxChannel's own channelMtx_
// will serialise the WDSP setter call against any in-flight
// process() on the worker thread.
void HL2Stream::registerTxControl(TxControl ctl) {
    std::function<void(double)> pushMic;
    std::function<void(double)> pushAlc;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        txControl_ = std::move(ctl);
        // Only push if BOTH callbacks landed — a clear ({}) call
        // gives us null function objects, in which case we just
        // dropped the registration and there's nothing to push.
        pushMic = txControl_.setMicGainDb;
        pushAlc = txControl_.setAlcMaxGainDb;
    }
    if (pushMic) pushMic(micGainDb_);
    if (pushAlc) pushAlc(alcMaxGainDb_);
}

// Source registration — caller-owned lifetime.  The wire writer
// thread calls the registered std::function once per datagram when
// (moxBit && injectTxIq_); the callback fills 126 complex<float>
// samples and returns true, or returns false if no data was ready.
//
// Passing an empty std::function ({}) clears the source.  Caller
// MUST clear before the underlying object goes away.  The mutex
// here is for the register/clear race only — the hot-path read
// inside the EP2 packer takes the same mutex briefly per datagram,
// which is fine at ~380 Hz (uncontested unless mid-registration).
void HL2Stream::registerTxIqSource(TxIqSource src) {
    std::lock_guard<std::mutex> lk(txIqSourceMtx_);
    txIqSource_ = std::move(src);
}

void HL2Stream::armTxSafetyTimer() {
    if (txTimeoutBypass_ || !txSafetyTimer_) return;
    // QTimer::start(int) with a fresh duration replaces any pending
    // expiry — operator intent on bypass-off-then-on or duration change
    // is "give me a fresh full window from this moment."
    txSafetyTimer_->start(txTimeoutSec_ * 1000);
    safetyLog(QStringLiteral("TX safety: armed for %1 s").arg(txTimeoutSec_));
}

void HL2Stream::cancelTxSafetyTimer() {
    if (!txSafetyTimer_) return;
    if (txSafetyTimer_->isActive()) {
        txSafetyTimer_->stop();
        safetyLog(QStringLiteral("TX safety: cancelled (keyup or bypass)"));
    }
}

void HL2Stream::onTxSafetyTimeout() {
    // Drive the auto-release through the normal funnel so the TR-delay
    // keyup chain runs cleanly + ATT-on-TX restores + UI red-on-air
    // clears via the standard moxActiveChanged(false) edge.
    safetyLog(QStringLiteral(
        "TX safety: timeout reached after %1 s — auto-releasing")
        .arg(txTimeoutSec_));
    emit txTimeoutFired();
    requestMox(false);
}

void HL2Stream::setSampleRate(int hz) {
    std::uint8_t bits;
    int micDec;
    switch (hz) {
    case 96000:  bits = 0x01; micDec = 2; break;
    case 192000: bits = 0x02; micDec = 4; break;
    case 384000: bits = 0x03; micDec = 8; break;
    default:     return;   // 48k excluded (EP2 cadence)
    }
    sampleRateBits_.store(bits, std::memory_order_relaxed);
    // TX Component 3 — keep the mic decimation factor coherent with
    // the new wire rate so the 48 kHz mic output stays at 48 kHz across
    // rate switches (matches the C reference's `SetDDCRate`
    // factor table at netInterface.c:1328-1354).  The
    // `mic_decimation_count_` does NOT need to reset across normal rate
    // switches — same-direction phase walk-in stabilises within one
    // datagram; the RX-worker-thread-only counter is safe to leave alone.
    micDecimationFactor_.store(micDec, std::memory_order_relaxed);
    emit logLine(QStringLiteral("IQ sample rate -> %1 kHz (wire C1), "
                                "mic dec /%2")
                 .arg(hz / 1000).arg(micDec));
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
// for the verified slot map + cites (HL2 wire-protocol C reference
// + ak4951v4 gateware control.v).
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
        // per the HL2 wire-protocol reference; slots 5/6 below mirror
        // it on DDC2/DDC3.
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
        // 0x1C TX step-att — C3 = (31 - db) & 0x1F (HL2 inverted
        // range; see hl2_stream.h header doc for wire-protocol cite).
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

void HL2Stream::updateOcPattern(bool transmitting) {
    // OC pattern for the current band when the board is enabled, else 0.
    // The FSM passes transmitting=true on keydown (in fsmAdvance, after
    // raising ATT-on-TX) so the TX-side OC bits hit the wire BEFORE the
    // MOX bit + rf_delay window.  This gives the filter board's relays
    // time to settle into TX configuration (per-band LPF in place) before
    // any RF energy starts being emitted — operator's hot-switch safety.
    // On keyup the FSM passes transmitting=false in fsmKeyupSettled AFTER
    // the wire MOX bit has cleared and ptt_out_delay elapsed, so the
    // board switches back to RX configuration only after RF is gone.
    int pattern = 0;
    if (filterBoardEnabled_) {
        const int bi = lyra::bandIndexForFreq(
            static_cast<int>(rx1FreqHz_.load(std::memory_order_relaxed)));
        pattern = lyra::n2adrOcPattern(bi, transmitting) & 0x7F;
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

    // EP2 writer pacing — the verified HL2 wire-protocol reference's
    // sendProtocol1Samples model: sends are driven by AUDIO
    // PRODUCTION, not a free-running PC timer.  RX audio
    // is decoded from the HL2's own IQ (its crystal), so sending one
    // datagram per 126-frame chunk AS IT'S PRODUCED locks the EP2 cadence
    // to the HL2 clock — single crystal, no two-clock drift, no codec
    // FIFO under/overrun (the crackle/pop the timer approach caused).
    // When no audio flows (PC-output mode, or a real underrun) a silence
    // keepalive maintains the gateware's EP2 watchdog.
    //
    // HIGHEST priority + a 1 ms scheduler tick keep send jitter low
    // (the verified HL2 wire-protocol reference runs this thread at
    // MMCSS "Pro Audio").
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
        // USB frame (two slots per datagram).  Matches the verified
        // HL2 wire-protocol cadence (1 slot per USB frame, ~760/s on EP2);
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

        // Audio L/R + TX-I/Q into the 126 LRIQ tuples (BE 16-bit).
        // TX-I/Q is normally zero (no modulator yet, no operator-armed
        // tone) but TX-0c-tune fills it with a complex 1 kHz sinusoid
        // when the operator has armed TUN and the wire-MOX bit is
        // high.  The HL2+ ak4951v4 gateware DAC consumes the TX-I/Q
        // samples only while MOX is set (dsopenhpsdr1.v:351-371), so
        // we gate on the SAME moxBit snapshot the C&C slots used above
        // (one coherent decision per datagram).  Phase advances per
        // sample at 48 kHz; sampled once per LRIQ index, written to
        // BOTH USB frames so the on-air carrier is continuous.
        const bool emitTone =
            moxBit && tuneEnabled_.load(std::memory_order_relaxed);
        // TX-1 component 7 — cos² fade is FSM-driven now, not EP2-
        // packer driven.  The FSM calls fade_.notifyMoxState(true)
        // at fsmKeydownPostMox (T+15 ms, same moment wire MOX bit
        // goes hot — but BEFORE rf_delay, so fade-in ramps the
        // amplitude 0→1 over rfDelayMs to pre-fill the producer's
        // EP2 hand-off accumulator + provide hot-switch protection
        // for the external amp).  It calls notifyMoxState(false) at
        // fsmKeyupPostSpace (T+spaceMoxDelayMs after keyup) so the
        // fade-out completes BEFORE the wire MOX bit clears (§5.7
        // keyup ordering invariant) — gateware sees a smoothly-
        // faded tail, not a hard chop.  EP2 packer just calls
        // fade_.advance() per LRIQ sample below.
        // Tune carrier = zero-beat at LO (DC injection): a constant
        // value in the I channel produces a pure carrier exactly at
        // TX freq on the air, no audio offset.  Matches the universal
        // HF-rig TUN convention — every commercial HF rig,
        // SSB/CW/DIGU all expect "tune carrier at the dial".  Because
        // there's no audio component there's no sideband to choose,
        // so mode (USB / LSB / CWL / etc.) doesn't matter — the carrier
        // lands at the dial freq in every mode.  Constant DC I/Q has
        // no harmonics either, so it's the cleanest possible test
        // tone for linearity and PA-current bench.  Q=0 keeps the
        // signal real (no quadrature component).  Future: a separate
        // 2-tone test signal for IMD/linearity work (a different
        // feature than TUN).
        //
        // Component 5a wires the cos² envelope here: the TUN constant
        // (and the component-6 SSB I/Q below) is multiplied by the
        // fade coefficient per LRIQ sample.  At operator keydown the
        // envelope ramps 0 → 1 over fadeInMs; at keyup it ramps
        // 1 → 0 over fadeOutMs.  When neither TUN nor SSB is active,
        // txI/txQ stay zero — the fade coefficient still advances
        // internally so a mid-flight arm sees the correct state.
        constexpr float kTuneCarrierFullScale = 0.95f * 32767.0f;

        // ── TX-1 component 6: SSB I/Q source pull ────────────────
        //
        // Reference-faithful match to the HL2 wire-protocol C
        // reference's EP2 TX I/Q consumer (see docs/architecture/
        // tx1_ssb_design.md §5.7 for the verified cite + handshake
        // semantics).  Pattern:
        //
        //   if (!XmitBit) memset(outIQbufp, 0, sizeof(complex)*126)
        //
        // = MANDATORY zero on no-MOX.  We extend the rule slightly:
        // zero ALSO when injectTxIq is false, OR when the source
        // callback isn't registered, OR when it returned false (no
        // data ready).  All four cases fall through to the same
        // zero-fill path — the reference's universal posture.
        //
        // TUN takes priority over SSB (operator gesture semantics).
        // If both happen to be armed, TUN wins.  This is rare in
        // practice (operator either arms TUN or relies on SSB mic,
        // not both at once).
        //
        // The source callback (typically TxDspWorker::tryConsumeTxIq)
        // returns true if it filled the 126-element buffer, false if
        // it had no data ready.  Non-blocking — the EP2 writer
        // cannot afford to wait (it's on a hard 2.6 ms timer cadence).
        //
        // Buffer is stack-allocated per-datagram (~1 KB).  No
        // heap traffic on the hot path.
        std::complex<float> ssbBuf[126];
        bool emitSsb = false;
        if (moxBit && injectTxIq_.load(std::memory_order_acquire)
                   && !emitTone) {
            // Brief lock — uncontested at the ~380 Hz wire cadence
            // unless mid-registration (which only happens at app
            // boot / shutdown, not on the hot path).
            TxIqSource src;
            {
                std::lock_guard<std::mutex> lk(txIqSourceMtx_);
                src = txIqSource_;
            }
            if (src && src(ssbBuf)) {
                emitSsb = true;
            } else {
                // SSB expected on this datagram but data not ready
                // (source unregistered, or producer not signalling
                // yet).  Count the underrun; the zero-fill branch
                // below runs as the universal fall-through.
                txIqUnderruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        for (int i = 0; i < 126; ++i) {
            const qint16 L = audio ? audio[i * 2 + 0] : 0;
            const qint16 R = audio ? audio[i * 2 + 1] : 0;
            const float coef = fade_.advance();
            qint16 txI;
            qint16 txQ;
            if (emitTone) {
                // TUN priority path — DC carrier × cos² envelope.
                txI = static_cast<qint16>(
                    kTuneCarrierFullScale * coef + 0.5f);
                txQ = qint16{0};
            } else if (emitSsb) {
                // SSB I/Q × cos² envelope, quantized via the
                // reference-faithful symmetric round-to-nearest
                // (see docs/architecture/tx1_ssb_design.md §5.7
                // for the verified reference cite):
                //   x >= 0 ? floor(x * 32767 + 0.5) : ceil(x * 32767 - 0.5)
                // float saturates to qint16 range via clamp to
                // [-32768, 32767] before the int cast (defensive —
                // the WDSP TXA chain's ALC should keep output in
                // [-1, +1) but cap anyway).
                const float scI = ssbBuf[i].real() * coef * 32767.0f;
                const float scQ = ssbBuf[i].imag() * coef * 32767.0f;
                const float rndI = scI >= 0.0f ? std::floor(scI + 0.5f)
                                               : std::ceil( scI - 0.5f);
                const float rndQ = scQ >= 0.0f ? std::floor(scQ + 0.5f)
                                               : std::ceil( scQ - 0.5f);
                const float clI = rndI < -32768.0f ? -32768.0f
                                : rndI >  32767.0f ?  32767.0f : rndI;
                const float clQ = rndQ < -32768.0f ? -32768.0f
                                : rndQ >  32767.0f ?  32767.0f : rndQ;
                txI = static_cast<qint16>(clI);
                txQ = static_cast<qint16>(clQ);
            } else {
                // Mandatory zero-fill — covers !moxBit AND
                // (moxBit && !injectTxIq) AND (moxBit && injectTxIq
                // && no data).  Matches the verified reference's
                // universal `!XmitBit ⇒ zero` posture.
                txI = qint16{0};
                txQ = qint16{0};
            }
            const int base = (i < 63) ? (16 + i * 8) : (528 + (i - 63) * 8);
            pktBytes[base + 0] = static_cast<std::uint8_t>((L >> 8) & 0xFF);
            pktBytes[base + 1] = static_cast<std::uint8_t>( L       & 0xFF);
            pktBytes[base + 2] = static_cast<std::uint8_t>((R >> 8) & 0xFF);
            pktBytes[base + 3] = static_cast<std::uint8_t>( R       & 0xFF);
            pktBytes[base + 4] = static_cast<std::uint8_t>((txI >> 8) & 0xFF);
            pktBytes[base + 5] = static_cast<std::uint8_t>( txI       & 0xFF);
            pktBytes[base + 6] = static_cast<std::uint8_t>((txQ >> 8) & 0xFF);
            pktBytes[base + 7] = static_cast<std::uint8_t>( txQ       & 0xFF);
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
