// Lyra — TCI server.  See tci_server.h.

#include "tci_server.h"

#include "hl2_stream.h"
#include "logbuffer.h"
#include "prefs.h"
#include "spotstore.h"
// TX-rip Phase 1 (Q2): tci_mic_source.h / tx_dsp_worker.h removed —
// TX DSP worker + TCI mic source are being rebuilt from empty files
// per docs/TX_ARCHITECTURAL_MAPPING.md §10.3.  Inbound TX_AUDIO_STREAM
// is dropped; R-H2 mic-source force/restore returns with the rebuild.
#include "tci/TciTxBridge.h"   // TCI TX-audio re-home sink (TX_ARCH §4.6/§10.3)
#include "wdsp_engine.h"

#include <QDateTime>
#include <QHostAddress>
#include <QSettings>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

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
enum StreamType {
    STREAM_IQ        = 0,
    STREAM_RX_AUDIO  = 1,
    STREAM_TX_AUDIO  = 2,   // client → server, TX audio payload (Task #33)
    STREAM_TX_CHRONO = 3,   // server → client, "send me N samples" pull
    STREAM_LINEOUT   = 4
};
enum SampleFmt  { FMT_INT16 = 0, FMT_INT24 = 1, FMT_INT32 = 2, FMT_FLOAT32 = 3 };

void putU32(char *p, quint32 v) {            // little-endian store
    p[0] = char(v & 0xFF); p[1] = char((v >> 8) & 0xFF);
    p[2] = char((v >> 16) & 0xFF); p[3] = char((v >> 24) & 0xFF);
}
quint32 getU32(const char *p) noexcept {     // little-endian load
    return  quint32(quint8(p[0]))
         | (quint32(quint8(p[1])) <<  8)
         | (quint32(quint8(p[2])) << 16)
         | (quint32(quint8(p[3])) << 24);
}

// Task #33: bytes-per-sample for the four TCI sample formats.  Used to
// validate payload size against the header's `length` field before
// decoding.
int bytesPerSample(int fmt) {
    switch (fmt) {
        case FMT_INT16:   return 2;
        case FMT_INT24:   return 3;
        case FMT_INT32:   return 4;
        case FMT_FLOAT32: return 4;
        default:          return 0;
    }
}

// Task #33: decode an inbound TX_AUDIO_STREAM payload into mono
// float32 @ the source rate.  Handles all four sample formats, mono
// vs 2-channel (DIGU/DIGL complex IQ → real part = mono audio for
// the WDSP TXA chain, matching the working reference's behaviour for
// SSB/DIGU TX), and sanitizes NaN / ±Inf → 0 + clamps to ±4.0 per the
// working reference's `handleBinaryFrame` clamp at TCIServer.cs:
// 5661-5673.
//
// Returns the decoded mono samples (one float per frame, regardless
// of input channel count).  Returns empty if the input is malformed.
std::vector<float> decodeTciTxAudio(const char *data, int dataBytes,
                                    int fmt, int channels) {
    const int bps = bytesPerSample(fmt);
    if (bps == 0 || dataBytes < bps) return {};
    if (channels < 1) channels = 1;
    if (channels > 2) channels = 2;          // TCI v2 spec: 1 or 2
    const int scalarCount = dataBytes / bps;
    const int frames = scalarCount / channels;
    if (frames <= 0) return {};

    std::vector<float> mono;
    mono.reserve(static_cast<std::size_t>(frames));

    auto decodeScalar = [&](int idx) -> float {
        const char *p = data + idx * bps;
        switch (fmt) {
            case FMT_INT16: {
                qint16 s = qint16(quint16(quint8(p[0])) | (quint16(quint8(p[1])) << 8));
                return float(s) / 32768.0f;
            }
            case FMT_INT24: {
                qint32 s = qint32(quint32(quint8(p[0]))
                                | (quint32(quint8(p[1])) << 8)
                                | (quint32(quint8(p[2])) << 16));
                if (s & 0x800000) s |= int(0xFF000000U); // sign-extend 24→32
                return float(s) / 8388608.0f;
            }
            case FMT_INT32: {
                qint32 s = qint32(getU32(p));
                return float(double(s) / 2147483648.0);
            }
            case FMT_FLOAT32: {
                float v;
                std::memcpy(&v, p, 4);
                return v;
            }
        }
        return 0.0f;
    };

    auto sanitize = [](float s) -> float {
        if (std::isnan(s) || std::isinf(s)) return 0.0f;
        if (s >  4.0f) return  4.0f;
        if (s < -4.0f) return -4.0f;
        return s;
    };

    if (channels == 1) {
        for (int f = 0; f < frames; ++f)
            mono.push_back(sanitize(decodeScalar(f)));
    } else {
        // 2-channel inbound: average left + right into a single mono
        // float per audio frame.  This matches the working reference's
        // default `TCITxStereoInputMode.Both` behaviour at
        // cmaster.cs:1412-1416 (`mono[i] = (float)((left + right) * 0.5)`).
        //
        // Earlier Lyra code took ONLY the left channel here on a
        // misread of the TCI spec §3.4 "complex signal if channels=2"
        // line — that line refers to TCI v2 IQ streams (a different
        // dispatch path), NOT TX_AUDIO_STREAM.  For TX audio the
        // 2-channel payload is plain stereo PCM and clients are free
        // to place the modulated waveform in either channel or both;
        // dropping the right channel silently lost the audio whenever
        // a client (MSHV-class digital-modes client) put the signal
        // in R-only or distributed it across both channels.  The
        // operator-bench symptom was a single vertical line on the
        // panadapter during FT8 transmit (suppressed-carrier residue
        // only) and zero PSKReporter spots.
        for (int f = 0; f < frames; ++f) {
            const float l = decodeScalar(f * 2);
            const float r = decodeScalar(f * 2 + 1);
            mono.push_back(sanitize(0.5f * (l + r)));
        }
    }
    return mono;
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
        // Task #33 — SyncTciPttToMox (TCIServer.cs:5560-5577).  If the
        // wire MOX bit drops externally (operator click, FSM keyup,
        // foot-switch release, §15.20 TX-timeout fire) AND we owned
        // the TX-audio path, release ownership + stop the CHRONO
        // pump.  Keeps TCI-server state coherent with the real wire.
        connect(stream_, &lyra::ipc::HL2Stream::moxActiveChanged,
                this, &TciServer::onMoxActiveChanged);
    }
    // Task #33 — TX_CHRONO outbound pump.  Always constructed; only
    // ticks when the timer is started (in tryAcquireActiveTxAudioListener).
    chronoTimer_ = new QTimer(this);
    chronoTimer_->setInterval(kChronoIntervalMs);
    // PreciseTimer: Qt's default CoarseTimer downgrades sub-50 ms
    // intervals to ~5 % accuracy, which at 2 ms means the kernel
    // could quantise the tick up to ~20 ms.  Precise mode keeps the
    // 2 ms cadence the reference's TX stream loop establishes
    // (cmaster.cs:1253 `Thread.Sleep(2)`).  Per-tick work is
    // microseconds — Qt PreciseTimer at 500 Hz is well within budget.
    chronoTimer_->setTimerType(Qt::PreciseTimer);
    connect(chronoTimer_, &QTimer::timeout, this, &TciServer::onChronoTick);
    if (prefs_)
        connect(prefs_, &Prefs::modeChanged, this, &TciServer::onModeChanged);

    // Task #75 — cache the RX-out gain as a linear multiplier from
    // Prefs.tciRxGainDb so the audio hot path doesn't pay std::pow
    // per packet.  Seeded from the persisted value at ctor; refreshed
    // on every tciRxGainDbChanged emit (operator drags the Settings
    // slider, future TX-profile recall, etc.).
    if (prefs_) {
        rxGainLinear_ =
            std::pow(10.0, prefs_->tciRxGainDb() / 20.0);
        connect(prefs_, &Prefs::tciRxGainDbChanged, this, [this]() {
            rxGainLinear_ =
                std::pow(10.0, prefs_->tciRxGainDb() / 20.0);
        });
        // Task #108 — symmetric INBOUND (MSHV → Lyra TXA) gain cache.
        // Seeded from the persisted value at ctor; refreshed on every
        // tciTxGainDbChanged emit (operator drags the Settings slider,
        // future TX-profile recall, etc.).  Hot path applies the
        // multiplier in onTciAudioBlock right before TciMicSource->
        // submitFromTci, AFTER decode + resample.
        txGainLinear_ =
            std::pow(10.0, prefs_->tciTxGainDb() / 20.0);
        connect(prefs_, &Prefs::tciTxGainDbChanged, this, [this]() {
            txGainLinear_ =
                std::pow(10.0, prefs_->tciTxGainDb() / 20.0);
        });
    }
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
    // CODEX-P0 (2026-06-15) — release TX-audio ownership cleanly BEFORE the
    // owner sockets are deleteLater()'d below.  Without this, stop() (TCI
    // disable / profile-toggle that flips TCI-enable / shutdown) left
    // chronoTimer_ running against a soon-to-be-destroyed owner socket, so
    // onChronoTick dereferenced a dangling pointer (the ntdll 0xC0000005),
    // and could also leave the radio keyed + replay the previous
    // transmission's audio tail.  (txAudioOwner_ is now a QPointer too, so
    // the deref is crash-safe by construction; this is the matching clean
    // teardown.)
    chronoTimer_->stop();
    chronoOutstanding_ = 0;
    if (txAudioOwner_) {
        txAudioOwner_ = nullptr;
        if (stream_) stream_->requestMoxFromTci(false);
    }
    lyra::tci::TciTxBridge::instance().clear();   // drop stale TX backlog (no replay next session)
    sensorsEnabled_ = false;
    streams_.clear();
    recomputeStreaming();    // turn the engine taps back off
    destroyTxResampler();    // Task #68 — free WDSP resampler if any
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
        // Task #33: TCI v2 binary frames (TX_AUDIO_STREAM in, RX/IQ
        // streams out).  Inbound parsing handles only TX_AUDIO_STREAM
        // (Task #33); other types arrive harmlessly + ignored.
        connect(ws, &QWebSocket::binaryMessageReceived,
                this, &TciServer::onBinaryMessage);
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
    // Task #33 — release TX-audio ownership cleanly if the dropping
    // client was the owner.  Drops in-flight TX_AUDIO_STREAM frames
    // (binary handler gates on owner identity), stops the CHRONO
    // pump, and releases MOX via the source-tagged wrapper.
    if (ws == txAudioOwner_) {
        txAudioOwner_ = nullptr;
        chronoOutstanding_ = 0;
        chronoTimer_->stop();
        emit statusMessage(QStringLiteral(
            "TCI: TX-audio ownership released (client disconnected)"));
        if (stream_) stream_->requestMoxFromTci(false);
        // TX-rip Phase 1 (Q2): R-H2 activeMicSource_ save/restore
        // returns with the new TX DSP worker per
        // docs/TX_ARCHITECTURAL_MAPPING.md §10.3.
    }
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

std::vector<float> TciServer::resampleTxIn(const std::vector<float> &in,
                                           int inRate, int outRate) {
    // Reference: cmaster.cs:1431-1473 resampleTCITxSamples.
    if (in.empty() || inRate <= 0 || outRate <= 0) return {};
    if (inRate == outRate) return in;
    if (!engine_ || !engine_->wdspNative()) return {};
    const lyra::dsp::WdspApi &api = engine_->wdspNative()->api();
    if (!api.create_resampleFV || !api.xresampleFV
        || !api.destroy_resampleFV)
        return {};

    // Lazy create / recreate on rate change.
    if (!txResampler_
        || txResamplerInRate_  != inRate
        || txResamplerOutRate_ != outRate) {
        if (txResampler_) {
            api.destroy_resampleFV(txResampler_);
            txResampler_ = nullptr;
        }
        txResampler_       = api.create_resampleFV(inRate, outRate);
        txResamplerInRate_ = inRate;
        txResamplerOutRate_ = outRate;
        if (!txResampler_) return {};
    }

    // Output sizing: reference uses
    //   max(in.size + 64, ceil(in.size * outRate / inRate) + 64)
    // — generous slack so xresampleFV never overruns.
    const int inN = int(in.size());
    const int upperOut =
        int((qint64(inN) * outRate + inRate - 1) / inRate);
    const int maxOut = std::max(inN, upperOut) + 64;
    std::vector<float> out(size_t(maxOut), 0.0f);
    int outSamps = 0;
    // create_resampleFV takes a const input but the C signature is
    // non-const — safe to cast away since the resampler doesn't mutate.
    api.xresampleFV(const_cast<float *>(in.data()),
                    out.data(), inN, &outSamps, txResampler_);
    if (outSamps <= 0) return {};
    out.resize(size_t(outSamps));
    return out;
}

void TciServer::destroyTxResampler() {
    if (!txResampler_) return;
    if (engine_ && engine_->wdspNative()) {
        const lyra::dsp::WdspApi &api = engine_->wdspNative()->api();
        if (api.destroy_resampleFV) api.destroy_resampleFV(txResampler_);
    }
    txResampler_        = nullptr;
    txResamplerInRate_  = 0;
    txResamplerOutRate_ = 0;
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
    // Drop the RX-audio accumulator when no client subscribes so
    // a re-subscribe starts from a clean buffer instead of leaking
    // stale samples (worst case ≈ 42 ms of pre-disconnect audio).
    if (!anyAudio) audioPending_.clear();
}

void TciServer::onTciAudioBlock(const QByteArray &monoFloat, int rateHz) {
    if (monoFloat.isEmpty()) return;
    const int frames = monoFloat.size() / int(sizeof(float));
    if (frames <= 0) return;
    const float *mono =
        reinterpret_cast<const float *>(monoFloat.constData());

    // Accumulate-and-drain at the reference's per-client packet
    // size (TCIServer.cs:5437-5513 / m_audioStreamSamples = 2048).
    // WdspEngine delivers per-DSP-block bursts (typ 256 samples /
    // ~5.3 ms at 48 kHz) — left un-packetised, clients (MSHV /
    // JTDX / etc.) see audio at 8× the configured frame cadence
    // and mis-interpret energy / FT8 symbol timing (the operator-
    // bench-confirmed hot-waterfall symptom 2026-06-01).  Pack
    // into kAudioPacketSamples (= 2048) frames before emitting so
    // the wire matches what we advertise at handshake and what
    // the client's decoder expects.
    audioPendingRate_ = rateHz;
    // Task #75 — apply the operator's TCI RX-out gain ONCE at
    // ingress.  Unity (1.0) shortcuts the per-sample multiply for
    // the common default-OFF path so a 0 dB operator pays nothing.
    // Non-unity multiplies into the accumulator as samples are
    // copied in — clipping is not enforced here (3rd-party clients
    // FT8-decode in float; saturating to ±1.0 would clip-distort
    // strong signals and degrade decode SNR more than a soft float
    // overshoot would).
    const double g = rxGainLinear_;
    if (g == 1.0) {
        audioPending_.insert(audioPending_.end(), mono, mono + frames);
    } else {
        const float gf = float(g);
        audioPending_.reserve(audioPending_.size() + size_t(frames));
        for (int i = 0; i < frames; ++i)
            audioPending_.push_back(mono[i] * gf);
    }

    // Drop-oldest cap: never let the accumulator grow unbounded
    // (recomputeStreaming turns the engine tap off when no client
    // subscribes — this is belt-and-braces for the connect/
    // disconnect window).  Older samples are stale by definition;
    // discard them, keep the freshest tail.
    if (int(audioPending_.size()) > kAudioPendingCap) {
        const int drop = int(audioPending_.size()) - kAudioPendingCap;
        audioPending_.erase(audioPending_.begin(),
                            audioPending_.begin() + drop);
    }

    // Drain in a loop: a single onTciAudioBlock call might cross
    // the threshold more than once (e.g. a backlog from a paused
    // client returning).  Each iteration emits one fully-formed
    // kAudioPacketSamples frame per audio-subscribed client.
    while (int(audioPending_.size()) >= kAudioPacketSamples) {
        const float *block = audioPending_.data();
        for (auto it = streams_.cbegin(); it != streams_.cend(); ++it) {
            if (!it.value().audio) continue;
            QWebSocket *ws = it.key();
            if (!ws || ws->state() != QAbstractSocket::ConnectedState)
                continue;
            const int chans = it.value().chans, fmt = it.value().fmt;
            QByteArray frame = streamHeader(
                0, quint32(audioPendingRate_), quint32(fmt),
                quint32(kAudioPacketSamples * chans),
                STREAM_RX_AUDIO, quint32(chans));
            frame += audioPayload(block, kAudioPacketSamples, fmt, chans);
            ws->sendBinaryMessage(frame);
        }
        audioPending_.erase(audioPending_.begin(),
                            audioPending_.begin() + kAudioPacketSamples);
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

void TciServer::onBinaryMessage(const QByteArray &frame) {
    // Task #33: inbound TCI v2 binary frame.  Only TX_AUDIO_STREAM is
    // dispatched on the server side today; other types arrive
    // harmlessly + are ignored (the working reference returns early
    // on streamType != TX_AUDIO_STREAM at TCIServer.cs:5614).  See
    // docs/refs/mshv_tci/README.md for the architectural derivation.
    //
    // Single-active-listener gate (working reference pattern at
    // TCIServer.cs:5535-5557): only the client that owns TX-audio
    // ownership (acquired via trx:0,true,tci) may push frames into
    // the TX path.  This is also the "clearQueuedTxAudio" equivalent
    // for Lyra's no-intermediate-buffer architecture — releasing
    // ownership drops in-flight frames at the door without ever
    // reaching the decode / sanitize / push path.
    auto *ws = qobject_cast<QWebSocket *>(sender());
    // Task #33 commit 3.3 — ALWAYS-on binary-frame arrival diagnostic
    // (NOT verbose-gated).  Rate-limited to one line every ~250 ms so
    // the log doesn't drown if MSHV starts streaming continuously
    // (~20 TX_AUDIO_STREAM frames per second at 50 ms / 2400-sample
    // block cadence).  Counts frames received from the active TX-
    // audio owner vs others — helps the bench distinguish "MSHV not
    // sending anything" from "MSHV sending but Lyra not owner-
    // acquired."
    {
        static qint64 lastLogMs = 0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastLogMs > 250) {
            lastLogMs = nowMs;
            qInfo("[tci-rx-bin] frame %d bytes (owner=%s sender=%s)",
                  int(frame.size()),
                  txAudioOwner_ ? "set" : "null",
                  ws == txAudioOwner_ ? "owner" : "other");
        }
    }
    if (txAudioOwner_ == nullptr || ws != txAudioOwner_) return;
    // Note arrival of an inbound TX_AUDIO_STREAM block for the
    // CHRONO outstanding-counter timeout reset (working reference:
    // cmaster.cs:1303-1305 decrement on dequeue).
    if (chronoOutstanding_ > 0) --chronoOutstanding_;
    chronoLastInboundMs_ = QDateTime::currentMSecsSinceEpoch();

    if (frame.size() < 64) return;
    const char *p = frame.constData();
    const quint32 sampleRate = getU32(p + 4);
    const quint32 fmt        = getU32(p + 8);
    const quint32 length     = getU32(p + 20);  // scalar count requested
    const quint32 type       = getU32(p + 24);
    const quint32 headerChan = getU32(p + 28);
    if (type != STREAM_TX_AUDIO || length == 0) return;

    const int dataOffset = 64;
    const int dataBytes  = frame.size() - dataOffset;
    const int bps        = bytesPerSample(int(fmt));
    if (bps == 0 || dataBytes < bps) return;

    // Channel inference — modern (1 or 2) per the working reference's
    // header convention.  Legacy clients send channels=0 in the
    // header and rely on payload size to disambiguate (TCIServer.cs:
    // 5630-5652): if payload has 2× length scalars, it's stereo
    // interleaved; otherwise mono.  Preserve that compatibility so
    // older JTDX-style clients also work.
    int channels = int(headerChan);
    if (channels != 1 && channels != 2) {
        const int actualScalars = dataBytes / bps;
        channels = (actualScalars >= int(length) * 2) ? 2 : 1;
    }

    // Cap decode to whatever the header advertised in `length`, but
    // never read past the actual payload.  `length` is in scalars
    // (samples × channels) per the working reference's convention.
    int decodeScalars = std::min<int>(int(length), dataBytes / bps);
    if (channels == 2) decodeScalars -= decodeScalars % 2;  // pair-align
    if (decodeScalars <= 0) return;

    std::vector<float> mono = decodeTciTxAudio(p + dataOffset,
                                               decodeScalars * bps,
                                               int(fmt), channels);
    if (mono.empty()) return;

    // Match the reference (cmaster.cs:1431-1473 resampleTCITxSamples)
    // — accept any sample rate, resample to the TXA input rate
    // (kTciTxTargetRate = 48 kHz) via the WDSP polyphase
    // float-vector resampler.  Fast-path when rate matches.
    // Earlier hard-drop on non-48 kHz was a Lyra divergence that
    // would have silently broken any TCI client streaming at a
    // non-48 kHz native rate (uncommon for digital-modes clients
    // — MSHV / JTDX / WSJT-X all use 48 kHz — but a divergence
    // from the reference for zero benefit).
    if (sampleRate != kTciTxTargetRate) {
        mono = resampleTxIn(mono, int(sampleRate), kTciTxTargetRate);
        if (mono.empty()) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                qWarning("[tci] TX_AUDIO_STREAM rate %u → %d resample "
                         "failed (WDSP resampler unavailable?)",
                         unsigned(sampleRate), kTciTxTargetRate);
            }
            return;
        }
    }

    // TCI TX-audio re-home (TX_ARCHITECTURAL_MAPPING.md §4.6 data model):
    // the decoded + gained (#108) + resampled-to-TXA-rate (#68) mono goes
    // into the TciTxBridge.  The verbatim cm_main TX pump drains it through
    // the InboundTCITxAudio seam (CMaster.cpp xcmaster case 1) when the
    // operator has selected TCI as the mic source (use_tci_audio); on the
    // wire only while MOX (sendProtocol1Samples zeroes outIQbufp at !XmitBit).
    lyra::tci::TciTxBridge::instance().pushMono(mono);
}

void TciServer::onChronoTick() {
    // Task #64 — TX_CHRONO outbound pump implementing the reference
    // dynamic-pull formula (cmaster.cs:1289-1359).  Only runs when
    // MOX is ON AND we have an active TX-audio owner.  Per tick:
    // compute predictedPacketSamples (the size of one inbound TX
    // audio frame after resampling to the TXA input rate), compute
    // targetQueuedSamples (the buffer depth we want to hold based
    // on the client's bufferingMs hint), compare against the
    // current queue depth + outstanding-requests * predicted size,
    // and emit requestsNeeded CHRONO requests this tick (bounded
    // by kTciTxMaxOutstanding).
    //
    // Replaces the earlier fixed "one request every 50 ms asking
    // for 2400 samples" pump which produced mis-paced TX audio on
    // the wire — MSHV's FT8 symbol cadence ended up wrong because
    // it was delivering audio at our cadence rather than its own
    // (the operator-bench-confirmed FT8 zero-spots symptom
    // 2026-06-01).
    if (!txAudioOwner_ || !stream_ || !stream_->moxActive()) return;
    if (txAudioOwner_->state() != QAbstractSocket::ConnectedState) {
        // Owner dropped without disconnect signal yet — release now.
        txAudioOwner_ = nullptr;
        chronoTimer_->stop();
        if (stream_) stream_->requestMoxFromTci(false);
        return;
    }

    // Timeout reset (cmaster.cs:1329-1330): if no audio inbound for
    // max(kChronoTimeoutMs, bufferingMs*4) ms while we have
    // outstanding requests, assume the client died on the audio
    // side and clear the counter so we resume pumping fresh
    // requests.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 timeoutMs = std::max<qint64>(kChronoTimeoutMs,
                                              qint64(bufferingMs_) * 4);
    if (chronoOutstanding_ > 0
        && (nowMs - chronoLastInboundMs_) > timeoutMs) {
        chronoOutstanding_ = 0;
    }

    // Pull the live client-negotiated values + Lyra constants.
    const int requestRate    = requestRate_    > 0 ? requestRate_
                                                   : kTciTxTargetRate;
    const int requestSamples = requestSamples_ > 0 ? requestSamples_
                                                   : kChronoFallbackRequestSamples;
    const int bufferingMs    = bufferingMs_    >= 50 ? bufferingMs_
                                                     : kChronoFallbackBufferingMs;
    const int targetRate     = kTciTxTargetRate;
    const int txBlock        = kTciTxBlockSamples;

    // predictedPacketSamples = the resampled size of one inbound
    // frame: ceil(requestSamples * targetRate / requestRate), with
    // a floor at txBlock so we never under-shoot the DSP block size.
    const int predictedPacketSamples = std::max(
        txBlock,
        int((qint64(requestSamples) * targetRate
             + qint64(requestRate) - 1) / std::max(1, requestRate)));

    // targetQueuedSamples = how many samples we want held in the
    // queue at all times: ceil((bufferingMs + extra) * targetRate /
    // 1000), floored at txBlock*4 so we never under-buffer.
    const int targetQueuedSamples = std::max(
        txBlock * 4,
        int((qint64(bufferingMs + kTciTxExtraBufferMs) * targetRate
             + 999) / 1000));

    // queuedSamples = current TX-mic-source inbound depth = the
    // TciTxBridge backlog the cm_main pump hasn't drained yet.  Feeds the
    // reference CHRONO dynamic-pull so the client is backpressured.
    const int queuedSamples = lyra::tci::TciTxBridge::instance().queuedSamples();

    // futureSamples = what we'll have after all outstanding requests
    // get answered with predictedPacketSamples each.
    const qint64 futureSamples =
        qint64(queuedSamples)
        + qint64(chronoOutstanding_) * qint64(predictedPacketSamples);

    // requestsNeeded = the gap, rounded up by predictedPacketSamples.
    int requestsNeeded = 0;
    if (futureSamples < targetQueuedSamples) {
        const qint64 gap = qint64(targetQueuedSamples) - futureSamples;
        requestsNeeded = int((gap + predictedPacketSamples - 1)
                             / predictedPacketSamples);
    }

    // Emit up to requestsNeeded CHRONO requests this tick, bounded
    // by kTciTxMaxOutstanding (reference TCI_TX_MAX_OUTSTANDING).
    //
    // Channels + length matches the reference's SendTxChrono
    // (TCIServer.cs:5515-5532):
    //   - channels field = negotiated requestChannels_ (1 or 2)
    //   - length = useModernLengthSemantics ? samples * channels
    //                                       : samples
    //
    // Modern flag flips true the moment a client sends
    // AUDIO_STREAM_CHANNELS or AUDIO_STREAM_SAMPLE_TYPE during
    // handshake (TCIServer.cs:5930 + 5946).  Legacy/JTDX-style
    // clients leave it false; they expect length = samples
    // scalars regardless of channels.  Both are protocol-correct.
    //
    // Earlier Lyra hardcoded channels=1 / length=samples
    // regardless of negotiation — for MSHV-class clients that
    // negotiated channels=2 this meant Lyra was asking MSHV for
    // half-sized response frames, which MSHV may interpret as a
    // "send me less" hint and reduce its TX-audio rate to match.
    const int chFld = (requestChannels_ == 2) ? 2 : 1;
    const quint32 lenFld =
        seenModernTxNeg_
            ? quint32(predictedPacketSamples * chFld)
            : quint32(predictedPacketSamples);
    while (requestsNeeded > 0
           && chronoOutstanding_ < kTciTxMaxOutstanding) {
        QByteArray hdr = streamHeader(0,
                                      /*rate=*/quint32(targetRate),
                                      /*fmt=*/FMT_FLOAT32,
                                      /*length=*/lenFld,
                                      /*type=*/STREAM_TX_CHRONO,
                                      /*channels=*/quint32(chFld));
        txAudioOwner_->sendBinaryMessage(hdr);
        ++chronoOutstanding_;
        chronoLastInboundMs_ = nowMs;
        --requestsNeeded;
    }
}

void TciServer::onMoxActiveChanged(bool on) {
    // Task #33 — SyncTciPttToMox (working reference at TCIServer.cs:
    // 5560-5577, called from the MOX-change emit sites at :6884/6899).
    // If the wire MOX bit dropped externally AND we owned the TX-audio
    // path AND that PTT came from us, release ownership cleanly —
    // stops the CHRONO pump + notifies the client.  No-op on MOX rise
    // (we don't grab ownership on a non-TCI keydown).
    if (on) return;
    if (!txAudioOwner_) return;
    if (!stream_) return;
    // Only release if the source that just released was Tci — an
    // operator-MOX or HW-PTT keydown that overlapped with a TCI
    // session should not auto-release the TCI listener.  In normal
    // operation those overlap windows are vanishingly rare (TCI
    // sessions are pure FT8 cycles with no operator-keying expected),
    // but the check is correctness for the corner case.
    if (stream_->pttSource() != lyra::ipc::HL2Stream::PttSource::Tci)
        return;
    QWebSocket *was = txAudioOwner_;
    txAudioOwner_ = nullptr;
    chronoOutstanding_ = 0;
    chronoTimer_->stop();
    emit statusMessage(QStringLiteral(
        "TCI: TX-audio ownership released (wire MOX dropped externally)"));
    if (was && was->state() == QAbstractSocket::ConnectedState) {
        // Inform the client so it stops pushing TX audio cleanly.
        sendTo(was, QStringLiteral("trx:0,false"));
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
    if (cwluBecomesCw_) list += QStringLiteral(",CW");   // legacy-client alias
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
    sendTo(ws, QStringLiteral("channels_count:1"));   // legacy plural spelling (compat)
    sendTo(ws, QStringLiteral("vfo_limits:%1,%2").arg(kVfoLo).arg(kVfoHi));
    sendTo(ws, QStringLiteral("if_limits:%1,%2").arg(-half).arg(half));
    sendTo(ws, QStringLiteral("modulations_list:") + modulationsList());
    // Audio-stream parameter advertisements — the reference sends
    // all five at connect (TCIServer.cs:2493-2496 plus
    // sendTxStreamAudioBuffering on TCI v2 handshake) so the
    // client knows the wire format up front instead of having to
    // query / infer from inspected binary-frame headers.  MSHV in
    // particular pre-configures its decoder from these values; a
    // mismatch with what Lyra actually emits results in mis-paced
    // audio interpretation (the operator-bench-confirmed hot-
    // waterfall symptom 2026-06-01).  Values match Lyra's actual
    // emit shape: 48 kHz float32 stereo (StreamCfg defaults
    // chans=2, fmt=3) at the matched packet cadence kAudio
    // PacketSamples = 2048 enforced by onTciAudioBlock.  TX-side
    // buffering hint matches MSHV's ms_settings default 50 ms.
    sendTo(ws, QStringLiteral("audio_samplerate:48000"));
    sendTo(ws, QStringLiteral("audio_stream_sample_type:float32"));
    sendTo(ws, QStringLiteral("audio_stream_channels:2"));
    sendTo(ws, QStringLiteral("audio_stream_samples:%1")
                   .arg(kAudioPacketSamples));
    sendTo(ws, QStringLiteral("tx_stream_audio_buffering:50"));
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
    // Task #33 commit 3.3 — ALWAYS-on dump of every inbound TCI text
    // command (NOT verbose-gated).  Operator-bench-feedback 2026-06-01:
    // the bench needs to know exactly what MSHV is emitting on TX TUNE
    // / FT8 cycle attempts WITHOUT the operator having to remember to
    // flip the verbose toggle first.  TCI text traffic is sparse (a
    // few commands per second at most during a TX cycle), so the
    // always-on cost is negligible and the diagnostic value is high.
    // Once the first-light bench is closed and the audio path is
    // working, this can move under the verbose gate.
    qInfo("[tci-rx-text] %s", qPrintable(msg));
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
    // #105 CW-4 — host CW keying over TCI.  Verified against the EESDR TCI
    // protocol spec (docs/TCI Protocol.pdf §3.2) + Thetis TCIServer.cs +
    // SDRLogger+ (hamlog/main.py).  Routes into the CW-3 host keyer; no-op
    // outside CW mode (HL2Stream::sendCw gates on txMode).  sendCw/abortCw
    // are marshalled to the stream thread (both Q_INVOKABLE) so the keyer
    // is only ever touched there — no race vs the QML console's sendCw.
    //
    // Reserved-char un-escape (spec §3.2.1): clients replace : , ; in macro
    // text with ^ ~ * so they don't break protocol framing — convert back.
    auto cwUnescape = [](QString s) {
        return s.replace(QLatin1Char('^'), QLatin1Char(':'))
                .replace(QLatin1Char('~'), QLatin1Char(','))
                .replace(QLatin1Char('*'), QLatin1Char(';'));
    };
    if (cmd == QStringLiteral("CW_MACROS_SPEED") ||
        cmd == QStringLiteral("CW_KEYER_SPEED")) {
        if (stream_ && !args.isEmpty()) {                 // Set
            bool ok = false; const int wpm = args.last().toInt(&ok);
            if (ok) stream_->setCwKeyerSpeedWpm(wpm);
        } else if (stream_) {                             // Read -> Reply (bidirectional)
            sendTo(ws, QStringLiteral("cw_macros_speed:%1")
                          .arg(stream_->cwKeyerSpeedWpm()));
        }
        return;
    }
    if (cmd == QStringLiteral("CW_MACROS_STOP")) {         // immediate stop (spec §3.2.2)
        if (stream_)
            QMetaObject::invokeMethod(stream_, "abortCw", Qt::QueuedConnection);
        return;
    }
    if (cmd == QStringLiteral("CW_MACROS")) {
        // cw_macros:<tcvr>,<text> — arg[0] = transceiver index (ignored).
        // Raw commas in text are escaped to ~, so the text is a single arg;
        // the join+unescape is robust regardless.  Inline | | prosign-combine
        // and < > speed-step are macro-text niceties (follow-on); plain text
        // keys correctly today (CwMorse skips unknown glyphs like | < >).
        if (stream_ && args.size() >= 2) {
            const QString text = cwUnescape(args.mid(1).join(QLatin1Char(',')));
            if (!text.isEmpty())
                QMetaObject::invokeMethod(stream_, "sendCw",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, text));
        }
        return;
    }
    if (cmd == QStringLiteral("CW_MSG")) {
        // cw_msg:<tcvr>,<prefix>,<callsign>,<suffix> (spec §3.2.2) — the
        // contest exchange; "$N" after the callsign repeats it N times.
        // The 1-arg callsign-edit form (cw_msg:<tcvr>;) + the CW_MSG-over-
        // CW_MACROS interrupt/priority semantics are a follow-on (this cut
        // assembles + sends the full message).
        if (stream_ && args.size() >= 4) {
            QString call = args[2];
            int rep = 1;
            const int dollar = call.indexOf(QLatin1Char('$'));
            if (dollar >= 0) {
                bool ok = false; const int n = call.mid(dollar + 1).toInt(&ok);
                if (ok && n > 0) rep = n;
                call = call.left(dollar);
            }
            QStringList parts;
            if (!args[1].isEmpty()) parts << cwUnescape(args[1]);      // prefix
            for (int i = 0; i < rep; ++i) parts << cwUnescape(call);   // callsign ×N
            if (!args[3].isEmpty()) parts << cwUnescape(args[3]);      // suffix
            const QString text = parts.join(QLatin1Char(' '));
            if (!text.isEmpty())
                QMetaObject::invokeMethod(stream_, "sendCw",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, text));
        }
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

    // ── Task #33 commit 3.4: TCI handshake-config commands MUST be
    //    echoed back so the client knows the SET took effect.  MSHV
    //    bench-discovered 2026-06-01: without `tx_enable:0,true`
    //    echo, MSHV considers TX-on-channel-0 not permitted and
    //    refuses to fire `trx:` for any operator press.  Working
    //    reference TCIServer.cs:5028-5050 has dedicated handlers for
    //    each of these — all echo the SET back to the client.
    //
    //    These commands are CONFIG (one-shot at handshake, sometimes
    //    re-queried later) — they don't trigger MOX or any wire
    //    activity.  Accept + echo + store the soft value.
    if (cmd == QStringLiteral("TX_ENABLE")
        || cmd == QStringLiteral("RX_ENABLE")) {
        bool okc = false;
        const int ch = args.size() >= 1 ? parseChannel(args[0], &okc) : -1;
        if (!okc || ch != 0) return;
        const bool on = args.size() >= 2 ? parseBool(args[1])
                                         : true;   // 1-arg query → default true
        softSet(cmd, on ? QStringLiteral("true") : QStringLiteral("false"));
        sendTo(ws, cmd.toLower()
            + QStringLiteral(":0,%1").arg(on ? QStringLiteral("true")
                                             : QStringLiteral("false")));
        return;
    }
    // Audio-stream config — echo back the operator-set value so MSHV
    // confirms the streaming parameters took effect AND capture the
    // value into the live state the CHRONO formula consumes (matches
    // the reference's TCIServer.cs:5019-5050 + the consuming side at
    // cmaster.cs:1289-1359 which reads requestRate / requestSamples
    // / bufferingMs every tick).  Without this, the formula sat on
    // stale defaults and Lyra's CHRONO pump asked MSHV for a wrong
    // number of TX-audio samples per tick.
    if (cmd == QStringLiteral("AUDIO_SAMPLERATE")
        || cmd == QStringLiteral("AUDIO_STREAM_SAMPLES")
        || cmd == QStringLiteral("AUDIO_STREAM_CHANNELS")
        || cmd == QStringLiteral("AUDIO_STREAM_SAMPLE_TYPE")
        || cmd == QStringLiteral("TX_STREAM_AUDIO_BUFFERING")) {
        const QString val = args.isEmpty() ? QStringLiteral("0") : args[0];
        bool okv = false;
        const int n = val.trimmed().toInt(&okv);
        if (okv && n > 0) {
            if (cmd == QStringLiteral("AUDIO_SAMPLERATE")) {
                requestRate_ = n;
            } else if (cmd == QStringLiteral("AUDIO_STREAM_SAMPLES")) {
                requestSamples_ = n;
            } else if (cmd == QStringLiteral("AUDIO_STREAM_CHANNELS")) {
                // Reference clamps to {1, 2} (TCIServer.cs:5942-5943).
                if (n == 1 || n == 2)
                    requestChannels_ = n;
                seenModernTxNeg_ = true;
            } else if (cmd == QStringLiteral("AUDIO_STREAM_SAMPLE_TYPE")) {
                // Sample-type negotiation also flips the modern flag
                // per TCIServer.cs:5930.  Stored value handled by the
                // softSet echo path; we just need the flag here.
                seenModernTxNeg_ = true;
            } else { // TX_STREAM_AUDIO_BUFFERING
                bufferingMs_ = std::max(n, 50);   // reference floor
            }
        }
        softSet(cmd, val);
        sendTo(ws, cmd.toLower() + QStringLiteral(":%1").arg(val));
        return;
    }
    // TUNE — TCI v1.9/v2 separate TX path for tune-carrier (operator
    // hits a "TUNE" button in the client).  Working reference's
    // handleTune (TCIServer.cs:4343-4364) sets a TUN flag distinct
    // from MOX, but Lyra doesn't yet have a separate tune-carrier
    // mode — treat tune like trx for MOX engagement so MSHV's TUNE
    // button keys the rig.  Future v0.2.x: wire a real tune-carrier
    // generator (Task #50/#52 chain) for proper TUN behaviour.
    if (cmd == QStringLiteral("TUNE")) {
        bool okc = false;
        const int ch = args.size() >= 1 ? parseChannel(args[0], &okc) : -1;
        if (!okc || ch != 0 || !stream_) {
            sendTo(ws, QStringLiteral("tune:0,false"));
            return;
        }
        if (args.size() < 2) {
            // 1-arg query — report current state (we don't track a
            // separate TUN flag yet; report based on wire MOX).
            sendTo(ws, QStringLiteral("tune:0,%1").arg(
                stream_->moxActive() ? QStringLiteral("true")
                                     : QStringLiteral("false")));
            return;
        }
        const bool wantsTune = parseBool(args[1]);
        if (wantsTune) {
            // Acquire TCI audio ownership IF operator has Mic source =
            // tci (so the client's audio reaches the WDSP TXA chain;
            // matches the trx:0,true behaviour below).
            const bool wantTciAudio =
                prefs_ && prefs_->micSource() == QStringLiteral("tci");
            if (wantTciAudio) {
                if (txAudioOwner_ != nullptr && txAudioOwner_ != ws) {
                    sendTo(ws, QStringLiteral("tune:0,false"));
                    return;
                }
                if (txAudioOwner_ == nullptr) {
                    txAudioOwner_ = ws;
                    chronoOutstanding_ = 0;
                    chronoLastInboundMs_ =
                        QDateTime::currentMSecsSinceEpoch();
                    chronoTimer_->start();
                    emit statusMessage(QStringLiteral(
                        "TCI: TX-audio ownership ACQUIRED (tune)"));
                }
            }
            stream_->requestMoxFromTci(true);
            sendTo(ws, QStringLiteral("tune:0,true"));
        } else {
            if (txAudioOwner_ == ws) {
                txAudioOwner_ = nullptr;
                chronoOutstanding_ = 0;
                chronoTimer_->stop();
                emit statusMessage(QStringLiteral(
                    "TCI: TX-audio ownership RELEASED (tune)"));
            }
            stream_->requestMoxFromTci(false);
            sendTo(ws, QStringLiteral("tune:0,false"));
        }
        return;
    }

    // ── Task #33: TRX — TCI keying funnel (working reference's
    //    handleTrxMessage at TCIServer.cs:3459-3550).  Wired ONLY when
    //    the request comes with the "tci" source token (per TCI v2
    //    §3.3 TRX command).  Other forms (operator-tooling MOX requests
    //    via TRX without source, TUNE / RIT_ENABLE / XIT_ENABLE /
    //    SPLIT_ENABLE) keep the v0.2.2 acknowledge-inactive stub.
    if (cmd == QStringLiteral("TRX")) {
        bool okc = false; const int ch = args.size() >= 1 ? parseChannel(args[0], &okc) : -1;
        const bool wantsTx = args.size() >= 2 && parseBool(args[1]);
        const bool useTciAudio = args.size() >= 3
                              && args[2].compare(QStringLiteral("tci"),
                                                 Qt::CaseInsensitive) == 0;
        if (!okc || ch != 0 || !stream_) {
            const QString idx = args.isEmpty() ? QStringLiteral("0") : args[0];
            sendTo(ws, QStringLiteral("trx:%1,false").arg(idx));
            return;
        }
        if (wantsTx) {
            // Thetis-faithful: ALWAYS key MOX on trx:0,true regardless
            // of source token.  Working reference TCIServer.cs:3536-3537
            // sets TCIPTT = bMox unconditionally; the source token
            // (tci / mic1 / nothing) controls ONLY audio routing.
            // Earlier commit-3 incorrectly filtered MOX on
            // useTciAudio — MSHV (and other TCI clients) send plain
            // `trx:0,true` without the source token and expect the
            // radio to key anyway.  Operator-bench-confirmed
            // 2026-06-01 ("MSHV keys Thetis").
            //
            // Acquire TCI audio ownership if EITHER:
            //   (a) source token explicitly says "tci" (auto-detected
            //       digital-modes client), OR
            //   (b) operator has Mic source = "tci" in Settings
            //       (operator opted in via the picker — covers bare
            //       trx:0,true from clients that don't include the
            //       source token).
            const bool wantTciAudio = useTciAudio
                || (prefs_ && prefs_->micSource()
                                  == QStringLiteral("tci"));
            if (wantTciAudio) {
                if (txAudioOwner_ != nullptr && txAudioOwner_ != ws) {
                    emit statusMessage(QStringLiteral(
                        "TCI: TX-audio acquire denied — another client "
                        "owns TX audio"));
                    sendTo(ws, QStringLiteral("trx:0,false"));
                    return;
                }
                if (txAudioOwner_ == nullptr) {
                    txAudioOwner_ = ws;
                    chronoOutstanding_ = 0;
                    chronoLastInboundMs_ =
                        QDateTime::currentMSecsSinceEpoch();
                    chronoTimer_->start();
                    emit statusMessage(QStringLiteral(
                        "TCI: TX-audio ownership ACQUIRED"));
                }
                // Auto-flip the operator picker ONLY on the explicit
                // ",tci" token — bare trx:0,true with operator-set
                // Mic source=tci already matches, and we don't want
                // to flip the picker on a generic TCI-client keying
                // request that didn't specifically ask for TCI audio.
                if (useTciAudio && prefs_)
                    prefs_->setMicSource(QStringLiteral("tci"));
            }
            // R-H2 — diagnostic snapshot + token-agnostic source force.
            //
            // Mechanism guarded against: bare `trx:0,true` from MSHV /
            // JTDX / WSJT-X (no `,tci` token) WITH operator's
            // Prefs::micSource != "tci" produces:
            //   wantTciAudio = false → no audio ownership → no picker
            //   flip → activeMicSource_ stays at its previous value →
            //   TXA chain runs with zero audio input → SSB modulator
            //   outputs only the suppressed-carrier residue (a single
            //   tone at the bp0 passband center, visible as one bright
            //   vertical line on the panadapter and zero PSKReporter
            //   spots because there is no FT8 content in the I/Q).
            //
            // Fix: when ANY TCI client keys via trx:0,true (token-
            // agnostic), force activeMicSource_ to Tci for the
            // keydown lifetime so TCI binary-frame samples reach the
            // TXA chain.  Operator's persistent Prefs::micSource() is
            // NOT mutated (the picker UI does not flicker / does not
            // get rewritten); only the worker's runtime atomic is
            // forced, restored on keyup or owner disconnect.
            //
            // The unconditional statusMessage records the live state
            // at every keydown so the operator can verify post-bench
            // whether the force fired (and whether the original was
            // already Tci, in which case R-H2 is a no-op and the
            // diagnosis must descend to the next reconciled
            // deviation).  See docs/THETIS_VS_LYRA_RECONCILED.md R-H2.
            {
                // TX-rip Phase 1 (Q2): R-H2 activeMicSource_ readout +
                // force-to-Tci returns with the new TX DSP worker per
                // docs/TX_ARCHITECTURAL_MAPPING.md §10.3.
                const QString pickerStr = prefs_ ? prefs_->micSource()
                                                 : QStringLiteral("(null)");
                emit statusMessage(QString(
                    "TCI: keydown — prefs.micSource=%1 useTciAudio=%2 "
                    "(TX subsystem rebuilding)")
                        .arg(pickerStr)
                        .arg(useTciAudio ? QStringLiteral("1")
                                         : QStringLiteral("0")));
            }
            // Key MOX (Thetis-faithful) — records PttSource::Tci on
            // the FSM since the request arrived via the TCI WebSocket.
            stream_->requestMoxFromTci(true);
            sendTo(ws, useTciAudio
                       ? QStringLiteral("trx:0,true,tci")
                       : QStringLiteral("trx:0,true"));
            return;
        }
        // wantsTx == false — release MOX + ownership if held.
        // Working reference parallel: clearQueuedTxAudio + Release
        // ActiveTxAudioListener at TCIServer.cs:3479-3486 + 5578-5585.
        // The binary handler gates on owner identity so in-flight
        // TX_AUDIO_STREAM frames are discarded the moment ownership
        // clears (Lyra's clearQueuedTxAudio equivalent).
        if (txAudioOwner_ == ws) {
            txAudioOwner_ = nullptr;
            chronoOutstanding_ = 0;
            chronoTimer_->stop();
            emit statusMessage(
                QStringLiteral("TCI: TX-audio ownership RELEASED"));
        }
        // TX-rip Phase 1 (Q2): R-H2 keyup-restore returns with the
        // new TX DSP worker per docs/TX_ARCHITECTURAL_MAPPING.md §10.3.
        stream_->requestMoxFromTci(false);
        sendTo(ws, QStringLiteral("trx:0,false"));
        return;
    }
    // ── Acknowledge other TX-side queries as inactive (TUNE moved
    //    above into commit 3.4's real handler).
    if (cmd == QStringLiteral("RIT_ENABLE") || cmd == QStringLiteral("XIT_ENABLE")
        || cmd == QStringLiteral("SPLIT_ENABLE")) {
        const QString idx = args.isEmpty() ? QStringLiteral("0") : args[0];
        sendTo(ws, cmd.toLower() + QStringLiteral(":%1,false").arg(idx));
        return;
    }
    // TX-only setters with no read-back of interest — silently accept.
    // (TX_STREAM_AUDIO_BUFFERING moved to commit 3.4's echo path so
    // MSHV / Thetis-client gets confirmation the buffering value
    // applied.)
    if (cmd == QStringLiteral("DRIVE") || cmd == QStringLiteral("TUNE_DRIVE")
        || cmd == QStringLiteral("MON_ENABLE") || cmd == QStringLiteral("MON_VOLUME")
        || cmd == QStringLiteral("RIT_OFFSET") || cmd == QStringLiteral("XIT_OFFSET")
        || cmd == QStringLiteral("KEYER")
        || cmd.startsWith(QStringLiteral("CW_MACROS"))
        || cmd == QStringLiteral("CW_MSG") || cmd == QStringLiteral("CW_TERMINAL")
        || cmd == QStringLiteral("CW_KEYER_SPEED"))
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
    if (!sensorsEnabled_ || clients_.isEmpty() || !engine_) return;
    // Reference-faithful S-meter source: WDSP RXA_S_PK polled at the
    // operator-UI cadence (matches reference's GetRXAMeter path via
    // wdsp_engine.h:80 audioDbFs Q_PROPERTY).  Pre-Stage-2b this read
    // came off a Lyra-native pre-WDSP raw-IQ RMS accumulator on
    // HL2Stream (rx1DbFs); that accumulator is deleted as part of
    // Stage 2b strict-reference strip-out — reference has no
    // pre-DSP instrument here, only the WDSP-derived meter.  The
    // dBm offset (-30) carries forward unchanged as a coarse stopgap
    // until per-band cal lands; behavioural note: audioDbFs is
    // post-AGC, so the TCI client sees the AGC-shaped meter (=
    // exactly what reference's S-meter does, since reference shows
    // the same WDSP-derived reading).
    const double dbfs = engine_->audioDbFs();
    const double dbm = (dbfs <= -199.0) ? -140.0 : dbfs - 30.0;
    broadcast(QStringLiteral("rx_channel_sensors:0,0"),
              QStringLiteral("rx_channel_sensors:0,0,%1")
                  .arg(QString::number(dbm, 'f', 1)));
}

} // namespace lyra::ui
