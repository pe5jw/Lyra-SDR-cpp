// Lyra — TCI server.  See tci_server.h.

#include "tci_server.h"

#include "hl2_stream.h"
#include "prefs.h"
#include "spotstore.h"
#include "wdsp_engine.h"

#include <QHostAddress>
#include <QSettings>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <cmath>

namespace lyra::ui {

namespace {
constexpr auto kKeyEnabled  = "tci/enabled";
constexpr auto kKeyPort     = "tci/port";
constexpr auto kKeyHost     = "tci/bind_host";
constexpr auto kKeyRate     = "tci/rate_limit_ms";
constexpr auto kKeyInitial  = "tci/send_initial_state";
constexpr auto kKeyEsdr3    = "tci/emulate_expertsdr3";
constexpr auto kKeySunSdr   = "tci/emulate_sunsdr2";
constexpr auto kKeyCwlu     = "tci/cwlu_becomes_cw";
// vfo_limits advertised to clients (HL2 receive range, Hz).
constexpr qint64 kVfoLo = 10000;
constexpr qint64 kVfoHi = 55000000;

double dbToLinear(double db) {
    if (db <= -60.0) return 0.0;
    return std::pow(10.0, db / 20.0);
}

// TCI v2.0 binary stream (§3.4): a 64-byte little-endian header (16×u32)
// followed by samples.  Header fields: [0]receiver [1]sample_rate
// [2]format [3]codec=0 [4]crc=0 [5]length(scalar count) [6]type
// [7]channels [8..15]reserved.
enum StreamType { STREAM_IQ = 0, STREAM_RX_AUDIO = 1 };
enum SampleFmt  { FMT_INT16 = 0, FMT_INT24 = 1, FMT_INT32 = 2, FMT_FLOAT32 = 3 };

void putU32(char *p, quint32 v) {            // little-endian store
    p[0] = char(v & 0xFF); p[1] = char((v >> 8) & 0xFF);
    p[2] = char((v >> 16) & 0xFF); p[3] = char((v >> 24) & 0xFF);
}
QByteArray streamHeader(quint32 receiver, quint32 rate, quint32 fmt,
                        quint32 length, quint32 type, quint32 channels) {
    QByteArray h(64, '\0');
    char *p = h.data();
    putU32(p + 0,  receiver);
    putU32(p + 4,  rate);
    putU32(p + 8,  fmt);
    putU32(p + 12, 0);          // codec
    putU32(p + 16, 0);          // crc
    putU32(p + 20, length);
    putU32(p + 24, type);
    putU32(p + 28, channels);
    // [8..15] reserved already zero.
    return h;
}

// Convert a mono float32 payload to the requested sample format, optionally
// duplicating to L=R for 2-channel audio.  Returns the sample bytes.
QByteArray audioPayload(const float *mono, int frames, int fmt, int chans) {
    const int scalars = frames * chans;
    auto dup = [&](auto writer) {
        for (int f = 0; f < frames; ++f)
            for (int c = 0; c < chans; ++c) writer(mono[f]);
    };
    QByteArray out;
    if (fmt == FMT_FLOAT32) {
        out.resize(scalars * 4); float *d = reinterpret_cast<float *>(out.data());
        int i = 0; dup([&](float v){ d[i++] = v; });
    } else if (fmt == FMT_INT32) {
        out.resize(scalars * 4); char *d = out.data(); int i = 0;
        dup([&](float v){ putU32(d + 4 * i++, quint32(qint32(std::clamp(v,-1.f,1.f) * 2147483647.0f))); });
    } else if (fmt == FMT_INT24) {
        out.resize(scalars * 3); char *d = out.data(); int i = 0;
        dup([&](float v){ qint32 s = qint32(std::clamp(v,-1.f,1.f) * 8388607.0f);
            d[3*i] = char(s & 0xFF); d[3*i+1] = char((s>>8)&0xFF); d[3*i+2] = char((s>>16)&0xFF); ++i; });
    } else {  // FMT_INT16
        out.resize(scalars * 2); char *d = out.data(); int i = 0;
        dup([&](float v){ qint16 s = qint16(std::clamp(v,-1.f,1.f) * 32767.0f);
            d[2*i] = char(s & 0xFF); d[2*i+1] = char((s>>8)&0xFF); ++i; });
    }
    return out;
}
}

TciServer::TciServer(Prefs *prefs, lyra::ipc::HL2Stream *stream,
                     lyra::dsp::WdspEngine *engine, SpotStore *spots,
                     QObject *parent)
    : QObject(parent), prefs_(prefs), stream_(stream), engine_(engine),
      spots_(spots) {
    rateClock_.start();
    QSettings s;
    enabled_           = s.value(QString::fromLatin1(kKeyEnabled), false).toBool();
    port_              = s.value(QString::fromLatin1(kKeyPort), 50001).toInt();
    bindHost_          = s.value(QString::fromLatin1(kKeyHost),
                                 QStringLiteral("127.0.0.1")).toString();
    rateLimitMs_       = s.value(QString::fromLatin1(kKeyRate), 20).toInt();
    sendInitialState_  = s.value(QString::fromLatin1(kKeyInitial), true).toBool();
    emulateExpertSdr3_ = s.value(QString::fromLatin1(kKeyEsdr3), false).toBool();
    emulateSunSdr2_    = s.value(QString::fromLatin1(kKeySunSdr), false).toBool();
    cwluBecomesCw_     = s.value(QString::fromLatin1(kKeyCwlu), true).toBool();

    smeterTimer_ = new QTimer(this);
    smeterTimer_->setInterval(250);   // 4 Hz sensor cadence
    connect(smeterTimer_, &QTimer::timeout, this, &TciServer::onSmeterTick);

    // Keepalive ping + dead-socket prune.  A client that drops without a
    // clean WebSocket close (process killed, network blip, reconnect) won't
    // fire `disconnected` until a write fails — the ping forces that, and
    // the prune drops any socket no longer in the Connected state so the
    // client count stays accurate (fixes the "shows 1 when none / 2 when
    // one" stale-ghost drift).
    maintTimer_ = new QTimer(this);
    maintTimer_->setInterval(8000);
    connect(maintTimer_, &QTimer::timeout, this, &TciServer::onMaintenanceTick);

    if (stream_) {
        connect(stream_, &lyra::ipc::HL2Stream::rx1FreqChanged,
                this, &TciServer::onFreqChanged);
        connect(stream_, &lyra::ipc::HL2Stream::runningChanged,
                this, &TciServer::onRunningChanged);
    }
    if (prefs_)
        connect(prefs_, &Prefs::modeChanged, this, &TciServer::onModeChanged);
    if (engine_) {
        connect(engine_, &lyra::dsp::WdspEngine::volumeChanged,
                this, &TciServer::onVolumeChanged);
        connect(engine_, &lyra::dsp::WdspEngine::mutedChanged,
                this, &TciServer::onMutedChanged);
        connect(engine_, &lyra::dsp::WdspEngine::passbandChanged,
                this, &TciServer::onPassbandChanged);
        // Binary streams arrive from the DSP worker thread — queued onto
        // ours so the QWebSocket sends happen on the right thread.
        connect(engine_, &lyra::dsp::WdspEngine::tciAudioBlock,
                this, &TciServer::onTciAudioBlock, Qt::QueuedConnection);
        connect(engine_, &lyra::dsp::WdspEngine::tciIqBlock,
                this, &TciServer::onTciIqBlock, Qt::QueuedConnection);
    }
    if (spots_) {
        // Operator clicked a spot on the panadapter → echo it to clients
        // so the logger can log the QSO (spot_activated + rx_clicked_on_spot).
        connect(spots_, &SpotStore::spotActivated, this,
                [this](const QString &call, const QString &mode,
                       qint64 hz, quint32 argb) {
            broadcastNow(QStringLiteral("spot_activated:%1,%2,%3,%4")
                             .arg(call, mode).arg(hz).arg(argb));
            broadcastNow(QStringLiteral("rx_clicked_on_spot:0,0,%1,%2")
                             .arg(call).arg(hz));
        });
    }
}

TciServer::~TciServer() { stop(); }

bool TciServer::running() const { return server_ && server_->isListening(); }

void TciServer::setPort(int p) {
    if (p <= 0 || p > 65535 || p == port_) return;
    port_ = p;
    QSettings().setValue(QString::fromLatin1(kKeyPort), p);
    if (running()) { stop(); start(); }
}
void TciServer::setBindHost(const QString &h) {
    if (h == bindHost_) return;
    bindHost_ = h;
    QSettings().setValue(QString::fromLatin1(kKeyHost), h);
    if (running()) { stop(); start(); }
}
void TciServer::setRateLimitMs(int ms) {
    rateLimitMs_ = qBound(0, ms, 1000);
    QSettings().setValue(QString::fromLatin1(kKeyRate), rateLimitMs_);
}
void TciServer::setSendInitialState(bool on) {
    sendInitialState_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyInitial), on);
}
void TciServer::setEmulateExpertSdr3(bool on) {
    emulateExpertSdr3_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyEsdr3), on);
}
void TciServer::setEmulateSunSdr2(bool on) {
    emulateSunSdr2_ = on;
    QSettings().setValue(QString::fromLatin1(kKeySunSdr), on);
}
void TciServer::setCwluBecomesCw(bool on) {
    cwluBecomesCw_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyCwlu), on);
}
void TciServer::setEnabled(bool on) {
    enabled_ = on;
    QSettings().setValue(QString::fromLatin1(kKeyEnabled), on);
    if (on) start(); else stop();
}

bool TciServer::start() {
    if (running()) return true;
    if (!server_) {
        server_ = new QWebSocketServer(QStringLiteral("Lyra TCI"),
                                       QWebSocketServer::NonSecureMode, this);
        connect(server_, &QWebSocketServer::newConnection,
                this, &TciServer::onNewConnection);
    }
    QHostAddress addr;
    if (bindHost_.isEmpty() || bindHost_ == QStringLiteral("0.0.0.0")
        || bindHost_ == QStringLiteral("*"))
        addr = QHostAddress::Any;
    else
        addr = QHostAddress(bindHost_);
    const bool ok = server_->listen(addr, quint16(port_));
    if (ok) maintTimer_->start();
    emit statusMessage(ok ? tr("TCI server listening on %1:%2")
                                .arg(bindHost_).arg(port_)
                          : tr("TCI server failed to bind %1:%2 — %3")
                                .arg(bindHost_).arg(port_)
                                .arg(server_->errorString()));
    emit runningChanged();
    return ok;
}

void TciServer::stop() {
    smeterTimer_->stop();
    maintTimer_->stop();
    sensorsEnabled_ = false;
    streams_.clear();
    recomputeStreaming();    // turn the engine taps back off
    if (!server_) return;
    for (QWebSocket *ws : std::as_const(clients_)) ws->deleteLater();
    clients_.clear();
    if (server_->isListening()) server_->close();
    emit clientCountChanged(0);
    emit runningChanged();
}

void TciServer::onNewConnection() {
    pruneDeadClients();   // clear any ghost before counting the new one
    while (server_ && server_->hasPendingConnections()) {
        QWebSocket *ws = server_->nextPendingConnection();
        connect(ws, &QWebSocket::textMessageReceived,
                this, &TciServer::onTextMessage);
        connect(ws, &QWebSocket::disconnected,
                this, &TciServer::onClientDisconnected);
        clients_.append(ws);
        sendInit(ws);
        emit clientCountChanged(clients_.size());
    }
}

void TciServer::onClientDisconnected() {
    auto *ws = qobject_cast<QWebSocket *>(sender());
    if (!ws) return;
    clients_.removeAll(ws);
    streams_.remove(ws);
    ws->deleteLater();
    if (clients_.isEmpty()) { smeterTimer_->stop(); sensorsEnabled_ = false; }
    recomputeStreaming();
    emit clientCountChanged(clients_.size());
}

void TciServer::pruneDeadClients() {
    const int before = clients_.size();
    for (int i = clients_.size() - 1; i >= 0; --i) {
        QWebSocket *ws = clients_[i];
        if (!ws || ws->state() != QAbstractSocket::ConnectedState) {
            clients_.removeAt(i);
            streams_.remove(ws);
            if (ws) ws->deleteLater();
        }
    }
    if (clients_.size() != before) {
        if (clients_.isEmpty()) { smeterTimer_->stop(); sensorsEnabled_ = false; }
        recomputeStreaming();
        emit clientCountChanged(clients_.size());
    }
}

void TciServer::recomputeStreaming() {
    bool anyAudio = false, anyIq = false;
    for (const StreamCfg &c : std::as_const(streams_)) {
        anyAudio = anyAudio || c.audio;
        anyIq    = anyIq || c.iq;
    }
    if (engine_) {
        engine_->setTciAudioStreaming(anyAudio);
        engine_->setTciIqStreaming(anyIq);
    }
}

void TciServer::onTciAudioBlock(const QByteArray &monoFloat, int rateHz) {
    if (monoFloat.isEmpty()) return;
    const int frames = monoFloat.size() / int(sizeof(float));
    const float *mono = reinterpret_cast<const float *>(monoFloat.constData());
    for (auto it = streams_.cbegin(); it != streams_.cend(); ++it) {
        if (!it.value().audio) continue;
        QWebSocket *ws = it.key();
        if (!ws || ws->state() != QAbstractSocket::ConnectedState) continue;
        const int chans = it.value().chans, fmt = it.value().fmt;
        QByteArray frame = streamHeader(0, quint32(rateHz), quint32(fmt),
                                        quint32(frames * chans),
                                        STREAM_RX_AUDIO, quint32(chans));
        frame += audioPayload(mono, frames, fmt, chans);
        ws->sendBinaryMessage(frame);
    }
}

void TciServer::onTciIqBlock(const QByteArray &iqFloat, int rateHz) {
    if (iqFloat.isEmpty()) return;
    const int scalars = iqFloat.size() / int(sizeof(float));   // 2×frames
    // IQ is always float32, 2 channels (I,Q interleaved) — pass through.
    QByteArray header = streamHeader(0, quint32(rateHz), FMT_FLOAT32,
                                     quint32(scalars), STREAM_IQ, 2);
    const QByteArray frame = header + iqFloat;
    for (auto it = streams_.cbegin(); it != streams_.cend(); ++it) {
        if (!it.value().iq) continue;
        QWebSocket *ws = it.key();
        if (!ws || ws->state() != QAbstractSocket::ConnectedState) continue;
        ws->sendBinaryMessage(frame);
    }
}

void TciServer::onMaintenanceTick() {
    // Ping live clients (forces a write so a dead peer surfaces), then
    // prune anything no longer connected.
    for (QWebSocket *ws : std::as_const(clients_))
        if (ws && ws->state() == QAbstractSocket::ConnectedState) ws->ping();
    pruneDeadClients();
}

QString TciServer::modulationsList() const {
    QString list = QStringLiteral("USB,LSB,CWU,CWL,AM,SAM,DSB,FM,DIGU,DIGL");
    if (cwluBecomesCw_) list += QStringLiteral(",CW");   // Thetis-style alias
    return list;
}

void TciServer::sendInit(QWebSocket *ws) {
    const int rate = engine_ ? engine_->sampleRate() : 192000;
    const qint64 half = rate / 2;
    const QString mode = prefs_ ? toTciMode(prefs_->mode()) : QStringLiteral("USB");
    const qint64 hz = stream_ ? qint64(stream_->rx1FreqHz()) : 7074000;
    const bool run = stream_ && stream_->isRunning();

    const QString proto = emulateExpertSdr3_ ? QStringLiteral("ExpertSDR3")
                                             : QStringLiteral("Lyra");
    const QString dev = emulateSunSdr2_ ? QStringLiteral("SunSDR2PRO")
                                        : QStringLiteral("HermesLite2");

    sendTo(ws, QStringLiteral("protocol:%1,1.9").arg(proto));
    sendTo(ws, QStringLiteral("device:%1").arg(dev));
    sendTo(ws, QStringLiteral("receive_only:false"));
    sendTo(ws, QStringLiteral("trx_count:1"));
    sendTo(ws, QStringLiteral("channel_count:1"));    // RX1 only (no RX2 yet)
    sendTo(ws, QStringLiteral("channels_count:1"));   // Thetis spelling (compat)
    sendTo(ws, QStringLiteral("vfo_limits:%1,%2").arg(kVfoLo).arg(kVfoHi));
    sendTo(ws, QStringLiteral("if_limits:%1,%2").arg(-half).arg(half));
    sendTo(ws, QStringLiteral("modulations_list:") + modulationsList());
    sendTo(ws, QStringLiteral("ready"));

    if (!sendInitialState_) return;
    const qint64 carrier = hz + (engine_ ? engine_->markerOffsetHz() : 0);
    sendTo(ws, QStringLiteral("dds:0,%1").arg(hz));        // DDS centre
    sendTo(ws, QStringLiteral("vfo:0,0,%1").arg(carrier)); // operating carrier
    sendTo(ws, QStringLiteral("modulation:0,%1").arg(mode));
    if (engine_) {
        sendTo(ws, QStringLiteral("rx_filter_band:0,%1,%2")
                       .arg(qRound(engine_->passbandLowHz()))
                       .arg(qRound(engine_->passbandHighHz())));
        sendTo(ws, QStringLiteral("volume:%1").arg(qRound(engine_->volumeDb())));
        sendTo(ws, QStringLiteral("mute:%1")
                       .arg(engine_->muted() ? QStringLiteral("true")
                                             : QStringLiteral("false")));
    }
    sendTo(ws, QStringLiteral("trx:0,false"));
    sendTo(ws, run ? QStringLiteral("start") : QStringLiteral("stop"));
}

void TciServer::sendTo(QWebSocket *ws, const QString &line) {
    if (ws) ws->sendTextMessage(line + QLatin1Char(';'));
}
void TciServer::broadcastNow(const QString &line) {
    const QString msg = line + QLatin1Char(';');
    for (QWebSocket *ws : std::as_const(clients_)) ws->sendTextMessage(msg);
}
void TciServer::broadcast(const QString &key, const QString &line) {
    if (clients_.isEmpty()) return;
    const qint64 now = rateClock_.elapsed();
    const qint64 last = lastSent_.value(key, -1000000);
    if (now - last < rateLimitMs_) return;
    lastSent_[key] = now;
    broadcastNow(line);
}

QString TciServer::softGet(const QString &key, const QString &def) const {
    return soft_.value(key, def);
}
void TciServer::softSet(const QString &key, const QString &val) {
    soft_[key] = val;
}

int TciServer::parseChannel(const QString &s, bool *ok) {
    const int ch = s.trimmed().toInt(ok);
    if (*ok && (ch == 0 || ch == 1)) return ch;
    *ok = false;
    return -1;
}
bool TciServer::parseBool(const QString &s) {
    const QString u = s.trimmed().toLower();
    return u == QStringLiteral("true") || u == QStringLiteral("1");
}

QString TciServer::toTciMode(const QString &m) {
    if (m == QStringLiteral("CWL") || m == QStringLiteral("CWU"))
        return QStringLiteral("CW");
    if (m == QStringLiteral("FM")) return QStringLiteral("NFM");
    return m;
}
QString TciServer::fromTciMode(const QString &t, qint64 freqHz) const {
    const QString u = t.trimmed().toUpper();
    if (u == QStringLiteral("CW"))
        return freqHz >= 10000000 ? QStringLiteral("CWU") : QStringLiteral("CWL");
    if (u == QStringLiteral("NFM") || u == QStringLiteral("WFM"))
        return QStringLiteral("FM");
    return u;
}

void TciServer::onTextMessage(const QString &raw) {
    auto *ws = qobject_cast<QWebSocket *>(sender());
    QString msg = raw.trimmed();
    while (msg.endsWith(QLatin1Char(';'))) msg.chop(1);
    msg = msg.trimmed();
    if (msg.isEmpty()) return;
    QString cmd;
    QStringList args;
    const int colon = msg.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        cmd = msg.left(colon).trimmed().toUpper();
        const QStringList parts = msg.mid(colon + 1).split(QLatin1Char(','));
        for (const QString &p : parts) args << p.trimmed();
    } else {
        cmd = msg.toUpper();
    }
    dispatch(ws, cmd, args);
}

void TciServer::dispatch(QWebSocket *ws, const QString &cmd,
                         const QStringList &args) {
    const qint64 curHz = stream_ ? qint64(stream_->rx1FreqHz()) : 0;

    // ── lifecycle ────────────────────────────────────────────────
    if (cmd == QStringLiteral("START")) { emit startRequested(); return; }
    if (cmd == QStringLiteral("STOP"))  { emit stopRequested();  return; }
    if (cmd == QStringLiteral("SET_IN_FOCUS")) return;   // no-op

    // ── frequency: VFO / DDS / IF ────────────────────────────────
    if (cmd == QStringLiteral("DDS")) {
        bool okc = false; const int ch = args.size() >= 1 ? parseChannel(args[0], &okc) : -1;
        if (!okc) return;
        if (args.size() >= 2) {
            if (ch == 0 && stream_) { bool f=false; qint64 v=args[1].toLongLong(&f);
                if (f) stream_->setRx1FreqHz(quint32(v)); }
        } else if (ch == 0) sendTo(ws, QStringLiteral("dds:0,%1").arg(curHz));
        return;
    }
    if (cmd == QStringLiteral("VFO")) {
        // VFO = operating (carrier) freq.  In CW the carrier sits pitch Hz
        // off the tuned DDS centre, so convert carrier↔DDS both ways
        // (markerOffsetHz = VFO−DDS; 0 outside CW).
        const qint64 off = engine_ ? engine_->markerOffsetHz() : 0;
        bool okc = false; const int ch = args.size() >= 2 ? parseChannel(args[1], &okc) : -1;
        if (!okc) return;
        if (args.size() >= 3) {
            if (ch == 0 && stream_) { bool f=false; qint64 v=args[2].toLongLong(&f);
                if (f) stream_->setRx1FreqHz(quint32(v - off)); }   // carrier → DDS
        } else if (ch == 0) {
            sendTo(ws, QStringLiteral("vfo:0,0,%1").arg(curHz + off));  // DDS → carrier
        }
        return;
    }
    if (cmd == QStringLiteral("IF")) {
        if (args.size() >= 3) {
            bool okc=false; const int ch = parseChannel(args[1], &okc);
            bool oko=false; const qint64 off = args[2].toLongLong(&oko);
            if (okc && oko && ch == 0 && stream_)
                stream_->setRx1FreqHz(quint32(curHz + off));
        }
        return;
    }

    // ── mode + filter ────────────────────────────────────────────
    if (cmd == QStringLiteral("MODULATION")) {
        bool okc = false; const int ch = args.size() >= 1 ? parseChannel(args[0], &okc) : -1;
        if (!okc) return;
        if (args.size() >= 2) {
            if (ch == 0 && prefs_) prefs_->setMode(fromTciMode(args[1], curHz));
        } else if (ch == 0) {
            const QString m = prefs_ ? toTciMode(prefs_->mode()) : QStringLiteral("USB");
            sendTo(ws, QStringLiteral("modulation:0,%1").arg(m));
        }
        return;
    }
    if (cmd == QStringLiteral("RX_FILTER_BAND")) {
        bool okc = false; const int ch = args.size() >= 1 ? parseChannel(args[0], &okc) : -1;
        if (!okc) return;
        if (args.size() >= 3 && engine_) {
            bool a=false,b=false; const qint64 lo=args[1].toLongLong(&a);
            const qint64 hi=args[2].toLongLong(&b);
            if (a && b && ch == 0) engine_->setBandwidth(int(qAbs(hi - lo)));
        } else if (ch == 0 && engine_) {
            sendTo(ws, QStringLiteral("rx_filter_band:0,%1,%2")
                           .arg(qRound(engine_->passbandLowHz()))
                           .arg(qRound(engine_->passbandHighHz())));
        }
        return;
    }

    // ── audio: VOLUME / MUTE / RX_VOLUME / RX_MUTE ───────────────
    if (cmd == QStringLiteral("VOLUME")) {
        if (!args.isEmpty() && engine_) { bool f=false; double db=args[0].toDouble(&f);
            if (f) engine_->setVolume(dbToLinear(db)); }
        else if (engine_) sendTo(ws, QStringLiteral("volume:%1").arg(qRound(engine_->volumeDb())));
        return;
    }
    if (cmd == QStringLiteral("MUTE")) {
        if (!args.isEmpty() && engine_) engine_->setMuted(parseBool(args[0]));
        else if (engine_) sendTo(ws, QStringLiteral("mute:%1")
                       .arg(engine_->muted() ? QStringLiteral("true") : QStringLiteral("false")));
        return;
    }
    if (cmd == QStringLiteral("RX_VOLUME")) {
        bool okc=false; const int ch = args.size()>=2 ? parseChannel(args[1],&okc) : -1;
        if (!okc) return;
        if (args.size() >= 3 && engine_) { bool f=false; double db=args[2].toDouble(&f);
            if (f && ch==0) engine_->setVolume(dbToLinear(db)); }
        else if (ch==0 && engine_)
            sendTo(ws, QStringLiteral("rx_volume:0,0,%1").arg(qRound(engine_->volumeDb())));
        return;
    }
    if (cmd == QStringLiteral("RX_MUTE")) {
        bool okc=false; const int ch = args.size()>=1 ? parseChannel(args[0],&okc) : -1;
        if (!okc) return;
        if (args.size() >= 2 && engine_) { if (ch==0) engine_->setMuted(parseBool(args[1])); }
        else if (ch==0 && engine_)
            sendTo(ws, QStringLiteral("rx_mute:0,%1")
                       .arg(engine_->muted() ? QStringLiteral("true") : QStringLiteral("false")));
        return;
    }
    if (cmd == QStringLiteral("CW_PITCH")) {
        if (!args.isEmpty() && engine_) {
            // CW_PITCH:rx[,hz] — last arg is the pitch.
            bool f=false; const int hz = args.last().toInt(&f);
            if (f && args.size() >= 2) engine_->setCwPitchHz(hz);
            else if (args.size() == 1) {
                bool g=false; const int p = args[0].toInt(&g);
                if (g) engine_->setCwPitchHz(p);
            }
        }
        if (args.size() < 2 && engine_)
            sendTo(ws, QStringLiteral("cw_pitch:0,%1").arg(engine_->cwPitchHz()));
        return;
    }

    // ── sensors / S-meter ────────────────────────────────────────
    if (cmd == QStringLiteral("RX_SENSORS_ENABLE")) {
        sensorsEnabled_ = !args.isEmpty() && parseBool(args[0]);
        if (sensorsEnabled_ && !clients_.isEmpty()) smeterTimer_->start();
        else smeterTimer_->stop();
        return;
    }
    if (cmd == QStringLiteral("TX_SENSORS_ENABLE")) return;  // RX-only

    // ── RX-only: acknowledge TX-side queries as inactive ─────────
    if (cmd == QStringLiteral("TRX") || cmd == QStringLiteral("TUNE")
        || cmd == QStringLiteral("RIT_ENABLE") || cmd == QStringLiteral("XIT_ENABLE")
        || cmd == QStringLiteral("SPLIT_ENABLE")) {
        const QString idx = args.isEmpty() ? QStringLiteral("0") : args[0];
        sendTo(ws, cmd.toLower() + QStringLiteral(":%1,false").arg(idx));
        return;
    }
    // TX-only setters with no read-back of interest — silently accept.
    if (cmd == QStringLiteral("DRIVE") || cmd == QStringLiteral("TUNE_DRIVE")
        || cmd == QStringLiteral("MON_ENABLE") || cmd == QStringLiteral("MON_VOLUME")
        || cmd == QStringLiteral("RIT_OFFSET") || cmd == QStringLiteral("XIT_OFFSET")
        || cmd == QStringLiteral("KEYER")
        || cmd.startsWith(QStringLiteral("CW_MACROS"))
        || cmd == QStringLiteral("CW_MSG") || cmd == QStringLiteral("CW_TERMINAL")
        || cmd == QStringLiteral("CW_KEYER_SPEED")
        || cmd == QStringLiteral("TX_STREAM_AUDIO_BUFFERING"))
        return;

    // ── DX-cluster spots ─────────────────────────────────────────
    if (cmd == QStringLiteral("SPOT")) {
        // SPOT:call,mode,freqHz[,argb][,text]
        if (spots_ && args.size() >= 3) {
            bool f = false; const qint64 hz = args[2].toLongLong(&f);
            quint32 argb = 0xFFFFD700u;
            if (args.size() >= 4) {
                bool g = false; const quint32 c = args[3].toUInt(&g);
                if (g && c) argb = c;
            }
            const QString text = args.size() >= 5 ? args.mid(4).join(QLatin1Char(',')) : QString();
            if (f) spots_->addSpot(args[0], args[1], hz, argb, text);
        }
        return;
    }
    if (cmd == QStringLiteral("SPOT_DELETE")) {
        if (spots_ && !args.isEmpty()) spots_->deleteSpot(args[0]);
        return;
    }
    if (cmd == QStringLiteral("SPOT_CLEAR")) {
        if (spots_) spots_->clearAll();
        return;
    }

    // ── binary streams (TCI v2.0 §3.4) ───────────────────────────
    if (cmd.startsWith(QStringLiteral("AUDIO_")) || cmd.startsWith(QStringLiteral("IQ_"))
        || cmd.startsWith(QStringLiteral("LINE_OUT_"))) {
        if (!ws) return;
        StreamCfg &cfg = streams_[ws];
        // LINE_OUT is the HL2 codec line-out — alias of the RX audio stream.
        if (cmd == QStringLiteral("AUDIO_START") || cmd == QStringLiteral("LINE_OUT_START")) {
            cfg.audio = true;  recomputeStreaming();
        } else if (cmd == QStringLiteral("AUDIO_STOP") || cmd == QStringLiteral("LINE_OUT_STOP")) {
            cfg.audio = false; recomputeStreaming();
        } else if (cmd == QStringLiteral("IQ_START")) {
            cfg.iq = true;     recomputeStreaming();
        } else if (cmd == QStringLiteral("IQ_STOP")) {
            cfg.iq = false;    recomputeStreaming();
        } else if (cmd == QStringLiteral("AUDIO_STREAM_SAMPLE_TYPE") && !args.isEmpty()) {
            const QString t = args[0].toLower();
            cfg.fmt = (t == QStringLiteral("int16"))   ? FMT_INT16
                    : (t == QStringLiteral("int24"))   ? FMT_INT24
                    : (t == QStringLiteral("int32"))   ? FMT_INT32 : FMT_FLOAT32;
        } else if (cmd == QStringLiteral("AUDIO_STREAM_CHANNELS") && !args.isEmpty()) {
            cfg.chans = (args[0].toInt() == 1) ? 1 : 2;
        }
        // AUDIO_SAMPLERATE / IQ_SAMPLERATE / AUDIO_STREAM_SAMPLES accepted
        // but Lyra streams at its native rate (48 kHz audio / the IQ rate)
        // and reports that rate in each frame header.
        return;
    }

    // ── DSP toggles Lyra doesn't yet expose: soft store + echo ───
    // Accept the write and round-trip it so client panels stay in sync;
    // wired to the real WDSP chain when the DSP+Audio panel lands.
    static const QStringList kSoft = {
        QStringLiteral("AGC_MODE"), QStringLiteral("AGC_GAIN"),
        QStringLiteral("SQL_ENABLE"), QStringLiteral("SQL_LEVEL"),
        QStringLiteral("RX_NB_ENABLE"), QStringLiteral("RX_NB_PARAM"),
        QStringLiteral("RX_NR_ENABLE"), QStringLiteral("RX_ANF_ENABLE"),
        QStringLiteral("RX_ANC_ENABLE"), QStringLiteral("RX_APF_ENABLE"),
        QStringLiteral("RX_BIN_ENABLE"), QStringLiteral("RX_DSE_ENABLE"),
        QStringLiteral("RX_NF_ENABLE"), QStringLiteral("RX_BALANCE"),
        QStringLiteral("DIGL_OFFSET"), QStringLiteral("DIGU_OFFSET"),
        QStringLiteral("LOCK"), QStringLiteral("VFO_LOCK"),
        QStringLiteral("RX_CHANNEL_ENABLE")
    };
    if (kSoft.contains(cmd)) {
        // Heuristic: a "channelled" command keeps arg[0] (the rx index) in
        // the reply; the value is everything the client sent.  On a bare
        // query (only the rx index, or nothing) echo the stored/default.
        const bool boolish = cmd.endsWith(QStringLiteral("_ENABLE"))
                             || cmd == QStringLiteral("LOCK")
                             || cmd == QStringLiteral("VFO_LOCK");
        const QString def = boolish ? QStringLiteral("false") : QStringLiteral("0");
        if (args.size() >= 2) {                       // write (rx + value…)
            softSet(cmd, args.mid(1).join(QLatin1Char(',')));
        } else {                                       // query
            const QString rx = args.isEmpty() ? QStringLiteral("0") : args[0];
            sendTo(ws, cmd.toLower()
                       + QStringLiteral(":%1,%2").arg(rx, softGet(cmd, def)));
        }
        return;
    }

    // Unknown command → silently ignore (spec-compliant).
    Q_UNUSED(ws);
}

// ── broadcast handlers (radio → clients) ─────────────────────────
void TciServer::onFreqChanged() {
    if (!stream_) return;
    const qint64 hz = qint64(stream_->rx1FreqHz());          // DDS centre
    const qint64 carrier = hz + (engine_ ? engine_->markerOffsetHz() : 0);
    broadcast(QStringLiteral("dds:0"),   QStringLiteral("dds:0,%1").arg(hz));
    broadcast(QStringLiteral("vfo:0,0"), QStringLiteral("vfo:0,0,%1").arg(carrier));
}
void TciServer::onModeChanged() {
    if (!prefs_) return;
    broadcast(QStringLiteral("modulation:0"),
              QStringLiteral("modulation:0,%1").arg(toTciMode(prefs_->mode())));
    // The CW carrier offset flips with the mode, so the operating (VFO)
    // freq changes even though the DDS centre didn't — re-announce it.
    if (stream_) {
        const qint64 carrier = qint64(stream_->rx1FreqHz())
                               + (engine_ ? engine_->markerOffsetHz() : 0);
        broadcast(QStringLiteral("vfo:0,0"),
                  QStringLiteral("vfo:0,0,%1").arg(carrier));
    }
}
void TciServer::onVolumeChanged() {
    if (!engine_) return;
    broadcast(QStringLiteral("volume"),
              QStringLiteral("volume:%1").arg(qRound(engine_->volumeDb())));
}
void TciServer::onMutedChanged() {
    if (!engine_) return;
    broadcastNow(QStringLiteral("mute:%1")
                     .arg(engine_->muted() ? QStringLiteral("true")
                                           : QStringLiteral("false")));
}
void TciServer::onPassbandChanged() {
    if (!engine_) return;
    broadcast(QStringLiteral("rx_filter_band:0"),
              QStringLiteral("rx_filter_band:0,%1,%2")
                  .arg(qRound(engine_->passbandLowHz()))
                  .arg(qRound(engine_->passbandHighHz())));
}
void TciServer::onRunningChanged() {
    if (!stream_) return;
    broadcastNow(stream_->isRunning() ? QStringLiteral("start")
                                      : QStringLiteral("stop"));
}
void TciServer::onSmeterTick() {
    if (!sensorsEnabled_ || clients_.isEmpty() || !stream_) return;
    // rx1DbFs is baseband RMS in dBFS; offset to a rough dBm for display.
    // (A precise S-meter cal arrives with the meters feature.)
    const double dbfs = stream_->rx1DbFs();
    const double dbm = (dbfs <= -199.0) ? -140.0 : dbfs - 30.0;
    broadcast(QStringLiteral("rx_channel_sensors:0,0"),
              QStringLiteral("rx_channel_sensors:0,0,%1")
                  .arg(QString::number(dbm, 'f', 1)));
}

} // namespace lyra::ui
