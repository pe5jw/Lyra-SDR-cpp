// Lyra — WDSP RX channel engine (Step 3c-ii).
//
// Wraps a single WDSP receiver channel: OpenChannel + the locked
// first-light config (USB 200-3000 Hz, AGC MED, binaural mono) +
// SetChannelState start, with a matching close on teardown.
//
// Scope of Step 3c-ii is CHANNEL LIFECYCLE ONLY — open the channel,
// configure it, start it, prove it opens + closes without crashing.
// No IQ flows through fexchange0 yet (that's Step 3d), no audio is
// produced yet (Step 3e).
//
// Every parameter here is mirrored from the bench-proven Python tree
// (lyra/dsp/wdsp_engine.py RxConfig + RxChannel._open) so the C++
// rebuild starts from a known-good WDSP setup rather than re-deriving
// it.  See CLAUDE.md §14.2 for the load-bearing gotchas:
//   * OpenChannel 13th arg (block) MUST be 1 (block-until-output).
//   * out_size = in_size * out_rate / in_rate (NOT in_size).
//   * Sideband select lives in NBP0 — use RXASetPassband, not
//     SetRXABandpassFreqs (BP1 is bypassed with all DSP off).
//   * SetRXAPanelBinaural(ch, 0) => panel.copy=1 => mono on both
//     L/R; fixes the AM/FM/DSB right-channel-silent bug (§14.10).

#pragma once

#include "wdsp_native.h"

#include <QAudioDevice>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

class QAudioSink;

namespace lyra::dsp {

// Defined in wdsp_engine.cpp — a QIODevice the QAudioSink pulls audio
// from, backed by a mutex-protected stereo int16 ring.
class AudioRing;

// Per-channel sample rates + buffer size.  Defaults match the working
// Thetis/Lyra HL2 setup: 1024-frame 192 kHz IQ in, 4096-sample
// internal DSP buffer at 48 kHz, 48 kHz audio out.
struct RxConfig {
    int    inSize     = 1024;     // frames per fexchange0 call
    int    dspSize    = 4096;     // internal DSP buffer size
    int    inRate     = 192000;   // IQ input rate (HL2 default)
    int    dspRate    = 48000;    // WDSP internal DSP rate
    int    outRate    = 48000;    // audio output rate (AK4951 fixed)
    // Slew envelope (avoids click on start/stop).
    double tDelayUp   = 0.010;
    double tSlewUp    = 0.025;
    double tDelayDown = 0.000;
    double tSlewDown  = 0.010;
    // 1 = fexchange0 blocks until the DSP thread has produced the next
    // output buffer.  Required for a steady cadence (§14.2).
    int    block      = 1;
};

class WdspEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool   running   READ isRunning  NOTIFY runningChanged)
    // Step 3d: peak audio level out of the DSP chain, dBFS.  Sentinel
    // -200.0 = "no audio produced yet".  Written by the RX worker
    // thread in feedIq(); sampled by the UI at ~5 Hz via levelsChanged.
    Q_PROPERTY(double audioDbFs READ audioDbFs  NOTIFY levelsChanged)
    // Step 3e: operator audio controls.  SAFETY: starts MUTED at a low
    // default volume so the first listen can never be a full-scale
    // blast (operator-reported speaker damage in the Python tree).
    // volume is the SLIDER POSITION (0..1); the actual gain is a
    // perceptual dB taper of it (see wdsp_engine.cpp).  volumeDb is
    // that position expressed in dB, for the UI readout.
    Q_PROPERTY(double volume      READ volume      NOTIFY volumeChanged)
    Q_PROPERTY(double volumeDb    READ volumeDb    NOTIFY volumeChanged)
    Q_PROPERTY(bool   muted       READ muted       NOTIFY mutedChanged)
    Q_PROPERTY(int    audioDeviceIndex READ audioDeviceIndex NOTIFY audioDeviceChanged)
    // Panadapter frequency span (Hz) = the IQ sample rate DIVIDED BY the
    // zoom factor — the displayed bandwidth, centred on the RX1 DDC freq.
    // Used by the QML frequency scale / drag-pan / cursor readout, which
    // all track zoom for free because the displayed span shrinks as zoom
    // rises.  NOTIFY spanChanged (emitted on a rate OR a zoom change).
    Q_PROPERTY(int    spanHz READ spanHz NOTIFY spanChanged)
    // Panadapter zoom: 1.0 = full IQ span; higher magnifies the band
    // CENTRE.  Done old-Lyra-style — the analyzer always produces a
    // full-span, full-resolution spectrum; copySpectrum crops the centre
    // 1/zoom slice and resamples it to display width.  No live analyzer
    // reconfiguration (that corrupts the trace), pure display-side crop.
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    // Demod mode (USB/LSB/CWU/CWL/DSB/AM/FM/DIGU/DIGL) + RX filter
    // bandwidth (Hz).  setMode/setBandwidth drive SetRXAMode +
    // RXASetPassband live, using old Lyra's per-mode passband sign rules
    // (sideband-correct against the HL2 mirrored baseband).
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(int bandwidth READ bandwidth WRITE setBandwidth
               NOTIFY bandwidthChanged)
    // Current RX filter passband edges (Hz OFFSET from the tuned centre)
    // for the panadapter overlay — derived from mode + bandwidth (+ CW
    // pitch) by the same rules applyModeFilter pushes to WDSP.
    Q_PROPERTY(double passbandLowHz  READ passbandLowHz  NOTIFY passbandChanged)
    Q_PROPERTY(double passbandHighHz READ passbandHighHz NOTIFY passbandChanged)
    // CW tone pitch (Hz).  In CW the displayed VFO is the signal CARRIER;
    // the DDS is offset by ±pitch so the carrier lands in the pitch-
    // centred filter (old-Lyra / Thetis convention).
    Q_PROPERTY(int cwPitchHz READ cwPitchHz WRITE setCwPitchHz
               NOTIFY cwPitchChanged)
    // VFO − DDS, Hz: +pitch in CWU, −pitch in CWL, 0 otherwise.  The
    // tuning layer uses it (VFO = DDS + markerOffset) and the panadapter
    // draws the carrier marker offset by it.
    Q_PROPERTY(int markerOffsetHz READ markerOffsetHz NOTIFY markerOffsetChanged)

public:
    explicit WdspEngine(WdspNative *wdsp, QObject *parent = nullptr);
    ~WdspEngine() override;

    bool isRunning() const { return running_; }

    double audioDbFs() const {
        return audioDbFs_.load(std::memory_order_relaxed);
    }

    // In-passband RX signal strength (WDSP RXA_S_PK), in WDSP's raw
    // dBm-ish units — the same source Thetis reads for its S-meter.
    // Returns -200 when the RX channel isn't running.  Safe to call
    // from the UI thread (just reads WDSP's latest stored meter value).
    double sMeterDbm() const;

    double volume() const { return volume_.load(std::memory_order_relaxed); }
    double volumeDb() const;   // slider position -> dB (for UI readout)
    bool   muted()  const { return muted_.load(std::memory_order_relaxed); }
    // Output-list index: 0 = HL2 audio jack, 1..N = PC devices.
    int    audioDeviceIndex() const {
        return hl2Out_ ? 0 : deviceIndex_ + 1;
    }
    double zoom() const { return zoom_.load(std::memory_order_relaxed); }
    int    spanHz() const {
        const double z = zoom_.load(std::memory_order_relaxed);
        return static_cast<int>(cfg_.inRate / (z > 1.0 ? z : 1.0));
    }

    // Frames fexchange0 writes per process() call (= in_size *
    // out_rate / in_rate).  Step 3d sizes its output buffer to this.
    int outSize() const { return outSize_; }

    // Step 5: WDSP spectral analyzer (panadapter source).  The IQ fed
    // to the audio chain is also fed to the analyzer; copySpectrum
    // pulls the latest display-width dB array (called from the
    // panadapter's render thread — WDSP serialises feed-vs-read
    // internally, as Thetis relies on).  Plain C++ (not Q_INVOKABLE):
    // the panadapter is a C++ QQuickPaintedItem, not QML JS.
    int  spectrumPixelCount() const;
    int  copySpectrum(float *dst, int maxN);

    // Step 3e operator audio controls (call from the UI / main thread).
    // setVolume: linear gain 0.0..1.0 applied before int16 conversion.
    // setMuted:  hard mute (gain 0) without losing the volume setting.
    // audioOutputDevices: the operator's PC output devices, by name.
    // setAudioOutputDevice: switch output device live (restarts sink).
    Q_INVOKABLE void setVolume(double v);
    Q_INVOKABLE void setMuted(bool m);
    // Set the panadapter zoom (1.0 = full span).  Stores the factor; the
    // crop happens in copySpectrum.  No analyzer reconfiguration.
    Q_INVOKABLE void setZoom(double z);
    // Demod mode + RX bandwidth.  Stored when the channel is closed and
    // applied on open; applied live (SetRXAMode + RXASetPassband) while
    // running.
    QString mode() const { return mode_; }
    Q_INVOKABLE void setMode(const QString &m);
    int  bandwidth() const { return bw_; }
    Q_INVOKABLE void setBandwidth(int hz);
    // IQ sample rate (Hz).  Switching reopens the WDSP channel +
    // analyzer at the new rate (panadapter span follows).  Safe to call
    // while running — serialised against feedIq via channelMtx_.
    int  sampleRate() const { return cfg_.inRate; }
    Q_INVOKABLE void setSampleRate(int hz);
    double passbandLowHz()  const { return passbandLowHz_; }
    double passbandHighHz() const { return passbandHighHz_; }
    int  cwPitchHz() const { return static_cast<int>(cwPitchHz_ + 0.5); }
    Q_INVOKABLE void setCwPitchHz(int hz);
    int  markerOffsetHz() const;     // VFO − DDS (CW carrier convention)
    // Same VFO−DDS offset for an ARBITRARY mode — used when tuning to a CW
    // spot/VFO whose freq is the carrier (DDS = carrier − this).
    Q_INVOKABLE int cwMarkerOffsetForMode(const QString &mode) const;
    // Bandwidth (Hz) that results from dragging a passband edge to
    // `edgeOffsetHz` (offset from the tuned centre) in the current mode.
    // The panadapter edge-drag calls this, then writes Prefs.rxBandwidth.
    Q_INVOKABLE int bandwidthForEdge(double edgeOffsetHz) const;
    Q_INVOKABLE QStringList audioOutputDevices() const;
    Q_INVOKABLE void setAudioOutputDevice(int index);
    // Wire the HL2 onboard-codec audio path (reverse of setIqSink):
    // `push` hands decoded stereo int16 to the stream's EP2 writer;
    // `enable` turns EP2 audio injection on/off.  Set once at startup.
    void setHl2AudioSink(std::function<void(const qint16 *, int)> push,
                         std::function<void(bool)> enable);

    // Step 3d: feed interleaved baseband IQ — (I,Q,I,Q,…) doubles
    // already normalized to [-1,1) — from the RX worker thread.
    // Accumulates into in_size blocks; each full block runs
    // fexchange0 IN-LINE on the caller's thread (block=1 returns as
    // soon as the DSP thread has the output ready, ~187 calls/sec)
    // and updates audioDbFs.  MUST be called from a SINGLE thread
    // (the HL2Stream RX worker) — not thread-safe within the channel.
    // No audio is played yet (Step 3e); this only measures.
    void feedIq(const double *iq, int nframes);

    // TCI streaming taps (off unless a TCI client subscribes).  When on,
    // feedIq emits a copy of each post-DSP audio block (mono float32 @
    // outRate) / raw IQ block (interleaved I,Q float32 @ inRate) as a
    // queued signal so the (main-thread) TCI server can frame + send it.
    void setTciAudioStreaming(bool on) {
        tciAudioOn_.store(on, std::memory_order_relaxed);
    }
    void setTciIqStreaming(bool on) {
        tciIqOn_.store(on, std::memory_order_relaxed);
    }

    // Open RX1 (channel 0), apply the locked first-light config and
    // start the channel.  Idempotent; returns true on success.
    Q_INVOKABLE bool openRx1();

    // Stop (blocking flush) + close channel 0.  Idempotent.  Called
    // automatically on destruction.
    Q_INVOKABLE void closeRx1();

signals:
    void runningChanged();
    void levelsChanged();
    void volumeChanged();
    void mutedChanged();
    void audioDeviceChanged();
    void zoomChanged();
    void spanChanged();   // displayed span changed (rate OR zoom)
    void modeChanged();
    void bandwidthChanged();
    void passbandChanged();
    void cwPitchChanged();
    void markerOffsetChanged();
    // TCI streaming: post-DSP mono audio (float32 @ rateHz) and raw IQ
    // (interleaved I,Q float32 @ rateHz).  Emitted from the RX worker
    // thread → delivered to the TCI server via a queued connection.
    void tciAudioBlock(const QByteArray &monoFloat, int rateHz);
    void tciIqBlock(const QByteArray &iqFloat, int rateHz);
    void logLine(QString line);

private:
    void emitLog(const QString &line);   // mirror logLine -> qInfo console
    bool startAudio();   // create + start the QAudioSink (Step 3e)
    void stopAudio();    // stop + tear down the QAudioSink
    // Push the current mode_/bw_ to WDSP (SetRXAMode + RXASetPassband).
    // No-op when the channel isn't open (applied on the next openRx1).
    void applyModeFilter();
    // Passband edges (Hz offsets from centre) for mode_ + bw_ + pitch.
    void computePassband(double *lo, double *hi) const;
    void recomputePassband();   // store + emit passbandChanged

    WdspNative *wdsp_    = nullptr;
    RxConfig    cfg_;
    // Serialises the WDSP channel lifecycle (open/close on the main
    // thread, e.g. a sample-rate switch) against feedIq()'s fexchange0
    // on the RX worker thread — so a rate change can't tear the channel
    // down mid-process.
    std::mutex  channelMtx_;
    int         channel_ = 0;
    int         outSize_ = 0;
    bool        opened_  = false;
    bool        running_ = false;
    bool        analyzerOpen_ = false;   // Step 5 panadapter analyzer

    // Step 3d DSP buffers (all sized in the constructor).
    // accum_ : interleaved IQ doubles awaiting a full in_size block.
    // outBuf_: fexchange0 output (2 * outSize_ doubles, L/R).
    std::vector<double> accum_;
    std::vector<double> outBuf_;
    int                 fexErr_ = 0;
    std::atomic<double> audioDbFs_{-200.0};
    std::atomic<bool>   tciAudioOn_{false};   // TCI audio stream tap on
    std::atomic<bool>   tciIqOn_{false};      // TCI IQ stream tap on
    // 5 Hz UI poll — emits levelsChanged so the QML audioDbFs binding
    // re-reads the atomic.  Lives on the main thread (WdspEngine is a
    // main-thread object); started in openRx1, stopped in closeRx1.
    QTimer              levelsTimer_;

    // Step 3e: PC sound-card playback.  audioRing_ is the QIODevice the
    // sink pulls from (fed by feedIq on the RX worker thread);
    // audioSink_ is the QAudioSink driving the chosen output device.
    // audioMtx_ guards the audioRing_/audioSink_ pointers so the RX
    // worker's push() never races a main-thread device switch / teardown.
    std::mutex          audioMtx_;
    AudioRing          *audioRing_ = nullptr;
    QAudioSink         *audioSink_ = nullptr;
    std::vector<qint16> pcm16_;
    // Slider defaults to 0.65 (≈ -14 dB taper, plus the HL2 path's extra
    // ~-10.5 dB) — a moderate, non-blast startup level.  muted_ is
    // restored from QSettings in the ctor (default UNMUTED).
    std::atomic<double> volume_{0.65};
    std::atomic<bool>   muted_{false};
    // Panadapter zoom (1.0 = full span).  Written from the UI/main
    // thread; read by spanHz() (UI) + copySpectrum() (crop).
    std::atomic<double> zoom_{1.0};
    // Demod mode + RX bandwidth (Hz) + CW pitch.  Main-thread only
    // (UI setters + openRx1).  Default USB 2.4 kHz; CW centres on pitch.
    QString mode_       = QStringLiteral("USB");
    int     bw_         = 2400;
    double  cwPitchHz_  = 600.0;
    double  passbandLowHz_  = 200.0;    // edges for the panadapter overlay
    double  passbandHighHz_ = 2400.0;
    // Last good full-resolution spectrum (kAnPixels dB points).  WDSP's
    // GetPixels resets its ready-flag on read, so with TWO consumers
    // (panadapter + waterfall) the second sees no data; caching here lets
    // both always read a consistent, valid frame (no uninitialised
    // garbage feeding the zoom crop).  GUI-thread only (the QQuickWidget
    // panadapter + waterfall reads are serialised).
    std::vector<float> specCache_;
    QList<QAudioDevice> devices_;       // operator's PC output devices
    int                 deviceIndex_ = 0;
    // Output routing: HL2 onboard codec (default — old Lyra's HL2 path)
    // vs a PC sound device.  When hl2Out_ the QAudioSink is not used;
    // audio goes to the EP2 writer via hl2AudioPush_.
    bool                hl2Out_ = true;
    std::function<void(const qint16 *, int)> hl2AudioPush_;
    std::function<void(bool)>                hl2AudioEnable_;
};

} // namespace lyra::dsp
