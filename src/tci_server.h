// Lyra — TCI server (Transceiver Control Interface, public spec
// v1.9 / v2.0).  Clean-room implementation against the published
// spec (D:\sdrprojects\TCI Protocol.pdf) and the command surface
// the working reference exposes — no external code is used.
//
// Lets logging / cluster software (SDRLogger+, Log4OM, WSJT-X, …)
// control and read Lyra over a WebSocket.  Lyra is RX-only today, so
// TX commands (trx/tune/drive/cw_*/mon/rit/xit/split) are acknowledged
// inactive.  DSP toggles Lyra doesn't yet expose a control for
// (agc/squelch/nb/nr/anf/…) are accepted and round-tripped via a soft
// store so client panels stay in sync; they get wired to the real DSP
// when the DSP+Audio control panel lands.
//
// Channel convention: channel 0 = RX1 (the only receiver today);
// channel 1 (RX2) is parsed but ignored until RX2 exists, so we
// advertise channel_count:1.
//
// Everything runs on the Qt main thread (QWebSocketServer is
// signal/slot driven); radio change-signals drive the broadcasts.

#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

#include <vector>

class QWebSocketServer;
class QWebSocket;
class QTimer;

namespace lyra::ipc { class HL2Stream; }
namespace lyra::dsp { class WdspEngine; class TciMicSource; }

namespace lyra::ui {

class Prefs;
class SpotStore;

class TciServer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
public:
    TciServer(Prefs *prefs, lyra::ipc::HL2Stream *stream,
              lyra::dsp::WdspEngine *engine, SpotStore *spots,
              QObject *parent = nullptr);
    ~TciServer() override;

    bool running() const;
    int  clientCount() const { return clients_.size(); }

    // --- operator settings (persisted to QSettings "tci/...") ---
    bool    enabled() const   { return enabled_; }
    int     port() const      { return port_; }
    QString bindHost() const  { return bindHost_; }
    int     rateLimitMs() const { return rateLimitMs_; }
    bool    sendInitialState() const { return sendInitialState_; }
    bool    emulateExpertSdr3() const { return emulateExpertSdr3_; }
    bool    emulateSunSdr2() const    { return emulateSunSdr2_; }
    bool    cwluBecomesCw() const     { return cwluBecomesCw_; }

    void setPort(int p);
    void setBindHost(const QString &h);
    void setRateLimitMs(int ms);
    void setSendInitialState(bool on);
    void setEmulateExpertSdr3(bool on);
    void setEmulateSunSdr2(bool on);
    void setCwluBecomesCw(bool on);
    // Apply the enable toggle: start (or restart) / stop + persist.
    void setEnabled(bool on);

    bool start();
    void stop();

    // Task #33: register the inbound TCI TX_AUDIO_STREAM sink.  May
    // be called late (after WDSP loads + TxDspWorker constructs) —
    // the binary handler tolerates a null sink (frames silently
    // dropped until set, plus a single rate-limited diagnostic line
    // so a misconfigured launch doesn't go silent forever).  Pass
    // {nullptr} to clear (e.g. on TciMicSource teardown).
    void setTciMicSource(lyra::dsp::TciMicSource *src) {
        tciMicSource_ = src;
    }

signals:
    void runningChanged();
    void clientCountChanged(int n);
    void statusMessage(const QString &msg);
    // Lyra is RX-only and the TCI server can't reach the connect flow
    // directly; MainWindow wires these to its Start/Stop handler.
    void startRequested();
    void stopRequested();

private slots:
    void onNewConnection();
    void onTextMessage(const QString &msg);
    void onClientDisconnected();
    void onSmeterTick();
    void onMaintenanceTick();   // ping clients + prune dead sockets
    // Task #33: TCI v2 §3.4 TX_CHRONO outbound pump — periodic when a
    // TX-audio listener is active AND wire MOX is on; emits a type=3
    // frame asking the client to deliver N samples next.
    void onChronoTick();
    // Task #33: hook to HL2Stream::moxActiveChanged so we release
    // TX-audio ownership cleanly when MOX drops externally (operator
    // click, FSM keyup, foot-switch release, §15.20 TX-timeout fire).
    // Equivalent to the working reference's SyncTciPttToMox.
    void onMoxActiveChanged(bool on);
    // TCI v2.0 binary streams (from the DSP worker, queued onto this thread).
    void onTciAudioBlock(const QByteArray &monoFloat, int rateHz);
    void onTciIqBlock(const QByteArray &iqFloat, int rateHz);
    // Task #33: inbound TCI v2 binary frame from any connected client.
    // Parses the 64-byte Stream header (TCI Protocol §3.4) and
    // dispatches per type — TX_AUDIO_STREAM goes to the TciMicSource
    // (with sanitization), other types are ignored.
    void onBinaryMessage(const QByteArray &frame);

private:
    void sendInit(QWebSocket *ws);                 // handshake burst
    // Task #68 — resample inbound TX_AUDIO_STREAM audio from any
    // client-side sample rate to the TXA input rate (typ 48 kHz).
    // Lazy-creates the WDSP resampler on first non-48k frame and
    // on rate change; returns the resampled vector (or `in` by
    // value if rate matches / WDSP missing — fast-path).
    std::vector<float> resampleTxIn(const std::vector<float> &in,
                                    int inRate, int outRate);
    void               destroyTxResampler();
    void dispatch(QWebSocket *ws, const QString &cmd, const QStringList &args);
    void sendTo(QWebSocket *ws, const QString &line);
    void broadcast(const QString &key, const QString &line); // rate-limited
    void broadcastNow(const QString &line);        // unthrottled (edges)
    void pruneDeadClients();                       // drop non-connected sockets
    void recomputeStreaming();                     // enable engine taps iff a client wants them
    // Radio-signal handlers → broadcasts.
    void onFreqChanged();
    void onModeChanged();
    void onRunningChanged();
    void onVolumeChanged();
    void onMutedChanged();
    void onPassbandChanged();
    // Soft-store helpers (DSP params Lyra doesn't truly control yet).
    QString softGet(const QString &key, const QString &def) const;
    void    softSet(const QString &key, const QString &val);
    // Mode mapping between Lyra modes and TCI tokens.
    static QString toTciMode(const QString &lyraMode);
    QString        fromTciMode(const QString &tciMode, qint64 freqHz) const;
    QString        modulationsList() const;
    static int     parseChannel(const QString &s, bool *ok);
    static bool    parseBool(const QString &s);

    Prefs                 *prefs_         = nullptr;
    lyra::ipc::HL2Stream  *stream_        = nullptr;
    lyra::dsp::WdspEngine *engine_        = nullptr;
    SpotStore             *spots_         = nullptr;
    // Task #33: registered at runtime by main() once TxDspWorker /
    // TciMicSource exist (post WDSP-load); null until then, which
    // the binary handler tolerates by dropping frames + rate-limited
    // diagnostic.
    lyra::dsp::TciMicSource *tciMicSource_ = nullptr;
    QWebSocketServer      *server_        = nullptr;
    QList<QWebSocket *>    clients_;
    // Per-client binary-stream subscription state (TCI v2.0 §3.4).
    struct StreamCfg {
        bool audio = false;
        bool iq    = false;
        int  fmt   = 3;   // 0 int16 / 1 int24 / 2 int32 / 3 float32
        int  chans = 2;   // audio channels (1 or 2); IQ is always 2
    };
    QHash<QWebSocket *, StreamCfg> streams_;
    QTimer                *smeterTimer_ = nullptr;
    QTimer                *maintTimer_  = nullptr;   // ping + prune dead clients
    bool                   sensorsEnabled_ = false;

    // Match the reference's RX-audio packetisation cadence (per
    // TCIServer.cs:5437-5513 PublishRxAudioSamples): accumulate
    // the per-DSP-block mono floats arriving from WdspEngine and
    // emit one binary frame only when audioStreamSamples_ samples
    // are queued.  At 48 kHz × 2048 ≈ 42.6 ms blocks, matching the
    // advertised `audio_stream_samples:2048` value clients (MSHV /
    // JTDX / etc.) configure their decoders for.  Without this
    // packetisation Lyra emits 8× more frames at 8× shorter blocks
    // and clients mis-interpret the energy density / FT8 symbol
    // cadence.
    std::vector<float>     audioPending_;
    int                    audioPendingRate_  = 48000;
    static constexpr int   kAudioPacketSamples = 2048;

    // TX-in resampler (Task #68) — lazy-created when an inbound
    // TX_AUDIO_STREAM frame arrives at a rate other than the TXA
    // input rate (kTciTxTargetRate=48000); destroyed + recreated
    // on rate change.  Matches the reference (cmaster.cs:1431-1473
    // resampleTCITxSamples) using the same WDSP polyphase
    // float-vector resampler.  Held opaque void* per WDSP's PORT
    // signature.  Owned by the TciServer (Qt main thread); the
    // inbound binary handler also runs on Qt main, so no mutex.
    void                  *txResampler_           = nullptr;
    int                    txResamplerInRate_     = 0;
    int                    txResamplerOutRate_    = 0;
    // Drop-oldest cap so a stuck/idle client can't grow the
    // accumulator unbounded (recomputeStreaming turns the engine
    // tap off when no clients subscribed; cap is belt-and-braces).
    static constexpr int   kAudioPendingCap   = 4 * kAudioPacketSamples;

    // Task #33: single active TX-audio listener (Thetis-faithful per
    // TCIServer.cs:3503/3510 TryAcquire/Release pattern).  Only ONE
    // TCI client owns the TX audio path at a time; a second client
    // asking for ownership gets denied.  Owned by the Qt main thread
    // (all WebSocket signals queue here) — no mutex needed.
    QWebSocket            *txAudioOwner_      = nullptr;
    // TX_CHRONO outbound pump (TCI v2 §3.4) — fires when txAudioOwner_
    // is set AND wire MOX is active.  Implements the reference's
    // dynamic-pull formula (cmaster.cs:1289-1359) — at each tick,
    // compute how many samples we will have once outstanding requests
    // come in, compare against the target buffer depth (a function
    // of the client's bufferingMs hint + the TXA input rate), and
    // emit `requestsNeeded` CHRONO requests this tick (bounded by
    // kTciTxMaxOutstanding).  Replaces the earlier fixed "one
    // request every 50 ms" pump which produced mis-paced TX audio
    // on the wire (the operator-bench-confirmed FT8 zero-spots
    // symptom 2026-06-01).
    QTimer                *chronoTimer_       = nullptr;
    int                    chronoOutstanding_ = 0;
    qint64                 chronoLastInboundMs_    = 0;    // monotonic ms
    // Dynamic client-negotiated parameters (default to MSHV values).
    // Updated when the client sends AUDIO_SAMPLERATE / AUDIO_STREAM_
    // SAMPLES / TX_STREAM_AUDIO_BUFFERING; consumed every CHRONO tick.
    int                    requestRate_      = 48000;
    int                    requestSamples_   = 2048;
    int                    bufferingMs_      = 50;
    // Lyra-side TXA input constants — txa input rate matches Lyra's
    // open-TX-channel in-rate (48 kHz) and block size matches
    // TxChannel::kInSize (128 samples post-§15.29 / ~2.67 ms).  See
    // the reference's GetInputRate + GetBuffSize (cmaster.cs:1267 +
    // :1313).  For the typical MSHV cadence (requestSamples=2048,
    // requestRate=48000), predictedPacketSamples=2048 and
    // targetQueuedSamples=4800 — the small Lyra txBlock vs the
    // reference's 720 does not affect the formula at this client
    // cadence.
    static constexpr int   kTciTxTargetRate   = 48000;
    static constexpr int   kTciTxBlockSamples = 128;
    static constexpr int   kTciTxMaxOutstanding = 64;   // ref TCI_TX_MAX_OUTSTANDING
    static constexpr int   kTciTxExtraBufferMs = 50;    // ref TCI_TX_EXTRA_BUFFER_MS
    static constexpr int   kChronoIntervalMs   = 50;    // tick cadence
    static constexpr int   kChronoTimeoutMs    = 250;   // outstanding clear
    static constexpr int   kChronoFallbackRequestSamples = 480;
    static constexpr int   kChronoFallbackBufferingMs    = 50;

    // settings
    bool     enabled_          = false;
    int      port_             = 50001;
    QString  bindHost_         = QStringLiteral("127.0.0.1");
    int      rateLimitMs_      = 20;      // ~50 Hz
    bool     sendInitialState_ = true;
    bool     emulateExpertSdr3_= false;
    bool     emulateSunSdr2_   = false;
    bool     cwluBecomesCw_    = true;
    // Task #75 — TCI RX-out linear-gain multiplier.  Cached from
    // Prefs.tciRxGainDb on every tciRxGainDbChanged emit so the
    // hot path in onTciAudioBlock is one std::atomic load (or a
    // single non-atomic double read on the main thread; tcp
    // ingress + main-thread Prefs setter never race).  Default
    // 1.0 = unity = pre-#75 byte-identical wire.
    double   rxGainLinear_     = 1.0;
    // Task #108 — symmetric INBOUND gain cache (MSHV → Lyra TXA).
    // Cached from Prefs.tciTxGainDb on every tciTxGainDbChanged emit
    // so the per-sample multiply in onTciAudioBlock doesn't pay
    // std::pow.  Default 1.0 = unity = pre-#108 byte-identical wire.
    double   txGainLinear_     = 1.0;

    // Per-key broadcast rate limiter + soft DSP-parameter store.
    QHash<QString, qint64> lastSent_;
    QHash<QString, QString> soft_;
    QElapsedTimer          rateClock_;
};

} // namespace lyra::ui
