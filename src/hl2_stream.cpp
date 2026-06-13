// Lyra — HPSDR Protocol 1 stream implementation (EP6 receive + EP2
// keepalive).  See hl2_stream.h for the locked architecture +
// protocol reference.

#include "hl2_stream.h"

#include "bands.h"
// Step 14 Stage 1 — wire-layer includes for the inert wire-up of
// prn singleton + metis_wire_bind + outbound_init in open().  See
// docs/architecture/STEP14_PLAN.md.  These three calls become live
// callers of the new wire layer for the first time; nothing reads
// the bound state yet (rxWorkerLoop / txWorkerLoop are still the
// live RX/TX path until later stages).
#include "wire/RadioNet.h"      // RadioNet, prn, UpdateRadioProtocolSampleSize, SampleRateIn2Bits
#include "wire/MetisFrame.h"    // metis_wire_bind, metis_socket_fd
#include "wire/ObBuffs.h"       // destroy_obbuffs (P3 — close() ring teardown)
#include "wire/FrameComposer.h" // §5 control-plane mapping: set_rx_freq / set_tx_freq / set_rx_step_attn_db (P4.b RX side)

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
#include <process.h>     // _beginthreadex — EP2 writer thread (P4.b; reference StartAudioNative netInterface.c:66)

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
    // Stage 2b2-fix-v2 — HW-PTT-in poll timer (Qt main thread).
    // 50 ms cadence (20 Hz) — fast enough that foot-switch
    // press-to-key feels instantaneous, slow enough that the
    // per-tick `prn->ptt_in` read + edge-detect costs are noise.
    // Reads the level the EP6 thread writes unconditionally at
    // every status decode (`Ep6RecvThread.cpp:866`), mirroring
    // the reference's "wire writes prn->ptt_in; consumer FSM
    // polls and edge-detects on its own clock" posture.
    hwPttTimer_.setInterval(50);
    connect(&hwPttTimer_, &QTimer::timeout,
            this, &HL2Stream::onHwPttPoll);
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
    // Task #39 — HL2 +20 dB hardware Mic Boost (codec analog PGA).
    // Persisted across launches; NOT defensively cleared on
    // open/close (it's voice-chain calibration, not a safety gate).
    micBoost_.store(
        QSettings().value(QStringLiteral("tx/micBoost"), false).toBool(),
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
    // §3.9-5 revert: MoxEdgeFade deleted (operator-rejected 2026-06-06).
    // Reference does not envelope-shape SSB TX I/Q at all — host-side
    // cos² fade was a Lyra invention.  Hot-switch protection for an
    // external linear is now rfDelayMs_ ALONE (matches reference).
    // TX-1 component 8a — operator-tunable WDSP TXA gain stages.
    // micGainDb defaults to 0 dB = WDSP unity (no change vs the lyra-
    // cpp ship-no-setters posture at TxChannel::open()).
    //
    // alcMaxGainLinear defaults to 3.0 LINEAR (= 3.0× amplitude =
    // +9.54 dB amplification headroom), matching the verified
    // reference's Setup-load spinner default EXACTLY — integer
    // spinner 0..120 incr 1 default 3, passed straight to
    // SetTXAALCMaxGain as LINEAR (no dB conversion).  WDSP create-
    // time is 1.0 LINEAR (= 0 dB) which pins the entire TXA output
    // chain at a 0 dB ALC ceiling regardless of mic level — the
    // load-bearing trap that this default lifts.  Earlier Lyra
    // shipped this property as dB and called `dbToLin(3.0)=1.413`
    // — capping the ceiling at 47% of the reference's value and
    // producing a 5-6 dB power deficit on continuous mic-input
    // tones (task #79 root cause, fixed in §15.27 2026-06-03).
    //
    // The old QSettings key `tx/alcMaxGainDb` (dB semantics) is
    // intentionally abandoned on upgrade — operator silently
    // inherits the new reference-faithful default on first launch.
    //
    // Push-to-channel happens in registerTxControl() once the
    // TxControl callbacks are wired (the channel is open by then;
    // the operator's persisted/default values land on the channel
    // before the first keydown).  Clamped on load to defend against
    // stale / corrupt QSettings (hand-edited file).
    micGainDb_ = std::clamp(
        QSettings().value(QStringLiteral("tx/micGainDb"),
                          kDefaultMicGainDb).toDouble(),
        kMinMicGainDb, kMaxMicGainDb);
    alcMaxGainLinear_ = std::clamp(
        QSettings().value(QStringLiteral("tx/alcMaxGainLinear"),
                          kDefaultAlcMaxGainLinear).toDouble(),
        kMinAlcMaxGainLinear, kMaxAlcMaxGainLinear);
    // §15.27 Commit B — ALC decay (operator-tunable wcpagc release tau).
    alcDecayMs_ = std::clamp(
        QSettings().value(QStringLiteral("tx/alcDecayMs"),
                          kDefaultAlcDecayMs).toInt(),
        kMinAlcDecayMs, kMaxAlcDecayMs);
    // §15.27 Commit B — Leveler trio.  Default OFF (operator preference);
    // when enabled, defaults match the verified reference UI exactly
    // (15.0 LINEAR / 100 ms).  All three keys clamped on load.
    levelerOn_ = QSettings().value(QStringLiteral("tx/levelerOn"),
                                   kDefaultLevelerOn).toBool();
    levelerMaxGainLinear_ = std::clamp(
        QSettings().value(QStringLiteral("tx/levelerMaxGainLinear"),
                          kDefaultLevelerMaxGainLinear).toDouble(),
        kMinLevelerMaxGainLinear, kMaxLevelerMaxGainLinear);
    levelerDecayMs_ = std::clamp(
        QSettings().value(QStringLiteral("tx/levelerDecayMs"),
                          kDefaultLevelerDecayMs).toInt(),
        kMinLevelerDecayMs, kMaxLevelerDecayMs);
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
    // §5 control-plane mapping (RX side): case 11 (!XmitBit) reads
    // prn->adc[0].rx_step_attn for the RX LNA gain.  +12 = the HPSDR P1
    // 0x14 bias, byte-identical to the retired composeCC RX branch
    // (0x40 | ((lnaGainDb_+12) & 0x3F)).  `db` is already clamped above.
    if (lyra::wire::prn != nullptr) {
        lyra::wire::set_rx_step_attn_db(db + 12, 0);
    }
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
    // §5 control-plane mapping (RX side) — auto-LNA path; same home as
    // setLnaGainDb (case 11 !XmitBit, +12 HPSDR P1 bias).  `db` clamped above.
    if (lyra::wire::prn != nullptr) {
        lyra::wire::set_rx_step_attn_db(db + 12, 0);
    }
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
    // Stage 2b2: read adc_overload direct from telemetry struct
    // (Ep6RecvThread writes ControlBytesIn[1]&0x01 into adc[0]
    // per HL2 single-frame assignment semantics, networkproto1.c:502).
    const bool ov = (lyra::wire::prn != nullptr
                  && lyra::wire::prn->adc[0].adc_overload != 0);
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
    // Stage 2b2: rxWorker_ retired; Ep6RecvThread is the live EP6
    // recv path and is stopped via ep6Thread_.stop() in close().
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
    // Stage 2b2: totalDg_/seqErrors_/framingErrors_/windowDg_ retired
    // from HL2Stream — Ep6RecvThread owns them as TU-scope atomics and
    // re-inits to 0 at run_loop thread entry on every start.

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
    txSendErrors_.store(0);
    // Stage 2b2: txSeq_ retired (metis_write_frame() owns the wire
    // sequence counter via the TU-scope MetisOutBoundSeqNum, shared
    // with the priming path for PureSignal-correct posture).
    // rx1DbFs_ retired (RX1 dBFS instrument removed per strict-
    // reference rule; WDSP audioDbFs is the reference-faithful path).
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

    statsClock_.start();   // baseline for actual-elapsed dg/s
    // Auto-LNA / overload monitor: fresh accumulator + hold clock.
    overloadLevel_ = 0;
    recovering_ = false;
    // Stage 2b2: adcOverloadNow_ retired (Auto-LNA reads
    // prn->adc[0].adc_overload direct in onAutoLnaTick).
    holdClock_.start();
    // Stage 2b2-fix: statsTimer_/autoLnaTimer_ .start() MOVED to
    // the very END of open() (after create_rnet + ep6Thread_.start),
    // so the timer ticks can never fire before `prn` is allocated
    // and the EP6 producer is alive.  Matches reference's
    // "allocate state first, THEN start consumers" pattern at
    // StartAudioNative (netInterface.c:32-94).  The previous order
    // had a defensive nullptr guard in onAutoLnaTick that papered
    // over the race; this restructure removes the race instead.

    // Radio memory: remember this radio so the next launch can
    // auto-connect without a Discover (read in main()).
    QSettings().setValue(QStringLiteral("radio/lastIp"), ip);

    // Step 14 Stage 1 — wire-layer init.  Reference-faithful per
    // the 2026-06-06 operator audit that caught the prior
    // `radio_net()` Meyers-singleton patch + the buffer-init split
    // across three call sites.  Now: ALL allocation lives in
    // `create_rnet()` per the reference's single allocator at
    // `netInterface.c:1590-1763`; per-session work here is just
    // wire-bind + sync-flag reset.
    //
    // Reference provenance:
    //   - `create_rnet()` allocator: netInterface.c:1590-1763.
    //     P3 (2026-06-12): the call MOVED to the main.cpp QTimer
    //     block AFTER create_xmtr() — the reference's once-per-
    //     process C#-init ordering.  The per-open call that lived
    //     here re-allocated prn on every stop/start (leak +
    //     wire-state reset, contradicting the close() "prn stays
    //     alive for re-open" contract), and its new tail
    //     registration SendpOutboundTx(OutBound) derefs
    //     pcm->xmtr[0].pilv with NO null guard — it must follow
    //     create_xmtr.
    //   - `prn` non-null contract before session-open body proceeds:
    //     netInterface.c:40 (`if (... || prn == NULL) return 3;`).
    //   - `UpdateRadioProtocolSampleSize()` at session-open:
    //     netInterface.c:45 — per-protocol sample sizes + the
    //     obbuffs ring pair (ring 0 rx audio / ring 1 tx I/Q; each
    //     create starts its ob_main pump thread, blocked on its
    //     Sem_BuffReady until a producer delivers — ring 0 has no
    //     producer until P4's RX switchover; ring 1's producer is
    //     xilv in the stream-1 cm pump, quiescent until P4 feeds
    //     Inbound(1,...)).  destroy_obbuffs(0/1) at close() per
    //     StopAudio netInterface.c:112-113.
    //   - `prn->hsendEventHandles[0,1]` + `prn->hobbuffsRun[0,1]`
    //     per-session semaphore allocation (= Lyra-native
    //     bool-flag reset in `outbound_init`):
    //     netInterface.c:68-71.  The VERBATIM HANDLE quartet
    //     creation lands at P4 with the sendProtocol1Samples
    //     thread start it pairs with.
    //   - file-scope `listenSock` global bound at session-open
    //     (= TU-scope socket fd set by `metis_wire_bind`):
    //     implicit at every `sendPacket(listenSock, ...)` call site
    //     (e.g. networkproto1.c:55, 89, 234).
    {
        // Reference per-session contract (netInterface.c:40).
        if (lyra::wire::prn == nullptr) {
            qCritical("[hl2] open(): wire layer not initialized — "
                      "create_rnet() has not run (main.cpp QTimer "
                      "init incomplete?)");
            return;
        }
        lyra::wire::UpdateRadioProtocolSampleSize();  // :45 — sizes + obbuffs ring pair
        // Bind the wire socket + extract radio IPv4 (network byte
        // order) for the per-call sockaddr_in construct in
        // `send_packet` — matches reference's `MetisAddr` file-
        // scope global init at the StartMetis discovery path.
        in_addr ipv4{};
        ::inet_pton(AF_INET, ip.toLatin1().constData(), &ipv4);
        // Mirror reference's `prn->base_outbound_port` write at
        // `netInterface.c:1598` so `send_packet` reads the right
        // port (1024 default; HL2 P1 EP2 main path).
        lyra::wire::prn->base_outbound_port = kRadioPort;
        lyra::wire::metis_wire_bind(static_cast<int>(socket_),
                                    ipv4.s_addr);
        // (§7) The OutboundRing cv-translation + its per-session
        // outbound_init() reset retired with the wire-LIVE switchover:
        // the verbatim chain pairs/refills via prn->hsendLRSem /
        // hsendIQSem / hobbuffsRun[] (created in the open() quartet),
        // not the cv flags.
    }

    // ============== Stage 2b2 — Wire-LIVE migration ==============
    //
    // Reference session-open sequence (StartAudioNative,
    // netInterface.c:32-94):
    //   1. SendStartToMetis()                            netInterface.c:50
    //   2. CreateSemaphore + _beginthreadex(MetisReadThreadMain)
    //                                                    netInterface.c:60-61
    //   3. WaitForSingleObject(hReadThreadInitSem,INF)   netInterface.c:63
    //   4. CreateSemaphore + _beginthreadex(sendProtocol1Samples)
    //                                                    netInterface.c:65-66
    //   5. hsendEventHandles + hobbuffsRun semaphore set netInterface.c:68-71
    //
    // Lyra mirrors verbatim below.  The OLD rxWorker_ jthread +
    // open-time START send inside txWorkerLoop are retired.

    // (1) — hoist START from OLD txWorkerLoop:2511-2526 to caller-
    // thread context here.  Reference: SendStartToMetis at
    // netInterface.c:50, before any thread spawn.
    {
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port   = htons(kRadioPort);
        ::inet_pton(AF_INET, ip.toLatin1().constData(), &dest.sin_addr);
        const QByteArray startPkt = buildControlPacket(true);
        const int sent = ::sendto(static_cast<SOCKET>(socket_),
                                  startPkt.constData(), startPkt.size(), 0,
                                  reinterpret_cast<sockaddr*>(&dest),
                                  sizeof(dest));
        if (sent != startPkt.size()) {
            const int wsaErr = ::WSAGetLastError();
            onFatalError(QStringLiteral("START: %1").arg(winsockError(wsaErr)));
            return;
        }
        emit logLine(QStringLiteral(
            "  START sent (0xEFFE 0x04 0x01 + 60 zeros), "
            "Ep6RecvThread engaging"));
    }

    // (2-3) — Wire Ep6RecvThread sinks BEFORE start().  Sink-
    // registration contract: registered ONCE, never reassigned
    // at runtime (assert(!running_) per Stage 2b2a).  Mirrors
    // reference's LoadRouterAll-once-at-session-open pattern
    // (router.c:111-143 called before _beginthreadex by C# console).
    //
    // Router sink is registered in main.cpp at WdspEngine setup
    // (so the IQ feed lambda has access to wdspEngine).  Here we
    // just point Ep6RecvThread at the router instance for dispatch.
    // Stage 2b2-fix-v2: router0_ HL2Stream member retired; the
    // Router is now process-singleton, created BEFORE HL2Stream
    // construction by main.cpp's `lyra::wire::create_router(0)`.
    // router_instance(0) returns the live Router; the lookup +
    // registry-lock cost is one-time per open() so it's a no-op
    // for steady-state RX/TX cadence.  Matches reference's
    // `prouter[0]` direct lookup at every xrouter call site.
    ep6Thread_.set_router(lyra::wire::router_instance(0), /*router_id=*/0);

    // Stage 2b2-fix-v2: HW-PTT-in is now a Qt-main-thread POLL of
    // `lyra::wire::prn->ptt_in` (the reference posture: wire side
    // writes the level unconditionally at `networkproto1.c:496`,
    // the FSM consumer reads it on its own clock and does its own
    // edge detect).  The previous wire-thread `set_hw_ptt_sink`
    // push-callback with `thread_local` edge-detect was a Lyra-
    // native idiom translation that the operator reference-verify
    // pass flagged as a deviation; replaced with `hwPttTimer_`
    // (started below at the end of open() alongside autoLnaTimer_)
    // which fires `onHwPttPoll()` at ~50 ms cadence on the Qt
    // main thread, mirroring the reference's
    // "consumer-polls-prn->ptt_in" shape.

    // (3) — Ep6RecvThread spawn + init-sem handshake.  start()
    // blocks until run_loop releases hReadThreadInitSem (per
    // Stage 2a: AFTER MMCSS classification, BEFORE priming +
    // WSAEventSelect — matches reference MetisReadThreadMain
    // release point at networkproto1.c:249).  Priming pass
    // (ForceCandCFrame(3) + 6 EP2 datagrams) runs async on the
    // EP6 thread after start() returns — same parallel-spawn
    // posture as reference (sendProtocol1Samples at :66 spawns
    // before the EP6 thread's priming completes).

    // §5 control-plane mapping (RX side) — at-open seed.  The verbatim
    // C&C composer (FrameComposer::write_main_loop_hl2) reads `prn` /
    // `SampleRateIn2Bits`, NOT the hl2_stream legacy atomics that the
    // retired composeCC read.  Seed those homes from the current
    // persisted operator state HERE — before ep6Thread_.start() kicks
    // off the priming pass (ForceCandCFrame reads prn->rx[0].frequency)
    // and before the writer's first compose — so RX tunes correctly
    // from the first frame instead of DDC0 sitting at 0 Hz until the
    // operator first touches the dial (dead-RX, operator-confirmed
    // 2026-06-13).  prn is guaranteed non-null here (the open() guard at
    // the top returns if create_rnet hasn't run).  Each operator setter
    // also writes its home on change (setRx1FreqHz / setLnaGainDb /
    // setSampleRate / updateOcPattern); this is the open-time snapshot.
    // Direct prn writes via the FrameComposer setters match the
    // reference (Console writes prn->rx[] from the control thread) + the
    // shipped AttOnTxPolicy precedent — no lock, no command queue.
    {
        const int rxHz =
            static_cast<int>(rx1FreqHz_.load(std::memory_order_relaxed));
        lyra::wire::set_rx_freq(0, rxHz);   // DDC0 RX1 (case 2/8/9)
        lyra::wire::set_rx_freq(1, rxHz);   // DDC1 (case 3 — RX1-mirror until RX2)
        lyra::wire::set_tx_freq(            // TX NCO + DDC2/3 mirror (case 1/5/6)
            static_cast<int>(txFreqHz_.load(std::memory_order_relaxed)));
        lyra::wire::set_rx_step_attn_db(    // LNA gain (case 11 !XmitBit)
            std::clamp(lnaGainDb_.load(std::memory_order_relaxed),
                       kLnaMinDb, kLnaMaxDb) + 12, 0);   // HPSDR P1 +12 bias
        lyra::wire::SampleRateIn2Bits =     // sample-rate code (case 0 C1)
            sampleRateBits_.load(std::memory_order_relaxed);
        lyra::wire::prn->oc_output = ocPattern_;  // OC pins (case 0 C2)

        // §5 control-plane mapping (TX side) — seed the TX homes too so
        // the first composed frame carries the operator's persisted TX
        // state (PA defaults OFF; drive default low; XmitBit 0 at open =
        // RX).  TX-inert at RX (gateware acts on these only during a
        // keyed MOX), so this does not change RX behaviour — it just
        // means the first keydown finds correct drive/PA/att already on
        // the wire rather than create-time defaults.
        lyra::wire::set_drive_level(            // drive level (case 10 C1)
            static_cast<int>(txDriveLevel_.load(std::memory_order_relaxed)));
        lyra::wire::set_pa_on(                   // PA enable (case 10 C2/C3)
            paOn_.load(std::memory_order_relaxed));
        lyra::wire::set_tx_step_attn_db(         // TX step-att (case 4/11)
            txStepAttnDb_.load(std::memory_order_relaxed));
        lyra::wire::XmitBit =                    // wire MOX gate (0 = RX at open)
            mox_.load(std::memory_order_relaxed) ? 1 : 0;
    }

    ep6Thread_.start(static_cast<int>(socket_));

    // (4) — EP2 writer thread (sendProtocol1Samples).  THE wire path
    // now (P4.b switchover) — replaces the OLD txWorker_ jthread
    // (txWorkerLoop, retired below).  VERBATIM reference order,
    // StartAudioNative netInterface.c:65-71:
    //   prn->hWriteThreadInitSem = CreateSemaphore(..);     // (see note)
    //   prn->hWriteThreadMain    = _beginthreadex(.., sendProtocol1Samples, ..);
    //   prn->hsendEventHandles[0] = (prn->hsendLRSem = CreateSemaphore(..));
    //   prn->hsendEventHandles[1] = (prn->hsendIQSem = CreateSemaphore(..));
    //   prn->hobbuffsRun[0] = CreateSemaphore(..);
    //   prn->hobbuffsRun[1] = CreateSemaphore(..);
    //
    // Thread-FIRST then the four sems — exactly as the reference.  (An
    // earlier pass reordered sems-before-thread "to remove a startup
    // race"; reverted — that is the "I have a reason to differ" trap
    // the DESIGN PRINCIPLE forbids.  The reference's order is safe in
    // practice: the new thread's AvSetMmThreadCharacteristics setup
    // gives this creating thread a large head start to finish the four
    // CreateSemaphore calls before the writer first reaches
    // WaitForMultipleObjects.  Decade-proven in Thetis; faithful port
    // protects PureSignal correctness, which depends on byte-faithful
    // wire behaviour.)
    //
    // hWriteThreadInitSem (reference netInterface.c:65): a
    // create-but-never-waited-on init sem.  In Lyra the thread-init-sem
    // MECHANISM is the Lyra-native std::counting_semaphore member
    // (RadioNet.h:605) — the same sanctioned surrounding-architecture
    // translation already used for hReadThreadInitSem; it is
    // default-constructed with prn and (matching the reference)
    // nothing waits on it, so there is no Win32 creation line to port.
    //
    // Packaging accommodation (NOT behavioral): the reference passes
    // sendProtocol1Samples directly to _beginthreadex (C); C++ requires
    // the _beginthreadex_proc_type cast — same documented class as the
    // (AVRT_PRIORITY)2 cast in NetworkProto1.cpp.
    //
    // io_keep_running == 1 is already guaranteed here: ep6Thread_.start()
    // above blocked on the read thread's init-sem, which Ep6RecvThread
    // releases only AFTER setting io_keep_running = 1 (mirrors
    // MetisReadThreadMain networkproto1.c:246).  The writer's
    // `while (io_keep_running != 0)` loop then blocks on BOTH hsend
    // sems until the RX OutBound(0) chain (hsendLRSem) AND the TX
    // OutBound(1) chain (hsendIQSem) each deliver a buffer (the §1
    // pacing model — no fresh data ⇒ writer blocks ⇒ silent wire, the
    // reference stuck-carrier safety).
    lyra::wire::prn->hWriteThreadMain = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 0,
        reinterpret_cast<_beginthreadex_proc_type>(&lyra::wire::sendProtocol1Samples),
        nullptr, 0, nullptr));
    lyra::wire::prn->hsendEventHandles[0] =
        (lyra::wire::prn->hsendLRSem = ::CreateSemaphore(nullptr, 0, 1, nullptr));
    lyra::wire::prn->hsendEventHandles[1] =
        (lyra::wire::prn->hsendIQSem = ::CreateSemaphore(nullptr, 0, 1, nullptr));
    lyra::wire::prn->hobbuffsRun[0] = ::CreateSemaphore(nullptr, 0, 1, nullptr);
    lyra::wire::prn->hobbuffsRun[1] = ::CreateSemaphore(nullptr, 0, 1, nullptr);

    // Stage 2b2-fix: timer consumers START LAST — only after
    // create_rnet() above has allocated `prn` and the EP6/TX
    // workers are alive.  Reference posture: state-allocated +
    // producers-running BEFORE any state-reading consumer ticks.
    // Removes the prior race window where autoLnaTimer fired
    // before `prn` existed (the defensive nullptr guard in
    // onAutoLnaTick is now structurally unreachable, not just
    // unobservable).
    statsTimer_.start();
    autoLnaTimer_.start();
    // Stage 2b2-fix-v2: HW-PTT poll timer starts last too — same
    // "wait until prn is allocated and EP6 producer is up" rule.
    // Reset lastHwPttLevel_ to the current wire state so the first
    // tick after open() doesn't fire a spurious edge on whatever
    // level the radio happens to be sending at session start.
    lastHwPttLevel_ = (lyra::wire::prn != nullptr
                    && lyra::wire::prn->ptt_in != 0);
    hwPttTimer_.start();
}

void HL2Stream::close() {
    // Task #40 — TX-triggered zombie shutdown investigation.  qWarning
    // brackets so the next bench shows in lyra-log.txt which step (if
    // any) wedged: rxWorker_.join, txWorker_.join, sendto STOP, or the
    // closesocket itself.
    qWarning("[shutdown] HL2Stream::close ENTRY (running=%d ep6Alive=%d txJoin=%d sockOpen=%d)",
             running_.load() ? 1 : 0,
             ep6Thread_.running() ? 1 : 0,
             txWorker_.joinable() ? 1 : 0,
             socket_ != kInvalidSocket ? 1 : 0);
    if (!running_.load(std::memory_order_acquire) &&
        !ep6Thread_.running() &&
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

    // ============== P4.b — EP2 writer teardown ==============
    //
    // Reference IOThreadStop (network.c:1434-1469) + StopAudio
    // (netInterface.c:96-114): io_keep_running=0 → wait the READ thread
    // → CloseHandle(hWriteThreadMain) + the 4 P1 sems → [StopAudio]
    // destroy_obbuffs(0/1).
    //
    // ⚠ DOCUMENTED LYRA DEVIATION (the ONLY P4.b deviation — operator-
    // approved 2026-06-13; design §6; rule #2 surrounding-architecture):
    // the reference does NOT wake or join sendProtocol1Samples — it
    // CloseHandle's the writer while it is still parked in
    // WaitForMultipleObjects(hsendEventHandles, INFINITE) and relies on
    // process-exit to reap it (a CloseHandle-while-waiting = documented
    // Win32 UB; the MW0LGE :1456 comment shows they fought crashes in
    // this very block).  VERIFIED 2026-06-13: the only
    // ReleaseSemaphore(hsend*) sites in the reference are inside
    // sendOutbound (network.c:1311-1336, the runtime producer path) —
    // there is NO shutdown-path wake anywhere in ChannelMaster, so the
    // writer is deterministically orphaned at stop.  Lyra must stop/
    // start cleanly (cb58bcb class; bench gate), so we WAKE the writer
    // (release both hsend sems once) → it observes io_keep_running=0 →
    // exits → then JOIN + CloseHandle.
    //
    // PURESIGNAL-SAFE: this runs ONLY at close(); it changes no WDSP
    // call, no wire byte, no buffer (outLRbufp/outIQbufp/OutBufp), no
    // MetisOutBoundSeqNum, and does NOT touch sendOutbound's runtime
    // sem-release path (the verbatim P2.c port).  It only governs WHEN
    // the writer THREAD stops — Lyra-native thread/process model.
    // create_xmtr's PS surface (out[3]/peer/…) is NOT torn down here
    // (that's app-quit destroy_xmtr), so it survives stop/start intact.
    lyra::wire::io_keep_running = 0;                  // ref network.c:1438
    qWarning("[shutdown] HL2Stream::close ep6Thread_.stop() - start");
    ep6Thread_.stop();   // ref: wait the read thread (producers quiesce; ob_main parks on Sem_BuffReady)
    qWarning("[shutdown] HL2Stream::close ep6Thread_.stop() - done");
    if (lyra::wire::prn != nullptr) {
        // Wake the parked writer (the accommodation the reference omits)
        // so it exits rather than leaking on a closed handle.  One final
        // EP2 frame may be emitted before it re-tests the flag — benign:
        // MOX was force-cleared above ⇒ !XmitBit ⇒ zeroed TX I/Q.
        if (lyra::wire::prn->hsendLRSem)
            ::ReleaseSemaphore(lyra::wire::prn->hsendLRSem, 1, nullptr);
        if (lyra::wire::prn->hsendIQSem)
            ::ReleaseSemaphore(lyra::wire::prn->hsendIQSem, 1, nullptr);
        qWarning("[shutdown] HL2Stream::close EP2 writer join - start");
        if (lyra::wire::prn->hWriteThreadMain) {
            ::WaitForSingleObject(lyra::wire::prn->hWriteThreadMain, 1000);
            ::CloseHandle(lyra::wire::prn->hWriteThreadMain);   // ref network.c:1462
            lyra::wire::prn->hWriteThreadMain = nullptr;
        }
        qWarning("[shutdown] HL2Stream::close EP2 writer join - done");
        // Close the 4 P1 sems (ref network.c:1464-1467).  Safe here —
        // BEFORE destroy_obbuffs as in the reference — because
        // ep6Thread_.stop() + the writer join above quiesced every
        // producer, so ob_main is parked on Sem_BuffReady and will not
        // ReleaseSemaphore a closed handle.  Null-cleared for a clean
        // re-open (open() recreates them).
        if (lyra::wire::prn->hsendIQSem)     { ::CloseHandle(lyra::wire::prn->hsendIQSem);     lyra::wire::prn->hsendIQSem = nullptr; }
        if (lyra::wire::prn->hsendLRSem)     { ::CloseHandle(lyra::wire::prn->hsendLRSem);     lyra::wire::prn->hsendLRSem = nullptr; }
        if (lyra::wire::prn->hobbuffsRun[0]) { ::CloseHandle(lyra::wire::prn->hobbuffsRun[0]); lyra::wire::prn->hobbuffsRun[0] = nullptr; }
        if (lyra::wire::prn->hobbuffsRun[1]) { ::CloseHandle(lyra::wire::prn->hobbuffsRun[1]); lyra::wire::prn->hobbuffsRun[1] = nullptr; }
        lyra::wire::prn->hsendEventHandles[0] = nullptr;
        lyra::wire::prn->hsendEventHandles[1] = nullptr;
    }

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

    // Step 14 Stage 1 — wire-layer bind teardown.  Clear the TU-scope
    // socket fd + dest pointer so any post-close call into
    // metis_write_frame() fails fast (sendto on -1) instead of writing
    // to a stale closed handle.  prn singleton stays alive — the
    // reference's prn is also non-null between session-close and the
    // next session-open (it points at a static _radionet struct), so
    // operator can re-open cleanly without re-allocating.
    lyra::wire::metis_wire_bind(-1, 0);

    // P3 (2026-06-12) — reference StopAudio (netInterface.c:112-113):
    // tear down the per-session obbuffs ring pair created by
    // UpdateRadioProtocolSampleSize at open().  destroy_obbuffs'
    // verbatim `obp.pcbuff[0] == NULL` guard covers close-before-
    // first-open; close()'s own NO-OP early-return above prevents a
    // double-close double-free (the reference does not clear the
    // obp aliases after free — P1 verbatim).
    lyra::wire::destroy_obbuffs(0);
    lyra::wire::destroy_obbuffs(1);

    statsTimer_.stop();
    autoLnaTimer_.stop();
    hwPttTimer_.stop();   // Stage 2b2-fix-v2 — HW-PTT poll
    if (adcOverload_) { adcOverload_ = false; emit adcOverloadChanged(); }
    onStatsTick();  // flush the final window so the UI shows true totals
    running_.store(false, std::memory_order_release);
    emit runningChanged();
    emit logLine(QStringLiteral(
        "stream closed: RX %1 dg (%2 seq errs, %3 framing), "
        "TX %4 keepalives (%5 send errors)")
        .arg(lyra::wire::ep6_total_datagrams())
        .arg(lyra::wire::ep6_seq_errors())
        .arg(lyra::wire::ep6_framing_errors())
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
            const qint64 lyraSeq = lyra::wire::ep6_seq_errors();
            emit logLine(QStringLiteral(
                "udp4 stats Δ (system-wide): inDatagrams=%1  "
                "noPorts=%2  inErrors=%3   (vs Lyra seqErrors=%4)")
                .arg(dRx).arg(dNp).arg(dEr)
                .arg(lyraSeq));
            // Mirror to qInfo so it lands in the captured stderr
            // log too (the operator's lyra-log.txt path).
            qInfo("[udp4 stats] Δ inDatagrams=%lld noPorts=%lld "
                  "inErrors=%lld  (vs Lyra seqErrors=%lld)",
                  static_cast<long long>(dRx),
                  static_cast<long long>(dNp),
                  static_cast<long long>(dEr),
                  static_cast<long long>(lyraSeq));
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
    const qint64 rxWin = lyra::wire::ep6_drain_window_datagrams();
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
        // Diagnostic dump of the reference EP6 slot rotation
        // (networkproto1.c:498-525).  Slot 0x18 carries no useful
        // data per gateware (`_hl2src/hl2_rtl_control.v:475`).  HL2
        // reinterpretation per `console.cs:24937-24941`: a8 C1:C2 is
        // temperature, a10 C3:C4 is PA current.
        // Stage 2b2: raw-int log reads from prn->... direct (the
        // single telemetry state of record after the Ep6RecvThread
        // migration; matches reference posture of int=0-until-first-
        // telemetry-frame).
        const int rxc = (lyra::wire::prn != nullptr)
            ? lyra::wire::prn->tx[0].exciter_power : 0;
        const int rfp = (lyra::wire::prn != nullptr)
            ? lyra::wire::prn->tx[0].fwd_power : 0;
        const int rrp = (lyra::wire::prn != nullptr)
            ? lyra::wire::prn->tx[0].rev_power : 0;
        const int rua = (lyra::wire::prn != nullptr)
            ? lyra::wire::prn->user_adc0 : 0;
        const QString s = QStringLiteral(
            "[telem] a8=(%1,%2) a10=(%3,%4) | T=%5C PA=%6A")
            .arg(rxc).arg(rfp).arg(rrp).arg(rua)
            .arg(hl2TempC(), 0, 'f', 1)
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
    // HL2 board temperature.  Reference HL2 path (`console.cs:
    // 24937-24941`) reinterprets the slot-0x08 C1:C2 byte (which the
    // reference C atomic calls `tx[0].exciter_power`) as raw temp on
    // HL2.  Formula per `console.cs:25079`:
    //   T_C = (3.26 * raw/4096 - 0.5) / 0.01
    // Gateware confirms (Y:/Claude local/_hl2src/hl2_rtl_control.v:
    // 473): `2'b01: iresp <= {... 4'h0, temperature, 4'h0, fwd_pwr}`
    // — slot 0x08 C1:C2 carries the on-board temp sensor (MCP9700,
    // 10 mV/°C, 0.5 V @ 0°C).
    //
    // Stage 2b2: read from `prn->tx[0].exciter_power` direct, mirroring
    // reference's read-_radionet-field pattern (matches reference's
    // `prn->tx[0].exciter_power = ...` write at networkproto1.c:506).
    // Wire-INERT pre-Stage-2b2; LIVE now that Ep6RecvThread::start runs.
    // Lyra-native -1 sentinel removed — reference reads int=0 at startup
    // until first telemetry frame populates the field; HL2 banner shows
    // briefly cold (~-50°C) for ~26ms post-open, matches reference UX.
    if (lyra::wire::prn == nullptr) return kNaN;
    const int raw = lyra::wire::prn->tx[0].exciter_power;
    return (3.26 * (raw / 4096.0) - 0.5) / 0.01;
}
double HL2Stream::hl2SupplyV() const {
    // Reference does NOT display supply voltage on HL2 per
    // `console.cs:26758-26761`: the status-bar "Volts" label slot is
    // reused to show temperature with a "C" suffix.  The C# accessor
    // `_MKIIPAVolts` is fed from `_voltsQueue` which is NEVER enqueued
    // on HL2 (`console.cs:24937-24941` HL2 branch only enqueues amps +
    // temp, not volts).  The HL2 gateware does not place supply in
    // the iresp 4-slot rotation either (`_hl2src/hl2_rtl_control.v:
    // 471-476` — slot 0x00 carries dsiq_status+VERSION_MAJOR; slot
    // 0x18 is "Unused in HL").
    //
    // Lyra mirrors this: V returns NaN on HL2; UI shows "n/a".  If
    // supply telemetry is wanted later, it has to come from a separate
    // I2C readback transaction (separate work item, not §3.9 / not
    // EP6).
    return kNaN;
}
double HL2Stream::paCurrentA() const {
    // HL2 PA bias current.  Reference HL2 path (`console.cs:24937-
    // 24941`) reinterprets the slot-0x10 C3:C4 byte (which the
    // reference C atomic calls `user_adc0`) as raw PA current on HL2.
    // Formula per `console.cs:25121-25131`:
    //   amps = ((3.26 * raw/4096) / 50) / 0.04 / (1000/1270)
    // Gateware confirms (`_hl2src/hl2_rtl_control.v:474`): `2'b10:
    // iresp <= {... 4'h0, rev_pwr, 4'h0, bias_current}` — slot 0x10
    // C3:C4 carries `bias_current` from the slow_adc ain2 channel.
    //
    // Stage 2b2: read `prn->user_adc0` direct (Ep6RecvThread writes
    // this from slot 0x10 C3:C4 per networkproto1.c:513).
    if (lyra::wire::prn == nullptr) return kNaN;
    const int raw = lyra::wire::prn->user_adc0;
    return ((3.26 * (raw / 4096.0)) / 50.0) / 0.04 / (1000.0 / 1270.0);
}
double HL2Stream::fwdPowerW() const {
    // Stage 2b2: read `prn->tx[0].fwd_power` direct (Ep6RecvThread
    // writes from slot 0x08 C3:C4 per networkproto1.c:507).
    if (lyra::wire::prn == nullptr) return kNaN;
    const int raw = lyra::wire::prn->tx[0].fwd_power;
    // Provisional + UNCALIBRATED — real watts need the per-band 3-point
    // forward-power cal (a later TX-3 step).
    const double v = (raw - 6.0) / 4095.0 * 3.3;
    return (v > 0.0) ? (v * v) / 1.5 : 0.0;
}
double HL2Stream::revPowerW() const {
    // Stage 2b2: read `prn->tx[0].rev_power` direct (Ep6RecvThread
    // writes from slot 0x10 C1:C2 per networkproto1.c:511).
    if (lyra::wire::prn == nullptr) return kNaN;
    const int raw = lyra::wire::prn->tx[0].rev_power;
    const double v = (raw - 6.0) / 4095.0 * 3.3;
    return (v > 0.0) ? (v * v) / 1.5 : 0.0;
}

// ----------------------------------------------------------------
// Stage 2b2 (operator-locked 2026-06-07, "do as the reference does
// — period"): HL2Stream::rxWorkerLoop body retired.  The EP6 recv
// path is now `wire::Ep6RecvThread` (started from HL2Stream::open
// — see ep6Thread_.start() call site).  Per-DDC IQ dispatch goes
// through `wire::Router::xrouter` to whichever consumer main.cpp
// registered (WDSP feed via lyra::wire::register_sink at app boot).
// HW PTT-in level forwards via the `set_hw_ptt_sink` callback wired
// in HL2Stream::open (edge-detect lambda-local; reference posture =
// no wire-side edge state, `prn->ptt_in` is a single-bit level).
// Telemetry, mic decimation, ADC overload, seq/total/framing/window
// counters all live in `wire::Ep6RecvThread` + `wire/RadioNet` +
// TU-scope statics — single source of truth per reference's
// _radionet + file-scope-global pattern (network.h / networkproto1.c).
// ----------------------------------------------------------------

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

        // §5 control-plane mapping (RX side): the verbatim
        // write_main_loop_hl2 tunes the DDCs from prn->rx[].frequency /
        // prn->tx[0].frequency (NOT rx1FreqHz_ — that was the retired
        // composeCC's source).  Write the verbatim homes on every dial
        // change so DDC0 follows the operator.  `if (prn)` guards the
        // pre-open window (a tune gesture before the wire layer is up
        // is captured by the at-open seed in open()).
        if (lyra::wire::prn != nullptr) {
            const int hzi = static_cast<int>(hz);
            lyra::wire::set_rx_freq(0, hzi);  // DDC0 (case 2/8/9)
            lyra::wire::set_rx_freq(1, hzi);  // DDC1 (case 3 — until RX2)
            // TX NCO + DDC2/3 (case 1/5/6).  txDdsHzForTune applies the
            // ∓cw_pitch TUN offset when tuning (zero-beat) — recomputed
            // on every dial change so a retune mid-tune stays zero-beat.
            lyra::wire::set_tx_freq(txDdsHzForTune(hz));
        }
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
        // §5 (TX): keep the wire gate (global XmitBit) coherent with the
        // raw MOX bit so diag/test tools that bypass the FSM still gate
        // the writer correctly.
        lyra::wire::XmitBit = on ? 1 : 0;
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
        // §5 (TX): prn->tx[0].frequency (case 1/5/6).  In simplex,
        // setRx1FreqHz mirrors this; the standalone setter covers
        // split-mode / direct TX-freq paths.
        if (lyra::wire::prn != nullptr)
            lyra::wire::set_tx_freq(static_cast<int>(hz));
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
    // §5 (TX): prn->tx[0].drive_level (compose_case_10 C1).
    if (lyra::wire::prn != nullptr) lyra::wire::set_drive_level(clamped);
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
    const int clamped = std::clamp(db, 0, 31);
    txStepAttnDb_.store(clamped, std::memory_order_relaxed);
    // §5 (TX): compose_case_4 (C3, 5-bit) + compose_case_11 (C4, 6-bit,
    // XmitBit-gated) read prn->adc[0].tx_step_attn.  set_tx_step_attn_db
    // stores the HL2 (31 - db) wire encoding — byte-identical to the
    // retired composeCC's (31 - txStepAttnDb_).  The FSM ATT-on-TX raise
    // (fsmAdvance → setTxStepAttnDb(kAttOnTxDb)) reaches the wire here.
    if (lyra::wire::prn != nullptr) lyra::wire::set_tx_step_attn_db(clamped);
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
    // §5 (TX): ApolloTuner/ApolloFilt globals + prn->tx[0].pa
    // (compose_case_10 C2 bit3 active-high PA-enable + C3 legacy bit).
    if (lyra::wire::prn != nullptr) lyra::wire::set_pa_on(on);
    QSettings().setValue(QStringLiteral("tx/paEnabled"), on);
    emit paEnabledChanged(on);
    safetyLog(QStringLiteral("TX: PA enable -> %1")
              .arg(on ? QStringLiteral("ON  (RF possible on next key)")
                      : QStringLiteral("off (PA bias disarmed)")));
}

void HL2Stream::setMicBoost(bool on) {
    // Task #39 — HL2 +20 dB hardware Mic Boost via the codec PGA.
    // Per the reference's SetMicBoost (netInterface.c:536-544 +
    // network.h:220-221 mic_boost : 1) the wire is a single bit
    // at C0 0x12 C2 bit 0: hardware is 2-state Off / +20 dB only.
    // Intermediate boost levels come from the operator Mic Gain
    // slider (Task #32) applied AFTER this hardware boost stage
    // — the two compose: HW +20 dB boost feeds the WDSP TXA
    // mic-input panel where the operator's Mic Gain slider
    // applies digital trim.
    //
    // No safety implication (purely audio-level), no MOX gating
    // (the bit can flip on RX without affecting any wire RF).
    // Persisted to QSettings; recalled on stream open.
    const bool prev = micBoost_.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    QSettings().setValue(QStringLiteral("tx/micBoost"), on);
    emit micBoostChanged(on);
    safetyLog(QStringLiteral("TX: Mic Boost -> %1")
              .arg(on ? QStringLiteral("ON  (+20 dB HW)")
                      : QStringLiteral("off (0 dB HW)")));
}

int HL2Stream::txDdsHzForTune(quint32 dialHz) const {
    // P4.b TUN zero-beat — Thetis tx_freq computation gated on chkTUN
    // (console.cs:32574-32587): while tuning, offset the TX NCO by
    // ∓cw_pitch (USB −, LSB +) so the ±cw_pitch postgen tone (main.cpp
    // setTune) cancels to a carrier at the dial.  Not tuning → dial
    // unchanged.  Same kTuneCwPitchHz both sides → they cancel.
    if (!tuneEnabled_.load(std::memory_order_relaxed))
        return static_cast<int>(dialHz);
    const int off = (txMode_.load(std::memory_order_relaxed) == 1)
        ? -kTuneCwPitchHz    // USB: dial − cw_pitch
        : +kTuneCwPitchHz;   // LSB: dial + cw_pitch
    return static_cast<int>(dialHz) + off;
}

int HL2Stream::txAnalyzerOffsetHz() const {
    // = txDdsHzForTune(d) − d (freq-independent): the NCO−dial offset the
    // panadapter crop must undo so the TUN carrier paints on the marker.
    if (!tuneEnabled_.load(std::memory_order_relaxed)) return 0;
    return (txMode_.load(std::memory_order_relaxed) == 1)
        ? -kTuneCwPitchHz    // USB: NCO = dial − cw_pitch
        : +kTuneCwPitchHz;   // LSB: NCO = dial + cw_pitch
}

void HL2Stream::setTuneEnabled(bool on) {
    // Arms / disarms the tune carrier.  P4.b re-home: the carrier now
    // comes from the WDSP TXA output-side tone generator (postgen /
    // gen1) via txControl_.setTune — the Thetis TUN mechanism
    // (console.cs:30787-30801: SetTXAPostGenMode(0) single tone +
    // ToneFreq ±pitch + ToneMag MAX_TONE_MAG + Run 1).  The legacy
    // host-streamed DC injection (EP2 packer ~0.95-fs into TX-I) died
    // with the wire-live switchover, so it's replaced here.  The TXA
    // channel armed by the MOX keydown (requestMox, fired alongside this
    // by the TUN button) processes the postgen tone → carrier → wire.
    //
    // Drive % scales the on-air power.  NOT persisted (TUN is an explicit
    // gesture) and auto-cleared on every wire-MOX-off edge by the ctor's
    // self-wired safety.
    const bool prev = tuneEnabled_.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    // Forward to the registered postgen callback (no-op if TX control
    // not yet registered — same pattern as the other setters).
    {
        std::function<void(bool)> fwd;
        {
            std::lock_guard<std::mutex> lk(txControlMtx_);
            fwd = txControl_.setTune;
        }
        if (fwd) fwd(on);
    }
    // P4.b TUN zero-beat: re-push the TX NCO so the ∓cw_pitch DDS offset
    // is applied (on) / removed (off) — txDdsHzForTune reads the
    // tuneEnabled_ just set above.  Pairs with the ±cw_pitch postgen tone
    // (the setTune fwd) → net carrier at the dial (Thetis chkTUN order).
    if (lyra::wire::prn != nullptr) {
        const quint32 dial = txFreqHz_.load(std::memory_order_relaxed);
        const int dds = txDdsHzForTune(dial);
        lyra::wire::set_tx_freq(dds);
        // TUN-zero-beat diagnostic: shows the dial vs the offset TX NCO so
        // the bench log pins where the carrier should land vs where it does.
        qInfo("[tx] TUN %s: dial=%u txDds(NCO)=%d off=%d mode=%d(USB=1) cw_pitch=%d"
              "  (gen1 tone = %s%d; net carrier should = dial)",
              on ? "ON" : "off",
              dial, dds, dds - static_cast<int>(dial),
              txMode_.load(std::memory_order_relaxed), kTuneCwPitchHz,
              (txMode_.load(std::memory_order_relaxed) == 1) ? "+" : "-",
              kTuneCwPitchHz);
    }
    emit tuneEnabledChanged(on);
    // P4.b TUN display-honesty: tell the panadapter the NCO−dial offset so
    // its TX-state crop renders the carrier at the dial (on the marker).
    emit txAnalyzerOffsetChanged(txAnalyzerOffsetHz());
    safetyLog(QStringLiteral("TX: tune-carrier -> %1")
              .arg(on ? QStringLiteral("ARMED  (WDSP postgen single tone, ±1 kHz per sideband — emits while MOX active)")
                      : QStringLiteral("disarmed (postgen stopped)")));
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

// Stage 2b2-fix-v2 — HW-PTT-in poll slot, Qt main thread.
// Mirrors the reference's "FSM consumer polls prn->ptt_in and
// edge-detects on its own clock" posture (no wire-thread push
// callback, no QueuedConnection slot hop per datagram).  Reads
// the level the EP6 thread wrote at the most-recent status
// decode (`Ep6RecvThread.cpp:866` unconditional write per the
// reference `networkproto1.c:496` semantic).  Edge state lives
// in `lastHwPttLevel_` (plain bool, Qt-main-thread sole writer
// + reader — no synchronisation needed).  Opt-in gated by
// `hwPttEnabled_` per §10 Q#1 (N8SDR HL2+/AK4951 non-zero
// ptt_in at RX rest; default-OFF safety until operator opts
// in after a per-unit bench check).
void HL2Stream::onHwPttPoll() {
    if (lyra::wire::prn == nullptr) return;          // stream torn down
    if (!hwPttEnabled_.load(std::memory_order_acquire)) {
        // Gate closed — keep lastHwPttLevel_ tracking the wire
        // so a re-enable after a level change doesn't fire a
        // spurious edge on the next tick.
        lastHwPttLevel_ = (lyra::wire::prn->ptt_in != 0);
        return;
    }
    const bool now = (lyra::wire::prn->ptt_in != 0);
    if (now != lastHwPttLevel_) {
        lastHwPttLevel_ = now;
        requestMoxFromHwPtt(now);
    }
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
    // per send) — coherent.  Reference parity per console.cs:30332
    // (HdwMOXChanged → SetTRXrelay(1) + SetPttOut(1)) + console.cs:
    // 30335 (cmaster.Mox = tx — loads XmitBit the EP2 packer reads).
    // Lyra collapses HdwMOXChanged + cmaster.Mox into ONE atomic
    // since the EP2 packer's C0-bit-0 emission IS XmitBit.
    mox_.store(true, std::memory_order_release);
    // §5 control-plane mapping (TX): the verbatim write_main_loop_hl2
    // (compose_case_0 C0 + the writer's `!XmitBit ⇒ memset(outIQbufp)`
    // gate) reads the GLOBAL XmitBit — NOT mox_, the FSM atomic the
    // retired composeCC snapshotted.  Bridge it here, at the same site
    // the reference loads it (cmaster.Mox = tx, console.cs:30335), so
    // the wire MOX bit sets AND the writer stops zeroing — letting the
    // mic→TXA→OutBound(1) modulator I/Q reach the wire.  Set alongside
    // mox_ so the §15.25 keydown ordering already in place governs it.
    lyra::wire::XmitBit = 1;

    // §15.25 keydown ordering CORRECTED 2026-06-09 per Thetis
    // console.cs:30342-30345 read 3× verified:
    //   line 30342-30343:  if (rf_delay > 0) Thread.Sleep(rf_delay);
    //   line 30344:        AudioMOXChanged(tx);
    //   line 30345:        WDSP.SetChannelState(WDSP.id(1, 0), 1, 0);
    //
    // i.e. WDSP TXA channel start happens AFTER rf_delay, NOT BEFORE.
    // The previous Lyra ordering (startFn + setInjectTxIq before the
    // QTimer) began the WDSP TXA cos² up-ramp while the gateware T/R
    // relay was still settling for the full rfDelayMs_ window — an
    // external linear with hot-switch protection saw TX I/Q on the
    // wire during its own settle period.  Fixed by moving startFn()
    // and setInjectTxIq(true) into fsmKeydownSettled (after the rf_delay
    // QTimer fires), which is exactly Thetis's order.
    QTimer::singleShot(rfDelayMs_, this,
                       [this]() { fsmKeydownSettled(); });
}

void HL2Stream::fsmKeydownSettled() {
    if (!requestedMox_) {
        // Cancelled mid-rf_delay — wire MOX already went on (set in
        // fsmKeydownPostMox), but WDSP TXA has NOT started yet (the
        // §15.25 keydown-ordering CORRECTION moved that to here).
        // Unwind through the real keyup chain: stopFn() is a no-op
        // when the channel was never started (TxChannel.stop guards
        // on running_), so the unwind is safe.
        safetyLog(QStringLiteral(
            "TX: keydown cancelled mid-rf_delay; initiating keyup"));
        QTimer::singleShot(spaceMoxDelayMs_, this,
                           [this]() { fsmKeyupPostSpace(); });
        return;
    }
    // §15.25 keydown ordering CORRECTED 2026-06-09 per Thetis
    // console.cs:30344-30345 (read 3× verified):
    //   line 30344:  AudioMOXChanged(tx);     // audio routing FIRST
    //   line 30345:  WDSP.SetChannelState(WDSP.id(1, 0), 1, 0);
    //                                          // TX TXA start LAST
    //
    // setInjectTxIq(true) is the Lyra equivalent of AudioMOXChanged
    // (arms the EP2 packer's SSB-IQ branch); startFn() is the Lyra
    // equivalent of SetChannelState(id(1,0),1,0) (starts the WDSP
    // TXA cos² up-ramp).  This runs AFTER the rfDelayMs_ QTimer
    // fired — so the gateware T/R relay has settled and the linear's
    // hot-switch protection window has elapsed before any TX I/Q
    // hits the wire.  Reference parity restored.
    setInjectTxIq(true);
    {
        std::function<void()> startFn;
        {
            std::lock_guard<std::mutex> lk(txControlMtx_);
            startFn = txControl_.start;
        }
        if (startFn) startFn();
    }
    moxActive_ = true;
    emit moxActiveChanged(true);
    safetyLog(QStringLiteral("TX: MOX_TX (wire MOX + TXA settled)"));
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

    // §15.25 keyup ordering CORRECTED 2026-06-09 — read Thetis
    // console.cs:30350-30383 THREE TIMES verified.  The previous
    // codification of this invariant said "(1) Clear inject_tx_iq
    // first" which was REFERENCE-DIVERGENT — Thetis actually does:
    //   line 30352-30353:  if (space_mox_delay > 0)
    //                          Thread.Sleep(space_mox_delay);
    //   line 30355:        _mox = tx;  // C# in-memory only
    //   line 30357:        WDSP.SetChannelState(WDSP.id(1,0), 0, 1);
    //                                            // TXA stop BLOCKING (FIRST)
    //   line 30367-30368:  if (mox_delay > 0)
    //                          Thread.Sleep(mox_delay);
    //   line 30373:        AudioMOXChanged(tx);  // EP2 branch zero
    //   line 30374:        HdwMOXChanged(tx, freq);  // SetPttOut(0)
    //   line 30376:        cmaster.Mox = tx;     // XmitBit = 0
    //   line 30377-30378:  if (ptt_out_delay > 0)
    //                          Thread.Sleep(ptt_out_delay);
    //   line 30379:        WDSP.SetChannelState(WDSP.id(0,0), 1, 0);
    //                                            // RX restart
    //
    // The KEY: WDSP TXA stop BLOCKING is FIRST (so the cos² downslew
    // tail flows out fexchange0 → EP2 packer → wire WHILE XmitBit is
    // still 1).  AudioMOXChanged (= Lyra's setInjectTxIq(false)) only
    // fires AFTER mox_delay.  HdwMOXChanged + cmaster.Mox (= Lyra's
    // mox_.store(false)) fire right after — they're the actual wire
    // MOX-bit drop.  Then ptt_out_delay → RX restart.
    //
    // Lyra ordering (post-correction):
    //   fsmKeyupPostSpace:  stopFn() BLOCKING, then QTimer(txStopDelayMs_)
    //                       → fsmKeyupTxOff
    //   fsmKeyupTxOff:      setInjectTxIq(false), mox_.store(false),
    //                       then QTimer(pttOutDelayMs_) → fsmKeyupSettled
    //   fsmKeyupSettled:    (RX restart N/A in Lyra-cpp v0.2.0 SSB —
    //                       RX-DSP stays running through TX with audio
    //                       muted via _tx_rx_muted gate; re-evaluate at
    //                       v0.2.2 CW work).  Cleanup + emit.
    //
    // The fsmKeyupInFlight stage in the previous design folded into
    // fsmKeyupTxOff post-correction (its only job — clear wire MOX bit
    // after txStopDelayMs_ — now happens in fsmKeyupTxOff alongside
    // setInjectTxIq).
    {
        std::function<void()> stopFn;
        {
            std::lock_guard<std::mutex> lk(txControlMtx_);
            stopFn = txControl_.stop;
        }
        if (stopFn) stopFn();
    }
    // Schedule the in-flight clear wait.  Thetis line 30367-30368
    // mox_delay (default 10 ms) lets WDSP-internal cos² downslew
    // samples emitted post-stop reach the HL2 + be packed at the
    // wire WHILE XmitBit is still 1, so the operator's keyed-tail
    // SSB envelope arrives on-air cleanly rather than being chopped
    // by a premature AudioMOXChanged.
    QTimer::singleShot(txStopDelayMs_, this,
                       [this]() { fsmKeyupTxOff(); });
}

void HL2Stream::fsmKeyupTxOff() {
    // §15.25 keyup ordering CORRECTED 2026-06-09 — this step now
    // implements Thetis console.cs:30373-30376 (the two-step wire
    // teardown: AudioMOXChanged → HdwMOXChanged + cmaster.Mox).
    //
    // (1) setInjectTxIq(false) is Lyra's AudioMOXChanged equivalent
    //     — arms the EP2 packer's else-branch (zero TX I/Q) regardless
    //     of XmitBit.  Until this fires, the EP2 packer continues to
    //     emit whatever the (stopped) WDSP TXA chain has left in
    //     out[0] — which after stopFn() BLOCKING + txStopDelayMs_ is
    //     a fully-faded silent buffer.
    //
    // (2) mox_.store(false) is Lyra's HdwMOXChanged + cmaster.Mox
    //     equivalent — flips C0 bit 0 on the next EP2 datagram, which
    //     the HL2 gateware sees as PTT-out drop + T/R relay release.
    setInjectTxIq(false);
    mox_.store(false, std::memory_order_release);
    // §5 control-plane mapping (TX): drop the wire MOX gate alongside
    // mox_ — the writer resumes `!XmitBit ⇒ memset(outIQbufp)` so TX I/Q
    // goes silent.  Fires AFTER the keyup TXA-stop blocking flush +
    // txStopDelayMs_ (§15.25), so the faded cos² tail already reached
    // the wire while XmitBit was still 1.
    lyra::wire::XmitBit = 0;
    // ptt_out_delay (Thetis line 30377-30378) gives the hardware T/R
    // relay time to physically switch back to RX before any RX-side
    // restoration logic runs.  Then fsmKeyupSettled does cleanup.
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
    // happens AFTER the wire MOX bit has cleared (in fsmKeyupTxOff per
    // the §15.25 corrected order) AND ptt_out_delay has elapsed — so
    // the filter board only returns to RX configuration when no RF is
    // being emitted.  Mirror of the fsmAdvance keydown raise; symmetric
    // hot-switch safety.
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
// Stage 2b2: the edge-detect previous level now lives in a
// thread_local inside the Ep6RecvThread hw_ptt_sink lambda
// (registered at open()), so setHwPttEnabled() no longer needs
// to reach into the wire thread's state — toggling the gate
// from the main thread is sufficient because the lambda's
// thread_local prev is reset implicitly across stop()/start()
// cycles (the Ep6RecvThread re-enters its run_loop with
// fresh-thread storage on every start).  During an in-session
// toggle, a stale prev=true on a re-enable is harmless: the
// new-level read settles to the live wire value within a few
// EP6 frames and the next genuine edge fires correctly.
void HL2Stream::setHwPttEnabled(bool on) {
    const bool prev = hwPttEnabled_.exchange(on, std::memory_order_release);
    if (prev == on) return;
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

void HL2Stream::setAlcMaxGainLinear(double linear) {
    // §15.27 — pass-through LINEAR factor (matches the verified
    // reference's Setup spinner-to-SetTXAALCMaxGain plumbing
    // exactly: no unit conversion at any layer between operator
    // value and WDSP API).  Clamped to [0, 120] LINEAR to match
    // the reference's spinner range.
    const double clamped = std::clamp(linear,
                                      kMinAlcMaxGainLinear,
                                      kMaxAlcMaxGainLinear);
    if (clamped == alcMaxGainLinear_) return;
    alcMaxGainLinear_ = clamped;
    QSettings().setValue(QStringLiteral("tx/alcMaxGainLinear"), clamped);
    emit alcMaxGainLinearChanged(clamped);
    std::function<void(double)> fwd;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setAlcMaxGainLinear;
    }
    if (fwd) fwd(clamped);
}

// §15.27 Commit B — ALC decay (operator-tunable wcpagc release tau).
// Same persistence + forward pattern as setAlcMaxGainLinear.
void HL2Stream::setAlcDecayMs(int decay_ms) {
    const int clamped = std::clamp(decay_ms,
                                   kMinAlcDecayMs,
                                   kMaxAlcDecayMs);
    if (clamped == alcDecayMs_) return;
    alcDecayMs_ = clamped;
    QSettings().setValue(QStringLiteral("tx/alcDecayMs"), clamped);
    emit alcDecayMsChanged(clamped);
    std::function<void(int)> fwd;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setAlcDecayMs;
    }
    if (fwd) fwd(clamped);
}

// §15.27 Commit B — Leveler enable.  Forwards the CURRENT max-gain
// value alongside the enable state because the WDSP run-flag setter
// pairs with the ceiling (we always want them coherent on every
// state change).  Same persistence + forward pattern as above.
void HL2Stream::setLevelerOn(bool on) {
    if (on == levelerOn_) return;
    levelerOn_ = on;
    QSettings().setValue(QStringLiteral("tx/levelerOn"), on);
    emit levelerOnChanged(on);
    std::function<void(bool,double)> fwd;
    double topLinear;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setLevelerOn;
        topLinear = levelerMaxGainLinear_;
    }
    if (fwd) fwd(on, topLinear);
}

// §15.27 Commit B — Leveler max-gain ceiling (LINEAR factor,
// matches the verified reference's Setup spinner-to-SetTXALevelerTop
// plumbing exactly).  Clamped to [0, 20] LINEAR to match the
// reference's spinner range (narrower than ALC's 0..120).  Forwards
// the CURRENT enable state alongside the new ceiling so the WDSP
// pair stays coherent.
void HL2Stream::setLevelerMaxGainLinear(double linear) {
    const double clamped = std::clamp(linear,
                                      kMinLevelerMaxGainLinear,
                                      kMaxLevelerMaxGainLinear);
    if (clamped == levelerMaxGainLinear_) return;
    levelerMaxGainLinear_ = clamped;
    QSettings().setValue(QStringLiteral("tx/levelerMaxGainLinear"), clamped);
    emit levelerMaxGainLinearChanged(clamped);
    std::function<void(bool,double)> fwd;
    bool on;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setLevelerOn;
        on  = levelerOn_;
    }
    if (fwd) fwd(on, clamped);
}

// §15.27 Commit B — Leveler decay (exponential-curve tau in ms).
void HL2Stream::setLevelerDecayMs(int decay_ms) {
    const int clamped = std::clamp(decay_ms,
                                   kMinLevelerDecayMs,
                                   kMaxLevelerDecayMs);
    if (clamped == levelerDecayMs_) return;
    levelerDecayMs_ = clamped;
    QSettings().setValue(QStringLiteral("tx/levelerDecayMs"), clamped);
    emit levelerDecayMsChanged(clamped);
    std::function<void(int)> fwd;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        fwd = txControl_.setLevelerDecayMs;
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
    // Mirror the WDSP TX mode so txDdsHzForTune signs the TUN DDS offset
    // (USB −cw_pitch / LSB +cw_pitch) correctly.
    txMode_.store(clamped, std::memory_order_relaxed);
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
    // If tuning when the sideband changes, re-push the TX NCO so the
    // DDS offset flips sign with the mode (keeps the tune carrier
    // zero-beat).  Rare (mode change mid-tune) but kept coherent.
    if (tuneEnabled_.load(std::memory_order_relaxed) && lyra::wire::prn != nullptr) {
        lyra::wire::set_tx_freq(
            txDdsHzForTune(txFreqHz_.load(std::memory_order_relaxed)));
        // P4.b TUN display-honesty: the NCO offset sign flipped with the
        // sideband — re-tell the panadapter so the crop stays on the marker.
        emit txAnalyzerOffsetChanged(txAnalyzerOffsetHz());
    }
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
// component-8a setMicGainDb/setAlcMaxGainLinear).  See TxControl
// struct doc in the header for the contract.  Same mutex pattern as
// registerTxIqSource.
//
// TX-1 component 8a — registration ALSO pushes the autoloaded
// micGainDb_ + alcMaxGainLinear_ to the channel ONCE so the freshly-
// opened WDSP TXA channel doesn't sit at WDSP create-time defaults
// (especially the load-bearing ALC max-gain = 1.0 LINEAR trap that
// pins the TXA output chain at a 0 dB ALC ceiling).  This is the
// architectural equivalent of the verified reference's Setup first-
// load pushing the operator's persisted profile values onto the
// channel.  Capture the callbacks INSIDE the lock, release the
// lock, then call OUTSIDE the lock — TxChannel's own channelMtx_
// will serialise the WDSP setter call against any in-flight
// process() on the worker thread.
void HL2Stream::registerTxControl(TxControl ctl) {
    std::function<void(double)>       pushMic;
    std::function<void(double)>       pushAlc;
    std::function<void(int)>          pushAlcDecay;
    std::function<void(bool,double)>  pushLeveler;
    std::function<void(int)>          pushLevelerDecay;
    bool   levOn;
    double levTop;
    {
        std::lock_guard<std::mutex> lk(txControlMtx_);
        txControl_ = std::move(ctl);
        // Only push if the callback landed — a clear ({}) call
        // gives us null function objects, in which case we just
        // dropped the registration and there's nothing to push.
        pushMic          = txControl_.setMicGainDb;
        pushAlc          = txControl_.setAlcMaxGainLinear;
        pushAlcDecay     = txControl_.setAlcDecayMs;
        pushLeveler      = txControl_.setLevelerOn;
        pushLevelerDecay = txControl_.setLevelerDecayMs;
        levOn            = levelerOn_;
        levTop           = levelerMaxGainLinear_;
    }
    if (pushMic)          pushMic(micGainDb_);
    if (pushAlc)          pushAlc(alcMaxGainLinear_);
    if (pushAlcDecay)     pushAlcDecay(alcDecayMs_);
    if (pushLeveler)      pushLeveler(levOn, levTop);
    if (pushLevelerDecay) pushLevelerDecay(levelerDecayMs_);
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
    // §5 control-plane mapping (RX side): compose_case_0 C1 reads the
    // `SampleRateIn2Bits` global (NOT sampleRateBits_, the retired
    // composeCC source).  Plain global — no prn guard needed; valid at
    // all times.  The at-open seed covers the initial rate.
    lyra::wire::SampleRateIn2Bits = bits;
    // TX Component 3 — keep the mic decimation factor coherent with
    // the new wire rate so the 48 kHz mic output stays at 48 kHz across
    // rate switches (matches the C reference's `SetDDCRate`
    // factor table at netInterface.c:1328-1354).  The
    // `mic_decimation_count_` does NOT need to reset across normal rate
    // switches — same-direction phase walk-in stabilises within one
    // datagram; the RX-worker-thread-only counter is safe to leave alone.
    // Stage 2b2: route the mic decimation factor to the TU-scope
    // free variable that Ep6RecvThread's mic-tap reads (replaces the
    // retired HL2Stream::micDecimationFactor_ atomic).  Plain int per
    // reference; single-writer (setSampleRate on Qt main) vs single-
    // reader (Ep6RecvThread tap) is benign on the wire-rate codepath.
    lyra::wire::mic_decimation_factor = micDec;
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
        const bool micBoost = micBoost_.load(std::memory_order_relaxed);
        // Task #39 — Mic Boost bit (C2 bit 0 / 0x01) per the
        // reference's standard SetMicBoost (networkproto1.c:748-751
        // + network.h:220-221 mic_boost : 1).  +20 dB analog PGA
        // gain in the codec's mic preamp.  Independent of PA — the
        // bits compose freely.  bit 7 (VNA) must remain clear for
        // PA keying (control.v:359).
        // Local renamed from `mb` → `micBoost` to dissolve the
        // shadow of the function-scope `mb` (MOX bit captured by
        // setAddr lambda at line ~2314).  Cosmetic; no wire-byte
        // change.
        out[2] = static_cast<std::uint8_t>((pa ? 0x08 : 0x00)
                                           | (micBoost ? 0x01 : 0x00));
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
    // §5 control-plane mapping (RX side): compose_case_0 C2 derives the
    // OC pins as ((prn->oc_output << 1) & 0xFE).  Write the raw pattern
    // home (composer does the shift) — the retired composeCC read ocC2_.
    // `if (prn)` guards the pre-open window (covered by the at-open seed).
    if (lyra::wire::prn != nullptr) {
        lyra::wire::prn->oc_output = pattern;
    }
    // C2[7:1] = OC pins; C2[0] (CW key bit) stays 0.  Atomic store so the
    // EP2 writer thread picks it up on the next send.  (ocC2_ is the
    // retired composeCC source — left until the §7 retirement cleanup.)
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

void HL2Stream::txWorkerLoop(std::stop_token stop, SocketHandle sh) {
    // Stage 2b2: `sh` (socket fd) carried for the audio_cv timing
    // path but the EP2 send itself now goes through
    // lyra::wire::metis_write_frame() which reads its socket fd +
    // dest from the TU-scope MetisAddr bound at session open.  No
    // local sockaddr_in needed.  Reference: networkproto1.c:216-237
    // MetisWriteFrame → network.c:1382-1402 sendPacket → MetisAddr
    // file-scope global.
    //
    // START packet send is now hoisted to HL2Stream::open() body
    // (matches reference SendStartToMetis posture at
    // netInterface.c:50 — caller thread, BEFORE thread spawn).
    (void) sh;  // socket fd reserved for any future direct-sendto
                // diagnostic; the main EP2 path uses
                // metis_write_frame() exclusively.

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
        // Stage 2b2: sequence number + HPSDR header construction
        // moved into lyra::wire::metis_write_frame() (called at the
        // bottom of this lambda).  Reference posture: ONE shared
        // outbound seq counter (MetisOutBoundSeqNum at
        // networkproto1.c:30); priming + EP2 writer both consume
        // it via MetisWriteFrame at networkproto1.c:216-237.
        // PureSignal-load-bearing — calcc/iqc assume single
        // continuous outbound stream.
        // The template's bytes [0..7] (HPSDR header) are no longer
        // patched here; metis_write_frame() builds the header
        // internally + memcpys our 1024-byte USB-frames payload
        // from pktBytes+8.  Those template bytes become dead-space
        // (harmless — never read by metis_write_frame).

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
        // §3.9-5 revert (operator-rejected 2026-06-06): MoxEdgeFade
        // deleted.  Reference does not envelope-shape SSB TX I/Q
        // (WDSP TXAUslewCheck returns 0 for SSB modes).  EP2 packer
        // emits raw TX I/Q samples per the reference's universal
        // posture: `!XmitBit ⇒ zero`, otherwise pass the modulator
        // output through unmodified.  Hot-switch protection for an
        // external linear is rfDelayMs_ (TR sequencing) — same
        // mechanism the reference uses.
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
        // §3.9-5 revert: no host-side envelope.  TUN carrier is
        // emitted at constant amplitude per the reference's gen0
        // posture (a constant DC injection on the I channel); SSB
        // I/Q passes WDSP TXA's settled output unmodified.
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
            qint16 txI;
            qint16 txQ;
            if (emitTone) {
                // TUN priority path — DC carrier.
                txI = static_cast<qint16>(
                    kTuneCarrierFullScale + 0.5f);
                txQ = qint16{0};
            } else if (emitSsb) {
                // SSB I/Q, quantized via the reference's symmetric
                // round-to-nearest (see docs/architecture/
                // tx1_ssb_design.md §5.7 for the verified cite):
                //   x >= 0 ? floor(x * 32767 + 0.5) : ceil(x * 32767 - 0.5)
                // float saturates to qint16 range via clamp to
                // [-32768, 32767] before the int cast (defensive —
                // the WDSP TXA chain's ALC should keep output in
                // [-1, +1) but cap anyway).
                const float scI = ssbBuf[i].real() * 32767.0f;
                const float scQ = ssbBuf[i].imag() * 32767.0f;
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

        // Stage 2b2: route through shared wire-layer primitive.
        // metis_write_frame() builds the 8-byte HPSDR header (sync +
        // endpoint=0x02 + BE seq from shared g_metis_out_seq_num) +
        // memcpys 1024-byte payload from pktBytes+8 + holds
        // prn->sndpkt around sendto (matches reference's
        // MetisWriteFrame + sendPacket chain at networkproto1.c:216-
        // 237 + network.c:1382-1402).  Returns payload bytes sent
        // (negative on socket error — matches reference's
        // SOCKET_ERROR-only check; partial-but-positive returns are
        // not treated as errors, per reference posture at
        // network.c:1391-1399).
        const int n = lyra::wire::metis_write_frame(0x02, pktBytes + 8);
        if (n < 0) {
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
