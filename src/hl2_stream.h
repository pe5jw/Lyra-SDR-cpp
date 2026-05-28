// Lyra — HPSDR Protocol 1 stream (EP6 receive + EP2 keepalive).
//
// Step 2a scope (shipped): open the stream, RX EP6 datagrams on a
// dedicated OS thread, verify Metis header + USB-frame sync, count
// datagrams + dropouts + framing errors.
//
// Step 2b scope (this commit): add the EP2 keepalive writer on its
// OWN dedicated OS thread.  HL2 gateware watchdog cuts EP6 after
// ~13 sec without host EP2 traffic — Step 2a surfaced this empirically
// on the operator's HL2+ bench (65 520 dg / ~13 sec then stream
// stops cold).  EP2 writer fires at 380 Hz (= 48 kHz audio sample
// rate / 126 audio samples per EP2 datagram) carrying a minimum-
// viable C&C config (192 kHz IQ, nddc=4, MOX off, duplex bit on)
// + zero audio/TX-IQ payload.  Keeps the gateware happy indefinitely.
//
// Locked architectural rule: the wire path runs on dedicated OS
// threads (one for RX, one for TX), each on a std::jthread, native
// WinSock2 socket shared between them (sendto + recvfrom are
// independently thread-safe at the OS level on separate directions
// of one socket).  No Qt event loop on the wire threads.  No GIL.
// No Python.  Anywhere.  Ever.
//
// References (read before coding — Lyra-native implementation,
// nothing copied):
//   * HL2 wiki Protocol.md — register map, EP2/EP6 layout
//   * HPSDR Protocol 1 spec (openHPSDR.org)
//   * CLAUDE.md §3.2 / §3.4 — operator-verified byte layouts
//   * CLAUDE.md §5 — original Python threading model (now C++23)
//   * CLAUDE.md §15.26 — Win32 HIGH_RESOLUTION timer pattern
//
// Wire reference summary:
//
//   Host → radio control (64-byte UDP datagram to radio:1024):
//     bytes [0..1] = 0xEF 0xFE (magic)
//     byte  [2]    = 0x04 (command)
//     byte  [3]    = command byte; 0x01 = start IQ, 0x00 = stop
//     bytes [4..63] = zero padding
//
//   Host → radio EP2 keepalive (1032-byte UDP datagram to radio:1024):
//     8-byte Metis header:
//       bytes [0..1] = 0xEF 0xFE (magic)
//       byte  [2]    = 0x01 (data frame)
//       byte  [3]    = 0x02 (endpoint = EP2 = host → radio)
//       bytes [4..7] = sequence number (BIG-endian uint32)
//     USB frame 1 (512 bytes at offset 8):
//       bytes [0..2] = 0x7F 0x7F 0x7F (sync)
//       byte  [3]    = C0 = frame address << 1 | MOX bit
//       bytes [4..7] = C1..C4 (config registers; frame-0 carries
//                      sample rate, OC pins, nddc, duplex bit)
//       bytes [8..511] = 504 bytes = 63 LRIQ tuples × 8 bytes
//                       (L audio + R audio + TX I + TX Q, all
//                        16-bit signed BE; ZERO during RX-only)
//     USB frame 2 (512 bytes at offset 520): same layout
//
//   Radio → host EP6 receive (1032-byte UDP datagram from radio:1024):
//     same Metis layout, byte [3] = 0x06; each USB frame's 504-byte
//     payload = 19 sample slots × 26 bytes (DDC0-3 I/Q + mic).
//     Parsing lands in Step 2c — Step 2b only verifies integrity.
//
// Frame-0 C1..C4 we send on every keepalive (matches the post-START
// gateware default the operator's HL2+ runs at — no state change):
//   C1 = 0x02 — sample rate bits [1:0] = 10 = 192 kHz per DDC
//   C2 = 0x00 — no open-collector outputs, no 10MHz ref override
//   C3 = 0x00 — no random, no dither, no preamp adjust
//   C4 = 0x1C — nddc=4 (bits [6:3] = 0011 = 0x18) | duplex bit
//               (bit 2 = 0x04) per CLAUDE.md §3.2 main-loop emission

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <stop_token>
#include <vector>

namespace lyra::ipc {

// Platform socket handle, opaque to consumers of this header (we do
// NOT drag winsock2.h through here).  On Windows the platform
// SOCKET type is UINT_PTR which is quintptr.  On POSIX it would be
// `int` — the implementation casts internally.
using SocketHandle = quintptr;
inline constexpr SocketHandle kInvalidSocket = ~SocketHandle{0};

class HL2Stream : public QObject {
    Q_OBJECT
    // RX direction (Step 2a)
    Q_PROPERTY(bool    running              READ isRunning            NOTIFY runningChanged)
    Q_PROPERTY(QString targetIp             READ targetIp             NOTIFY runningChanged)
    Q_PROPERTY(double  datagramsPerSec      READ datagramsPerSec      NOTIFY statsChanged)
    Q_PROPERTY(qint64  totalDatagrams       READ totalDatagrams       NOTIFY statsChanged)
    Q_PROPERTY(qint64  seqErrors            READ seqErrors            NOTIFY statsChanged)
    Q_PROPERTY(qint64  framingErrors        READ framingErrors        NOTIFY statsChanged)
    // TX direction (Step 2b — EP2 keepalive)
    Q_PROPERTY(double  txDatagramsPerSec    READ txDatagramsPerSec    NOTIFY statsChanged)
    Q_PROPERTY(qint64  txTotalDatagrams     READ txTotalDatagrams     NOTIFY statsChanged)
    Q_PROPERTY(qint64  txSendErrors         READ txSendErrors         NOTIFY statsChanged)
    // RX1 signal level (Step 2c — first real radio reception in C++)
    Q_PROPERTY(double  rx1DbFs              READ rx1DbFs              NOTIFY statsChanged)
    // RX1 (DDC0) receive frequency, Hz — tuning from C++ (Step 4)
    Q_PROPERTY(quint32 rx1FreqHz            READ rx1FreqHz            NOTIFY rx1FreqChanged)
    // RX1 LNA gain (AD9866 PGA), dB.  Range −12…+48; sent on the C&C
    // 0x14 register (C4 = 0x40 | ((dB+12)&0x3F)), rotated into the EP2
    // C&C cadence at ~20 Hz.  Persisted.
    Q_PROPERTY(int lnaGainDb READ lnaGainDb WRITE setLnaGainDb NOTIFY lnaGainChanged)
    // Auto-LNA — overload-triggered gain protection (Thetis-style).
    // When on, sustained ADC overload backs the LNA off 3 dB; when the
    // band is clear it creeps gain back up 1 dB per hold interval,
    // riding the overload edge.  Roams the full −12…+48 range
    // independently of the manual set point; auto adjustments are NOT
    // persisted (the manual slider value remains the stored set point,
    // restored when Auto is turned off).  Fresh-install default ON
    // (matches the operator's Thetis chkAutoStepAttenuator=True).
    Q_PROPERTY(bool autoLna       READ autoLna        WRITE setAutoLna        NOTIFY autoLnaChanged)
    Q_PROPERTY(bool autoLnaUndo   READ autoLnaUndo    WRITE setAutoLnaUndo    NOTIFY autoLnaChanged)
    Q_PROPERTY(int  autoLnaHoldSec READ autoLnaHoldSec WRITE setAutoLnaHoldSec NOTIFY autoLnaChanged)
    // Live ADC-overload indicator — true when the HL2 gateware reported
    // ADC clipping in the last ~400 ms window (EP6 status C1 bit 0).
    Q_PROPERTY(bool adcOverload   READ adcOverload    NOTIFY adcOverloadChanged)
    // External filter board (N2ADR): when enabled, the per-band OC
    // pattern is driven on frame-0 C2 so the board's RX band-pass +
    // 3 MHz HPF relays follow the band (front-end protection).  ocBits
    // is the live 7-bit J16 pattern (readout).
    Q_PROPERTY(bool filterBoardEnabled READ filterBoardEnabled
               WRITE setFilterBoardEnabled NOTIFY filterBoardChanged)
    Q_PROPERTY(int  ocBits             READ ocBits NOTIFY ocBitsChanged)
    // TX-0a — HL2 telemetry decoded from the EP6 status rotation
    // (C0&0xF8 address field).  RF-SAFE: pure read of the radio→host
    // stream, zero effect on what we transmit.  Temp + supply are
    // RX-time-meaningful; PA current/volts + fwd/rev power matter
    // during TX.  ⚠ The AINx→slot map + the watt/amp conversions are
    // gateware-rev-specific on HL2+ — these follow the documented HL2
    // rotation and MUST be bench-verified on the operator's unit
    // (set env LYRA_TELEM_DEBUG=1 to log the raw (addr,C1..C4)
    // rotation).  NaN getter = no telemetry for that slot yet.
    Q_PROPERTY(double hl2TempC   READ hl2TempC   NOTIFY statsChanged)
    Q_PROPERTY(double hl2SupplyV READ hl2SupplyV NOTIFY statsChanged)
    Q_PROPERTY(double paCurrentA READ paCurrentA NOTIFY statsChanged)
    Q_PROPERTY(double fwdPowerW  READ fwdPowerW  NOTIFY statsChanged)
    Q_PROPERTY(double revPowerW  READ revPowerW  NOTIFY statsChanged)
    // TX-0c-fsm — wire-level MOX state (true once the keydown TR-delay
    // sequence completes and the C0 bit-0 has settled on the wire;
    // cleared at the END of the keyup sequence).  This is the
    // operator-visible "on the air" truth — drive the red TX indicator
    // off this, NOT off the toggle button's checked state, so the LED
    // tracks the actual radio state through the TR-delay window.
    Q_PROPERTY(bool moxActive READ moxActive NOTIFY moxActiveChanged)
    // TX-0c-pa-debug — host-side TX safety timeout.  Auto-clears MOX
    // (via requestMox(false)) if the radio stays keyed continuously
    // past txTimeoutSec seconds.  Persisted (tx/timeoutSeconds in
    // QSettings).  Operator can bypass via txTimeoutBypass — typically
    // for long-form AM ragchews / slow CW beacons where the ~10-min
    // default is too short.  Default is 600 s (10 min); range 60..1200
    // (1..20 min) enforced by the setter.
    Q_PROPERTY(int  txTimeoutSec    READ txTimeoutSec
               WRITE setTxTimeoutSec NOTIFY txTimeoutSecChanged)
    Q_PROPERTY(bool txTimeoutBypass READ txTimeoutBypass
               WRITE setTxTimeoutBypass NOTIFY txTimeoutBypassChanged)

public:
    explicit HL2Stream(QObject *parent = nullptr);
    ~HL2Stream() override;

    bool    isRunning()         const { return running_.load(std::memory_order_acquire); }
    QString targetIp()          const { return targetIp_; }
    double  datagramsPerSec()   const { return dgPerSec_; }
    qint64  totalDatagrams()    const { return totalDg_.load(std::memory_order_relaxed); }
    // seqErrors: count of EP6 datagrams whose sequence number was
    // anything other than (previous+1).  Incremented by 1 per event
    // — NOT summed by the gap magnitude — so a one-shot sequence
    // counter reset (which the HL2 gateware can do transiently)
    // shows up as "1 seq err" instead of nonsense.  Matches the
    // reference-implementation diagnostic posture; see CLAUDE.md
    // §15.x / FEATURES.md for the rationale.
    qint64  seqErrors()         const { return seqErrors_.load(std::memory_order_relaxed); }
    qint64  framingErrors()     const { return framingErrors_.load(std::memory_order_relaxed); }
    double  txDatagramsPerSec() const { return txDgPerSec_; }
    qint64  txTotalDatagrams()  const { return txTotalDg_.load(std::memory_order_relaxed); }
    qint64  txSendErrors()      const { return txSendErrors_.load(std::memory_order_relaxed); }
    // rx1DbFs — RMS magnitude of the RX1 baseband IQ stream in
    // dBFS (full scale = 1.0 magnitude after normalizing the
    // gateware's 24-bit signed samples).  Updated by the RX
    // worker thread every 50 ms (9600 samples at 192 kHz);
    // initial sentinel -200.0 means "no samples yet."
    double  rx1DbFs()           const { return rx1DbFs_.load(std::memory_order_relaxed); }
    quint32 rx1FreqHz()         const { return rx1FreqHz_.load(std::memory_order_relaxed); }
    int     lnaGainDb()         const { return lnaGainDb_.load(std::memory_order_relaxed); }
    bool    autoLna()           const { return autoLnaEnabled_; }
    bool    autoLnaUndo()       const { return autoLnaUndo_; }
    int     autoLnaHoldSec()    const { return autoLnaHoldSec_; }
    bool    adcOverload()       const { return adcOverload_; }
    // Operator gain bounds = the AD9866 LNA hardware range in full-range
    // mode (HL2 wiki: code 0…60 → −12…+48 dB; frame-11 C4 bit 6 = the
    // full-range enable, which Lyra sets).  +48 is the true ceiling
    // (old Lyra software-capped at +31 on an "above this is IMD" call —
    // now the operator's choice, with Auto-LNA managing the edge).
    static constexpr int kLnaMinDb = -12;
    static constexpr int kLnaMaxDb =  48;
    // TX safety timeout operator-facing range (seconds) — exposed so the
    // Settings UI builds its SpinBox against the canonical bounds.
    static constexpr int kTxTimeoutDefaultSec = 600;   // 10 min default
    static constexpr int kTxTimeoutMinSec     = 60;    //  1 min floor
    static constexpr int kTxTimeoutMaxSec     = 1200;  // 20 min ceiling
    bool    filterBoardEnabled() const { return filterBoardEnabled_; }
    int     ocBits()             const { return ocPattern_; }
    // TX-0a telemetry getters — convert the raw 12-bit EP6 ADC slots
    // (written by the RX worker) on the main thread.  Sentinel raw < 0
    // → NaN ("no telemetry yet").  Formulas per the documented HL2
    // rotation; bench-verify (see the Q_PROPERTY note).
    double  hl2TempC()   const;
    double  hl2SupplyV() const;
    double  paCurrentA() const;
    double  fwdPowerW()  const;
    double  revPowerW()  const;
    // TX-0c-fsm — true while the radio is wire-level keyed (post-keydown
    // settle, pre-keyup-clear).  Read by the UI red-on-air indicator.
    bool    moxActive()  const { return moxActive_; }
    // TX-0c-pa-debug — operator-tunable safety timeout (seconds) +
    // bypass.  Setters clamp + persist + emit changes; the FSM-side
    // keydown/keyup hooks arm/cancel the QTimer.
    int     txTimeoutSec()    const { return txTimeoutSec_; }
    bool    txTimeoutBypass() const { return txTimeoutBypass_; }

    // Step 3d: register a sink for DDC0 baseband IQ.  Called ONCE per
    // EP6 datagram from the RX worker thread with interleaved
    // (I,Q,…) doubles in [-1,1) (38 frames/datagram at 192 kHz nddc=4).
    // The sink runs SYNCHRONOUSLY on the RX worker thread — it is a
    // plain std::function, NOT a Qt signal, so there is no cross-thread
    // queueing on the hot path (the DSP engine consumes it inline,
    // mirroring how Thetis's cmaster pump calls fexchange0).  Must be
    // set before open() spawns the worker; not changed while running.
    void setIqSink(std::function<void(const double *, int)> sink) {
        iqSink_ = std::move(sink);
    }

    // Step 5: RX audio out via the HL2 onboard codec (AK4951).  The DSP
    // engine pushes decoded 48 kHz stereo int16 here (from the RX worker
    // thread, inline after fexchange0); the EP2 writer thread drains 126
    // frames per datagram into the LRIQ tuple L/R fields so it plays out
    // the radio's headphone jack — old Lyra's default HL2 audio path.
    // Thread-safe (brief mutex hold, a few hundred int16 per call).
    void pushAudio(const qint16 *lr, int nframes);
    // Enable/disable EP2 audio injection.  When off, the EP2 frames
    // carry silence (zero L/R) — the keepalive still flows.
    void setInjectAudio(bool on) {
        injectAudio_.store(on, std::memory_order_relaxed);
    }

public slots:
    // Open the stream to the radio at `ip`.  Creates one native UDP
    // socket, spawns the EP6 RX std::jthread + the EP2 TX std::jthread
    // (both share the socket), the TX thread sends START on entry.
    // Safe to call when already running (logs + ignores).
    void open(const QString &ip);

    // Request stop on both worker threads, join them, send STOP
    // from the main thread (best-effort), close the socket.  Safe
    // to call when already stopped (no-op).
    void close();

    // Set the DDC0 (RX1) receive frequency in Hz.  Stored in an atomic
    // the EP2 writer reads each send; takes effect on the next
    // keepalive (~2.6 ms) via the addr-2 C&C frame.  The duplex bit
    // (frame-0 C4=0x1C, sent every datagram) is what lets the gateware
    // apply post-priming RX-freq updates (CLAUDE.md §3.2).  Thread-safe.
    void setRx1FreqHz(quint32 hz);

    // Set the RX1 LNA gain (−12…+48 dB).  Stored in an atomic the EP2
    // writer encodes into the 0x14 C&C register; persisted.  Thread-safe.
    void setLnaGainDb(int db);

    // Auto-LNA controls (main thread).  setAutoLna(false) restores the
    // operator's persisted manual gain; setAutoLna(true) hands the LNA
    // to the overload-protection loop.  Undo + hold time are persisted.
    void setAutoLna(bool on);
    void setAutoLnaUndo(bool on);
    void setAutoLnaHoldSec(int sec);

    // Set the IQ sample rate (96 / 192 / 384 kHz).  Updates the frame-0
    // C1 speed bits the EP2 writer sends each datagram; takes effect on
    // the next send.  The DSP-side rate switch (WDSP channel reopen) is
    // driven separately via WdspEngine::setSampleRate.  Thread-safe
    // (atomic).  Ignores 48 k (EP2 cadence, like old Lyra).
    void setSampleRate(int hz);

    // Enable/disable the external N2ADR filter board.  When on, the
    // per-band OC pattern is driven on frame-0 C2 and re-applied on every
    // band change; when off, C2 OC pins are cleared (0).  Persisted to
    // QSettings (hw/filterBoard).  Thread-safe (atomic C2; readout on the
    // main thread).
    void setFilterBoardEnabled(bool on);

    // ---- TX-state C&C registers (TX-0b foundation) -----------------
    // Operator / PTT-FSM RF-transmit state.  Each setter clamps + stores
    // an atomic holding the HL2+ byte encoding for its TX C&C frame,
    // verified against Thetis WriteMainLoop_HL2 (networkproto1.c) +
    // the ak4951v4 gateware decode (control.v) — NOT prior Lyra TX code.
    // These are the SINGLE source of truth for the byte layout; a later
    // TX phase wires them into the EP2 C&C round-robin under MOX gating.
    //
    // INERT until then: the datagram emission loop reads NONE of them,
    // so at MOX=0 / PA-off the wire is byte-identical to RX.  All
    // thread-safe (lock-free atomics on x86_64).
    //
    //   setMox(on)            RF transmit active -> C0 bit 0
    //                         (networkproto1.c:896 C0=XmitBit)
    //   setTxFreqHz(hz)       TX NCO -> C0 0x02/0x08/0x0a (BE, all three
    //                         carry tx[0].frequency: networkproto1.c
    //                         cases 1/5/6)
    //   setTxDriveLevel(0..255)  drive DAC; C0 0x12 C1 (networkproto1.c:
    //                            1078; gateware uses top 4 bits -> 16 steps)
    //   setTxStepAttnDb(0..31)   C0 0x1C C3 = (31 - db) & 0x1F  (HL2's
    //                            inverted range: networkproto1.c:1019 +
    //                            console.cs:10658 SetTxAttenData(31-x))
    //   setPaEnabled(on)      onboard PA -> C0 0x12 C2 bit 3 (0x08),
    //                         active-high (networkproto1.c:1079 ApolloTuner
    //                         =0x8 netInterface.c:581; control.v:213
    //                         pa_enable<=cmd_data[19]).  NB: C2 bit 7
    //                         (0x80, VNA) must stay clear or the PA won't
    //                         key (control.v:359); C2 bit 2 (0x04) is a
    //                         SEPARATE full-duplex/T-R control, NOT a
    //                         PA-off flag.
    void setMox(bool on);
    void setTxFreqHz(quint32 hz);
    void setTxDriveLevel(int level);
    void setTxStepAttnDb(int db);
    void setPaEnabled(bool on);

    // ---- TX-0c-fsm: MOX/PTT sequencer (single funnel) ----------------
    // Operator/CAT/PTT/TUN intent gets funneled here.  Internally drives
    // a TR-sequenced state machine that times the wire MOX-bit flip
    // around the ATT-on-TX RX-protect raise/restore.  All delays via
    // QTimer::singleShot on this QObject's thread (no sleeps); the wire
    // worker thread reads mox_ / txStepAttnDb_ atomics as before.
    //
    //  Keydown (RX → MOX_TX):
    //      save txStepAttnDb_ → force ATT-on-TX (31)
    //      → wait kMoxDelayMs (15 ms)
    //      → mox_ = true       (C0 bit 0 on wire next datagram)
    //      → wait kRfDelayMs   (50 ms; rf settle, hot-switch-safe)
    //      → emit moxActiveChanged(true)
    //
    //  Keyup   (MOX_TX → RX):
    //      wait kSpaceMoxDelayMs (13 ms; re-key window — if requestMox
    //          fires true during this, collapse: stay TX, no wire flip)
    //      → mox_ = false
    //      → wait kPttOutDelayMs (5 ms)
    //      → restore txStepAttnDb_
    //      → emit moxActiveChanged(false)
    //
    // Bench discipline: at TX-0c-fsm the MOX bit lands on the wire but
    // paOn_ stays false, so the HL2's T/R relay clicks audibly but the
    // PA bias never enables — STILL ZERO RF.  TX-0c-pa-debug adds the
    // operator-gated PA enable.
    //
    // Re-entrancy: requestMox() is the SINGLE funnel; calling it during
    // a transition just updates the intent and the sequencer reads it
    // at each step (Thetis-style re-key collapse, cancel mid-keydown,
    // etc., all work).
    void requestMox(bool on);

    // ---- TX-0c-pa-debug: host-side safety timeout ----------------
    // setTxTimeoutSec clamps to kTxTimeoutMinSec..kTxTimeoutMaxSec
    // (60..1200 s; 1..20 min UI), persists to QSettings, and emits
    // txTimeoutSecChanged.  If the radio is currently keyed the
    // timer is re-armed with the new full duration (operator intent
    // = "I want N more seconds from now").
    //
    // setTxTimeoutBypass false → true cancels any active safety
    // timer (operator turned safety OFF mid-key); true → false re-
    // arms with full duration when keyed (operator turned safety
    // back ON, applies immediately).
    void setTxTimeoutSec(int sec);
    void setTxTimeoutBypass(bool on);

signals:
    void runningChanged();
    void statsChanged();
    void rx1FreqChanged();
    void lnaGainChanged();
    // Emitted ONLY by the manual setLnaGainDb() path (operator slider /
    // wheel / per-band restore) — NOT by Auto-LNA's roaming.  Lets
    // BandMemory save the operator's per-band manual set point without
    // capturing an auto-roamed value.
    void lnaSetByOperator(int db);
    void autoLnaChanged();
    void adcOverloadChanged();
    void filterBoardChanged(bool on);
    void ocBitsChanged(int pattern);
    void logLine(QString line);
    // TX-0c-fsm — fires when the wire MOX state changes after a TR-delay
    // transition completes (true at end of keydown rf_delay; false at end
    // of keyup ptt_out_delay).  Does NOT fire on mid-transition states.
    void moxActiveChanged(bool on);
    // TX-0c-pa-debug — fires on operator-set changes to the safety
    // timeout + bypass.  Settings UI binds via Q_PROPERTY; persistence
    // lives in the setter.
    void txTimeoutSecChanged(int sec);
    void txTimeoutBypassChanged(bool on);
    // Fires once when the safety timeout actually expires and the FSM
    // auto-clears MOX.  Useful for a status-bar toast / log highlight;
    // the actual MOX-off is driven through requestMox(false) regardless.
    void txTimeoutFired();

private slots:
    void onStatsTick();
    void onAutoLnaTick();
    void onFatalError(QString reason);

private:
    void rxWorkerLoop(std::stop_token stop, SocketHandle sock);
    void txWorkerLoop(std::stop_token stop, SocketHandle sock,
                      QString ip);
    // TX-0c-fsm — sequencer steps (all run on this QObject's thread via
    // QTimer::singleShot, NOT on the wire workers).  Each step re-reads
    // requestedMox_ so an operator change mid-transition (cancel during
    // keydown, re-key during keyup space window) takes effect cleanly.
    void fsmAdvance();        // entry — picks keydown vs keyup vs idle
    void fsmKeydownPostMox(); // after kMoxDelayMs — set wire MOX bit
    void fsmKeydownSettled(); // after kRfDelayMs  — emit moxActiveChanged(true)
    void fsmKeyupPostSpace(); // after kSpaceMoxDelayMs — clear wire MOX bit
                              //   (or collapse-stay-TX if re-keyed)
    void fsmKeyupSettled();   // after kPttOutDelayMs  — restore step-att,
                              //   emit moxActiveChanged(false)
    // TX-0c-pa-debug — arm the safety timer (called at moxActive=true
    // edge if bypass is off) / cancel it (at moxActive=false edge).
    // No-ops if bypass is on.
    void armTxSafetyTimer();
    void cancelTxSafetyTimer();
    // QTimer expiry slot — driven by tx_safety_timer_; routes through
    // requestMox(false) so the standard keyup TR-delay chain runs
    // cleanly (no shortcut on the wire bit).
    void onTxSafetyTimeout();
    // TX-0c emit: compose one C&C frame (C0..C4) for cycle slot `idx`
    // (0..18), reading the live MOX/freq/drive/step-att/PA state.  Pure
    // function — single-thread owned by txWorker_ (no locks needed; reads
    // are lock-free atomics).  `mox` is snapshotted once per datagram by
    // the caller so both USB frames of one datagram carry coherent MOX.
    //
    // Slot map (19-slot HL2 round-robin, matches Thetis WriteMainLoop_HL2):
    //   0  0x00 general   C1=rate C2=OC C3=0 C4=0x1C (duplex|nddc=4)
    //   1  0x02 TX freq   BE u32 (cases 1/5/6 all carry tx[0].frequency)
    //   2  0x04 RX1 freq  BE u32 (DDC0)
    //   3  0x06 RX2 freq  BE u32 (DDC1; RX1 freq until RX2 lands)
    //   4  0x1C TX step-att  C3 = (31 - txStepAttnDb_) & 0x1F
    //   5  0x08 DDC2 freq = TX freq (PS feedback when keyed)
    //   6  0x0a DDC3 freq = TX freq
    //   7  0x0c DDC4 (unused on HL2; emit benign zero data)
    //   8  0x0e DDC5 (unused)
    //   9  0x10 DDC6 (unused)
    //   10 0x12 drive/PA  C1=drive C2=0x40|(paOn?0x08:0) (active-high)
    //   11 0x14 LNA + MOX-gated step-att
    //                    C4 = mox ? ((31-txStepAttnDb_)&0x3F)|0x40
    //                              : ((lnaGainDb_+12)&0x3F)|0x40
    //   12 0x16 placeholder (CW state on HL2 / ADC-step-att on Hermes-II)
    //   13 0x18 placeholder
    //   14 0x1a placeholder (PWM/EER)
    //   15 0x1e placeholder (CW key/EER pulse width)
    //   16 0x20 placeholder
    //   17 0x22 placeholder
    //   18 0x24 placeholder
    // All "placeholder" slots emit C1..C4=0 with the correct addr in C0
    // so the gateware sees a valid frame in every slot (matches Thetis's
    // unconditional 19-slot emission — no slot skipping).
    void composeCC(int idx, bool mox, std::uint8_t out[5]) const;
    // Recompute the OC pattern (frame-0 C2) from the current band +
    // filter-board-enabled state.  Main thread only.
    void updateOcPattern();
    // Apply an LNA gain to the live wire value WITHOUT persisting it —
    // used by Auto-LNA so its transient adjustments never overwrite the
    // operator's manual set point in QSettings.  Emits lnaGainChanged
    // so the slider / dB readout / S-meter compensation all track.
    void applyLnaGainNoPersist(int db);

    SocketHandle         socket_ = kInvalidSocket;
    std::jthread         rxWorker_;
    std::jthread         txWorker_;
    std::atomic<bool>    running_{false};
    std::atomic<qint64>  totalDg_{0};
    std::atomic<qint64>  seqErrors_{0};
    std::atomic<qint64>  framingErrors_{0};
    std::atomic<qint64>  windowDg_{0};
    std::atomic<qint64>  txTotalDg_{0};
    std::atomic<qint64>  txWindowDg_{0};
    std::atomic<qint64>  txSendErrors_{0};
    std::atomic<quint32> txSeq_{0};
    // Step 2c: RX1 dBFS, written by the RX worker thread.  Sentinel
    // -200.0 = "no samples yet" / pre-first-window.  std::atomic<double>
    // is lock-free on x86_64 so the main-thread read in the Q_PROPERTY
    // getter doesn't take a lock.
    std::atomic<double>  rx1DbFs_{-200.0};
    // Step 4: DDC0 (RX1) receive frequency, Hz.  Read by the EP2 writer
    // each send.  Default 7.074 MHz (40m FT8) so first launch lands on
    // a known-active spot.  std::atomic<quint32> is lock-free on x86_64.
    std::atomic<quint32> rx1FreqHz_{7074000};
    // RX1 LNA gain (dB, −12…+48).  Read by the EP2 writer each cadence
    // tick.  Default +31 dB — old Lyra's practical-max set point: a
    // strong starting gain that's still below the +48 hardware ceiling,
    // with Auto-LNA (default on) backing it off on overload.  Overridden
    // from QSettings in the ctor once the operator sets it.
    std::atomic<int>     lnaGainDb_{31};
    // EP2 C&C round-robin slot index — one slot per USB frame, 0..18,
    // covering the full HL2 HPSDR-P1 C&C surface (general, TX/RX VFOs,
    // TX step-att, DDC2/3 TX-freq mirror, drive+PA, LNA+MOX-gated
    // step-att, CW/EER/BPF2/tx-latency/reset).  ~40 Hz per slot at
    // ~760 USB frames/sec.  Owned by the EP2 writer thread (txWorker_)
    // = single-thread, no atomic needed.
    int                  ccIdx_{0};
    // ADC-overload telemetry + Auto-LNA (Thetis-style, gain-sense).
    // adcOverloadNow_ holds the ADC0 overload bit from the MOST RECENT
    // EP6 address-0 status frame (direct overwrite — NOT a window-OR
    // latch).  The 400 ms tick samples it INSTANTANEOUSLY, matching the
    // reference HL2 read loop's getAndResetADC_Overload poll: a window-
    // OR over the ~1000 address-0 frames per 400 ms over-reported on a
    // strong front end (one micro-clip pinned overload every cycle →
    // gain driven to the floor).  adcOverload_ / overloadLevel_ /
    // autoLna*_ are main-thread state owned by onAutoLnaTick().
    std::atomic<bool>    adcOverloadNow_{false};
    // TX-0a — raw 12-bit EP6 telemetry slots, written by the RX worker
    // (direct overwrite, most-recent wins), read by the main-thread
    // getters.  −1 = no data yet.  std::atomic<int> is lock-free on
    // x86_64 so the getters take no lock on the hot path.
    std::atomic<int>     telTempRaw_{-1};      // 0x08 C1:C2 (AIN5)
    std::atomic<int>     telFwdRaw_{-1};       // 0x08 C3:C4 (AIN1)
    std::atomic<int>     telRevRaw_{-1};       // 0x10 C1:C2 (AIN2)
    std::atomic<int>     telPaCurRaw_{-1};     // 0x10 C3:C4 (PA bias current)
    std::atomic<int>     telSupplyRaw_{-1};    // 0x00 C1:C2 >>4 (supply V)
    // Full-rotation probe: addr-0's data pair (C1:C2 / C3:C4).  Addr 0
    // C1 bit 0 is the ADC-overload flag; the rest of the pair is unused
    // by the generic map but is the prime suspect for supply volts on
    // the ak4951v4 gateware (where 0x18 reads 0).  Captured raw for the
    // LYRA_TELEM_DEBUG dump so the real slot map can be derived from the
    // operator's hardware rather than assumed.
    std::atomic<int>     telA0c12Raw_{-1};     // 0x00 C1:C2
    std::atomic<int>     telA0c34Raw_{-1};     // 0x00 C3:C4
    bool                 adcOverload_    = false;
    int                  overloadLevel_  = 0;      // 0..5 confirm accumulator
    bool                 autoLnaEnabled_ = false;
    bool                 autoLnaUndo_    = true;
    int                  autoLnaHoldSec_ = 4;      // "Undo N s" creep interval
    QTimer               autoLnaTimer_;
    QElapsedTimer        holdClock_;               // since last auto gain change
    bool                 recovering_     = false;  // creeping gain back up
    // External filter board (N2ADR).  ocC2_ is the frame-0 C2 byte the
    // EP2 writer reads each send (= 7-bit OC pattern << 1; C2[0] stays 0).
    // filterBoardEnabled_ / ocPattern_ are main-thread state (the
    // Q_PROPERTY getters + Settings UI); ocC2_ is the atomic wire value.
    std::atomic<std::uint8_t> ocC2_{0};
    // Frame-0 C1 IQ speed bits [1:0]: 1=96k, 2=192k(default), 3=384k.
    // Read by the EP2 writer each send.
    std::atomic<std::uint8_t> sampleRateBits_{0x02};
    // ---- TX-0b: RF-transmit state (foundation; NOT yet emitted) ----
    // Encodings per Thetis WriteMainLoop_HL2 + ak4951v4 gateware decode
    // (see the setter doc block for cites).  INERT — read by no emission path yet;
    // at MOX=0/PA-off the datagram is byte-identical to RX.  A later TX
    // phase wires these into the C&C round-robin under MOX gating.
    std::atomic<bool>    mox_{false};          // C0 bit 0 (RF transmit)
    std::atomic<quint32> txFreqHz_{7074000};   // TX NCO -> 0x02/0x08/0x0a
    std::atomic<int>     txDriveLevel_{0};      // 0..255; 0x12 C1 (16 steps)
    std::atomic<int>     txStepAttnDb_{0};      // 0..31 dB; 0x1C C3 (31-db)
    std::atomic<bool>    paOn_{false};          // 0x12 C2 bit 3 (active-high)
    // ---- TX-0c-fsm: MOX/PTT sequencer state (single-thread, this thread) -
    // Operator/CAT intent — last requested MOX state.  Sequencer reads
    // it at every timer step so an operator change mid-transition takes
    // effect on the next scheduled boundary (cancel keydown → exit clean;
    // re-key during keyup space window → collapse-stay-TX).
    bool                 requestedMox_ = false;
    // True while any TR-delay timer chain is in flight; gates re-entry
    // into the FSM so a single intent edge schedules exactly one chain.
    bool                 fsmRunning_   = false;
    // Saved operator step-att before ATT-on-TX raise on keydown, so the
    // keyup restore lands the operator's pre-key set point (today always
    // 0 on RX, but forward-compat for a future operator-tunable RX
    // step-att).  Touched only by the FSM on this thread.
    int                  savedTxStepAttn_ = 0;
    // Wire-level MOX truth — true once the post-keydown rf_delay has
    // settled, cleared at the end of the keyup ptt_out_delay.  Drives
    // the UI red-on-air indicator (NOT the toggle button's checked
    // state).  Touched only on this thread.
    bool                 moxActive_    = false;
    // TX-0c-pa-debug — host-side safety timeout state.  Both ints/
    // bools are single-thread (this QObject's thread) — set by the
    // operator via the Settings UI, read by the FSM keydown hook to
    // decide whether to arm tx_safety_timer_ on each keydown edge.
    int                  txTimeoutSec_     = 600;   // 10 min default
    bool                 txTimeoutBypass_  = false;
    // Single-shot timer driving the auto-MOX-off on safety expiry.
    // Owned by this QObject (parent = this), runs on this thread.
    QTimer              *txSafetyTimer_    = nullptr;
    bool                 filterBoardEnabled_ = false;
    int                  ocPattern_ = 0;   // live 7-bit J16 pattern
    // Step 3d: DDC0 IQ sink (DSP engine).  Set once before open();
    // read on the RX worker thread.  Default-empty = no DSP wired.
    std::function<void(const double *, int)> iqSink_;
    // Step 5: EP2 RX-audio injection ring (interleaved stereo int16).
    // Producer = RX worker (pushAudio, via the DSP engine); consumer =
    // EP2 TX writer thread (drains 126 frames/datagram).  audioMtx_
    // guards the ring indices (hold time = a short memcpy).
    std::mutex            audioMtx_;
    std::condition_variable audioCv_;         // EP2 writer waits on this
    std::vector<qint16>   audioBuf_;          // 2*audioCap_ samples (L,R)
    int                   audioCap_   = 0;    // capacity in frames
    int                   audioRd_    = 0;
    int                   audioWr_    = 0;
    int                   audioCount_ = 0;    // frames currently buffered
    std::atomic<bool>     injectAudio_{false};
    double               dgPerSec_   = 0.0;
    double               txDgPerSec_ = 0.0;
    QString              targetIp_;
    QTimer               statsTimer_;
    // Measures the ACTUAL interval between stats ticks so dg/s is
    // correct even when the 5 Hz timer fires late (e.g. main-thread
    // paint load) — dividing by a fixed period inflated the reading
    // and made the WIRE-OK band flap.
    QElapsedTimer        statsClock_;

    static constexpr quint16 kRadioPort    = 1024;
    static constexpr int     kMetisDgSize  = 1032;   // 8 hdr + 2*512 USB
    static constexpr int     kStatPeriodMs = 200;    // 5 Hz UI updates
    // EP2 keepalive cadence: 48000 audio samples/sec ÷ 126 samples
    // per datagram (63 LRIQ tuples × 2 USB frames) = 380.95 dg/sec
    // → 2.6316 ms period.  Same cadence Thetis/pihpsdr/Quisk fire.
    static constexpr int     kEp2RateHz    = 380;
    // Auto-LNA overload-poll cadence (matches the reference ~400 ms loop).
    static constexpr int     kAutoLnaPeriodMs = 400;
    // Once recovery begins (after the hold time confirms the band is
    // clear), creep gain back up at this brisker fixed cadence so the
    // operator isn't left deaf for minutes — pull-DOWNs still honor the
    // operator's hold-time setting; only the pull-UP is sped up.
    static constexpr int     kAutoLnaRecoverMs = 1000;
    // ---- TX-0c-fsm: TR-sequencing delays (ms) -----------------------
    // Values pulled from the operator's Thetis DB export
    // (Default_5_16_2026; HL2+/AK4951 bench-validated set):
    //   udMoxDelay         = 15  → kMoxDelayMs        (RX-protect → MOX)
    //   udRFDelay          = 50  → kRfDelayMs         (MOX → RF settle;
    //                                                  hot-switch-safe
    //                                                  for ext linear)
    //   udSpaceMoxDelay    = 13  → kSpaceMoxDelayMs   (keyup re-key win)
    //   udGenPTTOutDelay   = 5   → kPttOutDelayMs     (MOX-clear → done)
    // (udPTTHang = 10 ms / udHermesStepAttenuatorDelay = 100 ms /
    //  udPSMoxDelay = 0.2 s land later — CW hang, RX att settle, PS.)
    // Per CLAUDE.md §6.7 these eventually live in a per-radio
    // capabilities struct; HL2+ values here are the v0.2 starting set.
    static constexpr int     kMoxDelayMs      = 15;
    static constexpr int     kRfDelayMs       = 50;
    static constexpr int     kSpaceMoxDelayMs = 13;
    static constexpr int     kPttOutDelayMs   = 5;
    // ATT-on-TX value (matches operator's Thetis udATTOnTX=31): forces
    // the AD9866 step-att to its 31-dB code on TX, which the encoder
    // turns into wire (31-31)&0x3F | 0x40 = 0x40 = min-LNA on the
    // FAST_LNA arbiter = max RX-ADC protection during TX coupling.
    static constexpr int     kAttOnTxDb       = 31;
    // TX safety timeout range — see public section above (canonical
    // bounds exposed for the Settings UI).
};

} // namespace lyra::ipc
