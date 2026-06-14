// Lyra-cpp — IvacAudio: the Qt-Multimedia device-I/O layer for one IVAC
// instance (VAC1 = id 0, VAC2 = id 1).  #158 Stage 2.
//
// This is the Lyra-native replacement for the Thetis ivac.c PortAudio
// device layer (CallbackIVAC / StartAudioIVAC / StopAudioIVAC), which
// was deliberately NOT ported in Stage 1.  Lyra is Qt-native and already
// drives RX audio out through QAudioSink (wdsp_engine.cpp AudioRing), so
// VAC uses the same idiom:
//
//   * VAC-OUT (radio RX audio -> PC, so a digital app HEARS the radio):
//     a render QIODevice the QAudioSink pulls; readData drains the IVAC
//     OUT ring via xrmatchOUT(rmatchOUT,...) in vac_size blocks, converts
//     double->int16, and serves the sink.  Target = a PC OUTPUT device
//     (the VAC playback endpoint, e.g. "CABLE Input").
//
//   * VAC-IN (PC audio -> radio TX, so a digital app KEYS/feeds the
//     radio): a QAudioSource captures int16, converts to double, and
//     pushes the IVAC IN ring via xrmatchIN(rmatchIN,...) in vac_size
//     blocks.  Source = a PC INPUT device (the VAC recording endpoint,
//     e.g. "CABLE Output").
//
// The two directions are independent (each has its own rmatchV ring doing
// its own two-clock drift correction), so two separate Qt devices is
// clean.  Kept OUT of wire/ so Qt never enters the C-style ChannelMaster
// layer.  Stage 2 is INERT on the radio: the rings exist but nothing
// taps RX/TX yet (Stage 3/4).  Format is Int16 stereo at vac_rate;
// mono-only capture devices are a documented Stage-2 limitation (the
// reference dups mono in its callback — a fast-follow if a tester hits a
// mono-only VAC; VB-Cable et al. are stereo).
//
// PureSignal is unaffected by this choice — PS is a wire/IQ feedback
// channel with zero audio-device coupling (verified; IVAC_PORT_PLAN.md).

#pragma once

#include <QObject>
#include <QStringList>
#include <QtGlobal>
#include <vector>

class QAudioSink;
class QAudioSource;
class QIODevice;

namespace lyra::ipc {

class IvacAudio : public QObject {
    Q_OBJECT
public:
    // <id> selects the wire/Ivac engine instance (must already exist via
    // create_ivac).  VAC1 = 0, VAC2 = 1.
    explicit IvacAudio(int id, QObject *parent = nullptr);
    ~IvacAudio() override;

    // Device descriptions, index-aligned with start()'s arguments.
    // outputs() = PC playback endpoints (VAC-out target);
    // inputs()  = PC recording endpoints (VAC-in source).
    static QStringList outputDevices();
    static QStringList inputDevices();

    // Open the Qt audio for this VAC.  outputIdx -> outputDevices()
    // (VAC-out), inputIdx -> inputDevices() (VAC-in); pass -1 to disable
    // that direction.  Returns true if at least one direction opened.
    bool start(int outputIdx, int inputIdx);
    void stop();
    bool running() const { return sink_ != nullptr || source_ != nullptr; }

    // --- core converters (also unit-tested directly, no real device) ---
    // VAC-out: drain the engine OUT ring (rmatchOUT) -> interleaved
    // stereo int16.  Always fills maxFrames (rmatchV pads on underrun).
    // Runs on the QAudioSink's audio thread.
    qint64 pullRenderInt16(qint16 *out, int maxFrames);
    // VAC-in: interleaved stereo int16 -> double -> engine IN ring
    // (rmatchIN), pushed in vac_size blocks.  Runs on the QAudioSource's
    // delivery thread.
    void pushCaptureInt16(const qint16 *in, int nframes);

private:
    void onCaptureReady();
    void cacheEngineSizes();   // refresh vacSize_/vacRate_ from the engine

    int id_;
    int vacSize_ = 0;
    int vacRate_ = 48000;

    QAudioSink   *sink_       = nullptr;   // VAC-out
    QIODevice    *renderDev_  = nullptr;   // pull source for sink_
    QAudioSource *source_     = nullptr;   // VAC-in
    QIODevice    *captureDev_ = nullptr;   // source_->start() QIODevice

    std::vector<double> renderDbuf_;   // 2*vacSize scratch for xrmatchOUT
    std::vector<qint16> stage_;        // 2*vacSize int16 render staging
    int stageFrames_ = 0;              // frames currently in stage_
    int stageRd_     = 0;              // frame read cursor in stage_
    std::vector<double> capAccum_;     // VAC-in accumulation (interleaved)
};

}  // namespace lyra::ipc
