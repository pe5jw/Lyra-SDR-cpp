// Lyra-cpp — IvacAudio implementation (#158 Stage 2).
//
// The Qt device layer for one IVAC instance.  See ivac_audio.h for the
// architecture; the engine half (wire/Ivac, the two rmatchV rings, the
// AAMix) is Stage 1 and untouched here.  Stage 2 is INERT on the radio:
// the render/capture paths move samples between Qt devices and the
// engine's rings, but nothing taps RX/TX yet (Stage 3/4 wire them in).

#include "ivac_audio.h"

#include "wire/Ivac.h"
#include "wire/wdspcalls.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QDebug>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lyra::ipc {

// ---------------------------------------------------------------------------
// Render pull device — the QAudioSink pulls from this in its own audio
// thread.  readData drains the engine OUT ring into the sink; mirrors the
// RX AudioRing idiom (wdsp_engine.cpp), but pull-only (writeData fails).
// Lives in the .cpp because the concrete type is private to IvacAudio.
// ---------------------------------------------------------------------------
namespace {

class IvacRenderDevice : public QIODevice {
public:
    explicit IvacRenderDevice(IvacAudio *owner) : owner_(owner) {}

    bool isSequential() const override { return true; }

    // Keep the sink pulling: readData always serves the full request, so a
    // generous constant is enough (the ring pads silence on underrun).
    qint64 bytesAvailable() const override {
        return qint64(1) << 18 ;  // 256 KiB headroom
    }

protected:
    qint64 readData(char *data, qint64 maxlen) override {
        const int frameBytes = 2 * int(sizeof(qint16));   // stereo int16
        const int maxFrames = int(maxlen / frameBytes);
        if (maxFrames <= 0)
            return 0;
        owner_->pullRenderInt16(reinterpret_cast<qint16 *>(data), maxFrames);
        return qint64(maxFrames) * frameBytes;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    IvacAudio *owner_;
};

inline qint16 clampToInt16(double v) {
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return static_cast<qint16>(std::lrint(v));
}

}  // namespace

// ---------------------------------------------------------------------------

IvacAudio::IvacAudio(int id, QObject *parent) : QObject(parent), id_(id) {}

IvacAudio::~IvacAudio() {
    stop();
}

QStringList IvacAudio::outputDevices() {
    QStringList out;
    const auto devs = QMediaDevices::audioOutputs();
    for (const auto &d : devs)
        out << d.description();
    return out;
}

QStringList IvacAudio::inputDevices() {
    QStringList in;
    const auto devs = QMediaDevices::audioInputs();
    for (const auto &d : devs)
        in << d.description();
    return in;
}

void IvacAudio::cacheEngineSizes() {
    using namespace lyra::wire;
    IVAC a = ivacGet(id_);
    if (!a)
        return;
    vacSize_ = a->vac_size;
    vacRate_ = a->vac_rate;
}

bool IvacAudio::start(int outputIdx, int inputIdx) {
    using namespace lyra::wire;
    IVAC a = ivacGet(id_);
    if (!a) {
        qWarning() << "IvacAudio::start — no engine instance for id" << id_
                   << "(create_ivac must run first)";
        return false;
    }

    stop();  // idempotent — never double-open
    cacheEngineSizes();
    if (vacSize_ <= 0 || vacRate_ <= 0) {
        qWarning() << "IvacAudio::start — bad engine vac_size/vac_rate"
                   << vacSize_ << vacRate_;
        return false;
    }

    renderDbuf_.assign(static_cast<std::size_t>(2 * vacSize_), 0.0);
    stage_.assign(static_cast<std::size_t>(2 * vacSize_), 0);
    stageFrames_ = 0;
    stageRd_ = 0;
    capAccum_.clear();

    QAudioFormat fmt;
    fmt.setSampleRate(vacRate_);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);

    // VAC-out (radio RX -> PC output device).
    if (outputIdx >= 0) {
        const auto outs = QMediaDevices::audioOutputs();
        if (outputIdx < outs.size() && outs[outputIdx].isFormatSupported(fmt)) {
            renderDev_ = new IvacRenderDevice(this);
            renderDev_->open(QIODevice::ReadOnly);
            sink_ = new QAudioSink(outs[outputIdx], fmt, this);
            sink_->start(renderDev_);
        } else {
            qWarning() << "IvacAudio::start — output device" << outputIdx
                       << "unavailable or won't accept Int16/stereo/"
                       << vacRate_;
        }
    }

    // VAC-in (PC input device -> radio TX).
    if (inputIdx >= 0) {
        const auto ins = QMediaDevices::audioInputs();
        if (inputIdx < ins.size() && ins[inputIdx].isFormatSupported(fmt)) {
            source_ = new QAudioSource(ins[inputIdx], fmt, this);
            captureDev_ = source_->start();  // pull mode: returns the QIODevice
            if (captureDev_) {
                connect(captureDev_, &QIODevice::readyRead, this,
                        [this] { onCaptureReady(); });
            } else {
                qWarning() << "IvacAudio::start — QAudioSource::start returned null";
                delete source_;
                source_ = nullptr;
            }
        } else {
            qWarning() << "IvacAudio::start — input device" << inputIdx
                       << "unavailable or won't accept Int16/stereo/"
                       << vacRate_;
        }
    }

    return sink_ != nullptr || source_ != nullptr;
}

void IvacAudio::stop() {
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    if (renderDev_) {
        renderDev_->close();
        delete renderDev_;
        renderDev_ = nullptr;
    }
    if (source_) {
        source_->stop();
        delete source_;       // also deletes captureDev_ (owned by source_)
        source_ = nullptr;
    }
    captureDev_ = nullptr;
}

// VAC-out core: drain the engine OUT ring (rmatchOUT) into the sink's int16
// buffer, refilling one vac_size block at a time.  Always fills maxFrames —
// rmatchV pads its own ring on underrun, so the sink never stalls.
qint64 IvacAudio::pullRenderInt16(qint16 *out, int maxFrames) {
    using namespace lyra::wire;
    IVAC a = ivacGet(id_);
    if (!a || vacSize_ <= 0) {
        std::memset(out, 0,
                    static_cast<std::size_t>(maxFrames) * 2 * sizeof(qint16));
        return maxFrames;
    }

    int produced = 0;
    while (produced < maxFrames) {
        if (stageRd_ >= stageFrames_) {
            // Refill: pull one vac_size block of doubles from rmatchOUT.
            xrmatchOUT(a->rmatchOUT, renderDbuf_.data());
            const int n = 2 * vacSize_;
            for (int i = 0; i < n; ++i)
                stage_[static_cast<std::size_t>(i)] =
                    clampToInt16(renderDbuf_[static_cast<std::size_t>(i)] * 32767.0);
            stageFrames_ = vacSize_;
            stageRd_ = 0;
        }
        const int take = std::min(maxFrames - produced, stageFrames_ - stageRd_);
        std::memcpy(out + static_cast<std::size_t>(produced) * 2,
                    stage_.data() + static_cast<std::size_t>(stageRd_) * 2,
                    static_cast<std::size_t>(take) * 2 * sizeof(qint16));
        produced += take;
        stageRd_ += take;
    }
    return maxFrames;
}

// VAC-in core: convert captured stereo int16 to double, accumulate to
// vac_size, and push each full block into the engine IN ring (rmatchIN).
void IvacAudio::pushCaptureInt16(const qint16 *in, int nframes) {
    using namespace lyra::wire;
    IVAC a = ivacGet(id_);
    if (!a || vacSize_ <= 0)
        return;

    // #158 diag — prove the QAudioSource capture is actually delivering
    // samples to the IN ring (readyRead firing) and at what level.  peak=0
    // with a moving Windows recording meter => Qt isn't getting the audio;
    // no line at all => readyRead never fires.  Rate-limited ~1/s, INFO so
    // it lands in lyra-log.txt without debug logging.
    {
        static long long capFrames = 0;
        static int       capPeak   = 0;
        for (int f = 0; f < nframes; ++f) {
            int l = in[f * 2 + 0]; if (l < 0) l = -l;
            int r = in[f * 2 + 1]; if (r < 0) r = -r;
            if (l > capPeak) capPeak = l;
            if (r > capPeak) capPeak = r;
        }
        capFrames += nframes;
        if (capFrames >= vacRate_) {
            qInfo("[vac1] capture: %lld frames/s, peak %d/32767 (%.1f%%)",
                  capFrames, capPeak, capPeak * 100.0 / 32767.0);
            capFrames = 0; capPeak = 0;
        }
    }

    for (int f = 0; f < nframes; ++f) {
        capAccum_.push_back(in[f * 2 + 0] / 32768.0);
        capAccum_.push_back(in[f * 2 + 1] / 32768.0);
    }

    const std::size_t blockDoubles = static_cast<std::size_t>(2 * vacSize_);
    std::size_t consumed = 0;
    while (capAccum_.size() - consumed >= blockDoubles) {
        xrmatchIN(a->rmatchIN, capAccum_.data() + consumed);
        consumed += blockDoubles;
    }
    if (consumed > 0)
        capAccum_.erase(capAccum_.begin(),
                        capAccum_.begin() + static_cast<std::ptrdiff_t>(consumed));
}

void IvacAudio::onCaptureReady() {
    if (!captureDev_)
        return;
    const QByteArray b = captureDev_->readAll();
    const int frameBytes = 2 * int(sizeof(qint16));
    const int nframes = int(b.size()) / frameBytes;
    if (nframes > 0)
        pushCaptureInt16(reinterpret_cast<const qint16 *>(b.constData()), nframes);
}

}  // namespace lyra::ipc
