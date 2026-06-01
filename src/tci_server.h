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

    // settings
    bool     enabled_          = false;
    int      port_             = 50001;
    QString  bindHost_         = QStringLiteral("127.0.0.1");
    int      rateLimitMs_      = 20;      // ~50 Hz
    bool     sendInitialState_ = true;
    bool     emulateExpertSdr3_= false;
    bool     emulateSunSdr2_   = false;
    bool     cwluBecomesCw_    = true;

    // Per-key broadcast rate limiter + soft DSP-parameter store.
    QHash<QString, qint64> lastSent_;
    QHash<QString, QString> soft_;
    QElapsedTimer          rateClock_;
};

} // namespace lyra::ui
