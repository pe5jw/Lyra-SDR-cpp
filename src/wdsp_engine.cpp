// Lyra — WDSP RX channel engine implementation (Step 3c-ii).
// See wdsp_engine.h for the locked scope + the §14.2 gotcha list.

#include "wdsp_engine.h"

#include "capturedprofile.h"
#include "noisereducer.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QDataStream>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardPaths>
#include <QIODevice>
#include <QMediaDevices>
#include <QSettings>

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstring>
#include <mutex>

namespace lyra::dsp {

namespace {

// Mode / AGC integer constants, verified against the Python tree's
// wdsp_native.py (RxaMode / AgcMode) which in turn matches the
// upstream WDSP source.  Do NOT change without re-verifying — these
// map directly onto WDSP's RXA mode + wcpAGC mode switch statements.
constexpr int    kRxaModeUSB = 1;   // RxaMode.USB
// wcpAGC mode enum (WDSP AGC modes): FIXD=0 LONG=1 SLOW=2 MED=3 FAST=4.
// "off" maps to FIXD (fixed gain) — the operator-facing AGC-off.
constexpr int    kAgcModeOff  = 0;  // AgcMode.FIXD (fixed gain)
constexpr int    kAgcModeSlow = 2;  // AgcMode.SLOW
constexpr int    kAgcModeMed  = 3;  // AgcMode.MED
constexpr int    kAgcModeFast = 4;  // AgcMode.FAST
constexpr double kUsbLowHz   = 0.0;   // SSB/DIG low cut starts at centre (op req)
constexpr double kUsbHighHz  = 3000.0;

// Step 3e level-cal (mirrors the bench-proven Python _open_wdsp_rx).
// slope 35 -> var_gain = 10^(35/200) ~ 1.5 (WDSP/industry soft-knee);
// threshold -100 dBFS gives the AGC ~70 dB of headroom to boost weak
// signals without over-driving the output past 0 dBFS.  size 4096 +
// the IQ input rate are SetRXAAGCThresh's noise-bandwidth conversion
// args.  Do NOT also call SetRXAAGCTop — it writes the same max_gain
// field SetRXAAGCThresh computes and would clobber it.
constexpr int    kAgcSlope         = 35;
constexpr double kAgcThreshDbFs    = -100.0;
constexpr double kAgcThreshFftSize = 4096.0;

// Step 5: WDSP spectral analyzer (panadapter) config.  Values mirror
// the standard HL2 RX analyzer setup (fft 4096, window 4, kaiser 14).  pixels is
// fixed (the panadapter scales it to its on-screen width).  overlap =
// fft - feed-block so one FFT advances per IQ feed (smooth).  Detector
// peak + recursive time-average for a stable display.  All tunable on
// the bench.
constexpr int    kAnDisp        = 0;       // analyzer id (RX1)
constexpr int    kAnMaxFft      = 32768;   // max fft (must be >= max_w)
constexpr int    kAnFftSize     = 4096;
// Full FFT resolution out of the analyzer (1 pixel per bin) so the
// zoom CROP in copySpectrum has the same source detail old Lyra's raw
// 4096-bin FFT had — cropping the centre 1/zoom of THIS stays smooth
// (a 2048 pre-binned output cropped + stretched looked "chunky").
constexpr int    kAnPixels      = 4096;    // dB output points
constexpr int    kAnWindow      = 4;       // soft-knee window (ref default)
constexpr double kAnKaiserPi    = 14.0;
constexpr int    kAnFrameRate   = 60;      // display fps — drives overlap,
                                           // max_w + averaging frame count.
                                           // Higher = more responsive trace
                                           // (tau holds the smoothing TIME
                                           // constant).  Operator-adjustable
                                           // control to come.
constexpr int    kAnDetector    = 0;       // 0 = peak
// Time-averaging OFF by default (reference ships AverageOn=false) — a
// LIVE trace, no smoothing lag, so it stays in sync with the audio.
// The smooth "fluid" look comes from the curve RENDERING (no lag), not
// from time-averaging.  Flip kAnAvgMode != 0 + tune kAnTau for an
// operator AVG toggle later.
constexpr int    kAnAvgMode     = 0;       // 0 = off (live)
constexpr double kAnTau         = 0.12;    // avg time constant (s) when on

// Step 3e audio-ring sizing.  The reference Python SoundDeviceSink
// pre-fills 100 ms (= 4800 frames @ 48 kHz) because Python/GIL
// delivered audio in bursty ~43 ms lumps that drained the ring.  The
// C++ RX worker pushes audio every ~5.3 ms with no GIL, so we run a
// SMALLER pre-fill.  Both tunable here.
constexpr int    kRingMs       = 200;  // ring capacity (ms)
constexpr int    kPrefillMs    = 30;   // startup silence pre-fill (ms)
// Explicit QAudioSink device-buffer depth.  Qt's default is large
// (~100-200 ms on WASAPI) and was the dominant audio latency that made
// the audio lag the panadapter.  ~40 ms keeps it tight without
// underrunning (operator-tunable on the bench).
constexpr int    kSinkBufferMs = 40;

// Step 3e: perceptual volume taper.  Slider position (0..1) -> dB gain
// so comfortable listening sits mid-slider instead of bunched at the
// bottom (a linear gain made 8% already loud).  pos=1 -> 0 dB (unity),
// pos=0.5 -> -20 dB, pos->0 -> silence.  Floor -40 dB (was -60) so the
// whole curve is louder per position (operator: "10 should sound like
// 20").  Mirrors the Python dB-volume idiom (CLAUDE.md §15.17).
constexpr double kMinVolDb = -40.0;

double posToDb(double pos) {
    return (pos <= 0.0) ? kMinVolDb : kMinVolDb * (1.0 - pos);
}

double posToGain(double pos) {
    if (pos <= 0.0) return 0.0;
    if (pos >= 1.0) return 1.0;
    return std::pow(10.0, posToDb(pos) / 20.0);
}

// The HL2 AK4951 codec output runs hotter than the PC sound-card chain,
// so the SAME volume slider was much louder on the HL2 jack (operator:
// "13 on PC ≈ 35 on HL2" → ~8.8 dB).  Attenuate the HL2-jack path so
// both outputs read roughly the same at a given slider position.
constexpr double kHl2OutAtten = 0.30;   // ≈ -10.5 dB

// Demod-mode name -> WDSP RxaMode int (matches the bench-proven Python
// wdsp_native.py RxaMode enum).  Unknown -> USB.
int modeToWdsp(const QString &m) {
    if (m == QLatin1String("LSB"))  return 0;
    if (m == QLatin1String("USB"))  return 1;
    if (m == QLatin1String("DSB"))  return 2;
    if (m == QLatin1String("CWL"))  return 3;
    if (m == QLatin1String("CWU"))  return 4;
    if (m == QLatin1String("FM"))   return 5;
    if (m == QLatin1String("AM"))   return 6;
    if (m == QLatin1String("DIGU")) return 7;
    if (m == QLatin1String("DIGL")) return 9;
    return 1;   // USB
}

} // namespace

// ---------------------------------------------------------------
// AudioRing — the QIODevice the QAudioSink pulls from (pull mode,
// the Qt-native equivalent of the reference's CallbackASIO /
// PortAudio callback).  Producer = the RX worker thread via push();
// consumer = Qt's audio backend thread via readData().  A std::mutex
// guards the ring indices (hold time = a memcpy of a few hundred
// int16, sub-ms).  On underrun readData pads silence and always
// returns the full requested length so the sink never stops.
// ---------------------------------------------------------------
class AudioRing : public QIODevice {
public:
    explicit AudioRing(int capacityFrames)
        : capFrames_(capacityFrames),
          buf_(static_cast<size_t>(capacityFrames) * 2, 0) {}

    // Producer (RX worker thread): push interleaved stereo int16.
    void push(const qint16 *lr, int nframes) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int f = 0; f < nframes; ++f) {
            if (countFrames_ >= capFrames_) {
                rd_ = (rd_ + 1) % capFrames_;   // overrun: drop oldest
                --countFrames_;
                ++overruns_;
            }
            buf_[static_cast<size_t>(wr_) * 2 + 0] = lr[f * 2 + 0];
            buf_[static_cast<size_t>(wr_) * 2 + 1] = lr[f * 2 + 1];
            wr_ = (wr_ + 1) % capFrames_;
            ++countFrames_;
        }
    }

    // Seed the ring with `frames` of silence so the sink has headroom
    // before the worker produces its first block.
    void prefillSilence(int frames) {
        std::lock_guard<std::mutex> lk(mtx_);
        rd_ = wr_ = countFrames_ = 0;
        const int n = std::min(frames, capFrames_);
        for (int i = 0; i < n; ++i) {
            buf_[static_cast<size_t>(wr_) * 2 + 0] = 0;
            buf_[static_cast<size_t>(wr_) * 2 + 1] = 0;
            wr_ = (wr_ + 1) % capFrames_;
            ++countFrames_;
        }
    }

    qint64 overruns()  const { std::lock_guard<std::mutex> lk(mtx_); return overruns_;  }
    qint64 underruns() const { std::lock_guard<std::mutex> lk(mtx_); return underruns_; }

    // --- QIODevice overrides (no Q_OBJECT needed: only virtuals) ---
    bool   isSequential() const override { return true; }
    qint64 writeData(const char *, qint64) override { return -1; }

    // Report a large, always-positive availability so the sink keeps
    // pulling on its own cadence (we pad silence inside readData).
    qint64 bytesAvailable() const override {
        return static_cast<qint64>(capFrames_) * kBytesPerFrame
               + QIODevice::bytesAvailable();
    }

    qint64 readData(char *data, qint64 maxlen) override {
        const qint64 maxFrames = maxlen / kBytesPerFrame;
        if (maxFrames <= 0) {
            return 0;
        }
        qint16 *out = reinterpret_cast<qint16 *>(data);
        bool padded = false;
        std::lock_guard<std::mutex> lk(mtx_);
        for (qint64 f = 0; f < maxFrames; ++f) {
            if (countFrames_ > 0) {
                out[f * 2 + 0] = buf_[static_cast<size_t>(rd_) * 2 + 0];
                out[f * 2 + 1] = buf_[static_cast<size_t>(rd_) * 2 + 1];
                rd_ = (rd_ + 1) % capFrames_;
                --countFrames_;
            } else {
                out[f * 2 + 0] = 0;   // underrun -> silence
                out[f * 2 + 1] = 0;
                padded = true;
            }
        }
        if (padded) {
            ++underruns_;
        }
        return maxFrames * kBytesPerFrame;   // always full -> sink stays active
    }

private:
    static constexpr qint64 kBytesPerFrame = 2 * sizeof(qint16);  // stereo
    mutable std::mutex  mtx_;
    std::vector<qint16> buf_;        // interleaved L,R; capFrames_*2
    int                 capFrames_;
    int                 rd_ = 0, wr_ = 0, countFrames_ = 0;
    qint64              overruns_ = 0, underruns_ = 0;
};

WdspEngine::WdspEngine(WdspNative *wdsp, QObject *parent)
    : QObject(parent), wdsp_(wdsp)
{
    // out_size = in_size * out_rate / in_rate (when in_rate >= out_rate).
    // With 1024 @ 192k -> 48k that is 256 frames per fexchange0 call.
    if (cfg_.inRate >= cfg_.outRate) {
        outSize_ = cfg_.inSize / (cfg_.inRate / cfg_.outRate);
    } else {
        outSize_ = cfg_.inSize * (cfg_.outRate / cfg_.inRate);
    }

    // fexchange0 output buffer: 2 * outSize_ doubles (interleaved L/R).
    outBuf_.assign(static_cast<size_t>(2 * outSize_), 0.0);
    // int16 stereo scratch for the audio-ring push (Step 3e).
    pcm16_.assign(static_cast<size_t>(2 * outSize_), 0);
    // Headroom for one in_size block + a couple of EP6 datagrams so
    // feedIq's append never reallocates in steady state.
    accum_.reserve(static_cast<size_t>(2 * (cfg_.inSize + 128)));

    // 5 Hz UI poll: emit levelsChanged so the QML audioDbFs binding
    // re-reads the atomic (mirrors HL2Stream's statsTimer cadence).
    levelsTimer_.setInterval(200);
    connect(&levelsTimer_, &QTimer::timeout, this, [this]() {
        emit levelsChanged();
        // Captured-profile capture progress: the RX worker updates the
        // atomics inside feedIq; surface them to the UI on this poll.
        // Emit while a capture is running and once on its falling edge.
        const bool cap = noiseCapturing_.load(std::memory_order_relaxed);
        if (cap || cap != npLastCapturing_) {
            npLastCapturing_ = cap;
            emit noiseCaptureChanged();
        }
    });

    // Step 3e: enumerate the operator's PC output devices + default.
    devices_ = QMediaDevices::audioOutputs();
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    for (int i = 0; i < devices_.size(); ++i) {
        if (devices_[i].id() == def.id()) {
            deviceIndex_ = i;
            break;
        }
    }

    // Radio memory: restore the operator's volume + output device.
    // Restore the operator's last mute state (default UNMUTED — the
    // audio path is now level-matched and the volume taper keeps the
    // startup level moderate).  Device matched by description; falls
    // back to the system default (set above) if it's no longer present.
    QSettings s;
    muted_.store(s.value(QStringLiteral("audio/muted"), false).toBool(),
                 std::memory_order_relaxed);
    // Auto-mute-on-TX (task #26) defaults TRUE — safer first-launch
    // posture.  Operator can turn it off in Settings -> Hardware ->
    // Transmit for ESSB monitoring or any other listen-to-myself
    // workflow.  txMuted_ stays false at boot (no live MOX yet).
    autoMuteOnTx_.store(
        s.value(QStringLiteral("audio/autoMuteOnTx"), true).toBool(),
        std::memory_order_relaxed);
    volume_.store(std::clamp(
        s.value(QStringLiteral("audio/volume"), 0.65).toDouble(), 0.0, 1.0),
        std::memory_order_relaxed);
    afGainDb_ = std::clamp(
        s.value(QStringLiteral("audio/afGainDb"), 0.0).toDouble(), 0.0, 40.0);
    balance_.store(std::clamp(
        s.value(QStringLiteral("audio/balance"), 0.0).toDouble(), -1.0, 1.0),
        std::memory_order_relaxed);
    const QString savedDev =
        s.value(QStringLiteral("audio/deviceName")).toString();
    if (!savedDev.isEmpty()) {
        for (int i = 0; i < devices_.size(); ++i) {
            if (devices_[i].description() == savedDev) {
                deviceIndex_ = i;
                break;
            }
        }
    }
    // Output routing: HL2 onboard codec (default — old Lyra's HL2 path)
    // unless the operator previously chose a PC device.
    hl2Out_ = s.value(QStringLiteral("audio/output"),
                      QStringLiteral("hl2")).toString() != QLatin1String("pc");
    cwPitchHz_ = std::clamp(
        s.value(QStringLiteral("dsp/cwPitchHz"), 600).toInt(), 200, 1500);
    // RX DSP operator state (NR + AGC mode).  Defaults match old Lyra's
    // first-light: NR off, Mode 3 (MMSE-LSA), AEPF on, NPE OSMS, AGC med.
    nrEnabled_   = s.value(QStringLiteral("dsp/nrEnabled"), false).toBool();
    nrMode_      = std::clamp(
        s.value(QStringLiteral("dsp/nrMode"), 3).toInt(), 1, 4);
    aepfEnabled_ = s.value(QStringLiteral("dsp/aepf"), true).toBool();
    npeMethod_   = std::clamp(
        s.value(QStringLiteral("dsp/npeMethod"), 0).toInt(), 0, 1);
    agcMode_     = s.value(QStringLiteral("dsp/agcMode"),
                           QStringLiteral("med")).toString();
    anfEnabled_  = s.value(QStringLiteral("dsp/anfEnabled"), false).toBool();
    lmsEnabled_  = s.value(QStringLiteral("dsp/lmsEnabled"), false).toBool();
    lmsStrength_ = std::clamp(
        s.value(QStringLiteral("dsp/lmsStrength"), 0.5).toDouble(), 0.0, 1.0);
    // Manual notches — each persisted as "offsetHz|widthHz|active".
    squelchEnabled_ = s.value(QStringLiteral("dsp/squelchEnabled"), false).toBool();
    squelchThreshold_ = std::clamp(
        s.value(QStringLiteral("dsp/squelchThreshold"), 0.20).toDouble(), 0.0, 1.0);
    nbEnabled_  = s.value(QStringLiteral("dsp/nbEnabled"), false).toBool();
    nbStrength_ = std::clamp(
        s.value(QStringLiteral("dsp/nbStrength"), 0.5).toDouble(), 0.0, 1.0);
    apfEnabled_ = s.value(QStringLiteral("dsp/apfEnabled"), false).toBool();
    apfGainDb_  = std::clamp(
        s.value(QStringLiteral("dsp/apfGainDb"), 12.0).toDouble(), 0.0, 24.0);
    binEnabled_ = s.value(QStringLiteral("dsp/binEnabled"), false).toBool();
    binDepth_   = std::clamp(
        s.value(QStringLiteral("dsp/binDepth"), 0.7).toDouble(), 0.0, 1.0);
    buildBinaural();   // Hilbert FIR + history (rate-agnostic, built once)
    notchEnabled_ = s.value(QStringLiteral("dsp/notchEnabled"), false).toBool();
    const QStringList savedNotches =
        s.value(QStringLiteral("dsp/notches")).toStringList();
    for (const QString &entry : savedNotches) {
        const QStringList p = entry.split(QLatin1Char('|'));
        if (p.size() == 3)
            notches_.push_back({p[0].toDouble(),
                                std::max(10.0, p[1].toDouble()),
                                p[2].toInt() != 0});
    }
    recomputePassband();   // seed passband edges from the loaded mode/bw/pitch
    // Captured-profile settings + saved profiles.
    {
        const int fft = s.value(QStringLiteral("dsp/npFftSize"), 4096).toInt();
        npFftSize_ = (fft == 2048 || fft == 4096 || fft == 8192) ? fft : 4096;
        const double sec = s.value(QStringLiteral("dsp/npCaptureSeconds"), 5.0).toDouble();
        npCaptureSeconds_ = (sec == 3.0 || sec == 5.0 || sec == 10.0) ? sec : 5.0;
        npAlpha_     = std::clamp(s.value(QStringLiteral("dsp/npAlpha"), 1.0).toDouble(), 1.0, 5.0);
        npFloorDb_   = std::clamp(s.value(QStringLiteral("dsp/npFloorDb"), -12.0).toDouble(), -30.0, -3.0);
        npSmoothing_ = std::clamp(s.value(QStringLiteral("dsp/npSmoothing"), 0.6).toDouble(), 0.0, 0.95);
    }
    loadProfiles();
}

WdspEngine::~WdspEngine()
{
    closeRx1();
}

void WdspEngine::emitLog(const QString &line)
{
    qInfo("%s", qPrintable(line));
    emit logLine(line);
}

bool WdspEngine::openRx1()
{
    if (opened_) {
        return true;  // idempotent
    }
    if (!wdsp_ || !wdsp_->isLoaded()) {
        emitLog(QStringLiteral(
            "[wdsp] engine: cannot open RX1 — DLL not loaded"));
        return false;
    }

    const WdspApi &api = wdsp_->api();
    if (!api.OpenChannel || !api.SetChannelState || !api.SetRXAMode ||
        !api.RXASetPassband || !api.SetRXAAGCMode ||
        !api.SetRXAPanelBinaural) {
        emitLog(QStringLiteral(
            "[wdsp] engine: cannot open RX1 — required symbols not "
            "resolved"));
        return false;
    }

    // OpenChannel(channel, in_size, dsp_size, in_rate, dsp_rate,
    //             out_rate, type=RX(0), state=stopped(0),
    //             tdelayup, tslewup, tdelaydown, tslewdown, block).
    api.OpenChannel(channel_, cfg_.inSize, cfg_.dspSize,
                    cfg_.inRate, cfg_.dspRate, cfg_.outRate,
                    0,   // type = RX
                    0,   // state = stopped — we start it explicitly below
                    cfg_.tDelayUp, cfg_.tSlewUp,
                    cfg_.tDelayDown, cfg_.tSlewDown,
                    cfg_.block);
    opened_ = true;

    // Binaural OFF (arg 0) => WDSP panel.copy=1 => I copied to Q at the
    // panel output => mono on BOTH L/R regardless of any upstream stage
    // that zeroed Q (e.g. EMNR).  This is the verified-correct default
    // + the AM/FM/DSB right-channel-silent fix (CLAUDE.md §14.10).
    api.SetRXAPanelBinaural(channel_, 0);

    // Demod mode + passband from the operator's current selection
    // (default USB 2.4 kHz; persisted via Prefs and applied before the
    // channel started running).  RXASetPassband updates NBP0
    // (front-of-chain, where sideband selection lives + always runs) +
    // BP1 + the SNBA filter in one call (§14.2).
    applyModeFilter();

    // AGC mode (operator-selected, persisted; default med).
    pushAgcMode();

    // Level calibration: replace WDSP's hot create-time AGC default
    // (max_gain = 10000 / 80 dB, which overshoots 0 dBFS) with a
    // threshold-computed ceiling.  SetRXAAGCThresh derives max_gain
    // from (thresh, size, rate) + the slope-derived var_gain; we must
    // NOT also call SetRXAAGCTop (same field, would clobber).
    if (api.SetRXAAGCSlope) {
        api.SetRXAAGCSlope(channel_, kAgcSlope);
    }
    if (api.SetRXAAGCThresh) {
        api.SetRXAAGCThresh(channel_, kAgcThreshDbFs, kAgcThreshFftSize,
                            static_cast<double>(cfg_.inRate));
    }
    // Panel (post-DSP makeup) gain = the operator's AF gain (dB → linear;
    // 0 dB = unity = WDSP create_panel default).
    if (api.SetRXAPanelGain1) {
        api.SetRXAPanelGain1(channel_, std::pow(10.0, afGainDb_ / 20.0));
    }

    // RX noise reduction (EMNR) + auto-notch + LMS — persisted state.
    pushNrState();
    pushAnfState();
    pushLmsState();
    pushNotches();
    pushSquelchState();

    // Noise blanker (EXT NOB-II): create the per-channel blanker sized to
    // our IQ block + rate, then push threshold/run.  feedIq splices
    // xnobEXT on the raw IQ before fexchange0 when NB is on.  Impulse-only
    // tuning (tight slew/hang/adv); destroyed in closeRx1.
    if (api.create_nobEXT && !nbCreated_) {
        nbBuf_.assign(static_cast<size_t>(2 * cfg_.inSize), 0.0);
        api.create_nobEXT(channel_, 0, 0, cfg_.inSize,
                          static_cast<double>(cfg_.inRate),
                          0.0001, 0.0001, 0.0001, 0.020, 20.0);
        nbCreated_ = true;
    }
    pushNbState();
    pushApfState();

    // Step 5: create + configure the spectral analyzer (panadapter
    // source).  Independent of the DSP channel; fed the same IQ.
    if (api.XCreateAnalyzer && api.SetAnalyzer) {
        int success = 0;
        char appDataPath[] = "";   // empty app-data path (no temp files)
        api.XCreateAnalyzer(kAnDisp, &success, kAnMaxFft, 1, 1, appDataPath);
        if (success == 0) {
            // overlap + max_w per the frame-rate formula.  max_w sizes
            // an internal display-history buffer — passing 0 makes WDSP
            // crash on a zero-size allocation.  overlap clamps to 0 at
            // 192 kHz / 4096 (samples-per-frame >> fft).
            const int overlap =
                std::max(0, kAnFftSize - cfg_.inRate / kAnFrameRate);
            const int maxW = kAnFftSize +
                std::min(cfg_.inRate / 10, kAnFftSize * kAnFrameRate / 10);
            // flp = per-FFT high-side-LO flags (int* vector).  One FFT,
            // not high-side -> {0}.  MUST be a real pointer (passing an
            // int crashes WDSP — it dereferences flp[i]).
            int flp[1] = {0};
            api.SetAnalyzer(
                kAnDisp,                    // disp
                1,                          // n_pixout
                1,                          // n_fft (spur-elim ffts)
                1,                          // typ = complex IQ
                flp,                        // flp (int* high-side LO flags)
                kAnFftSize,                 // sz (fft size)
                cfg_.inSize,                // bf_sz (our feed block)
                kAnWindow,                  // win_type
                kAnKaiserPi,                // pi (kaiser)
                overlap,                    // ovrlp
                0,                          // clp (clip bins/side)
                0.0, 0.0,                   // fsc_lin, fsc_hin (DOUBLE)
                kAnPixels,                  // n_pix (display points)
                1,                          // n_stch (stitches)
                0,                          // calset
                0.0, 0.0,                   // fmin, fmax
                maxW);                      // max_w (history buffer size)
            if (api.SetDisplayDetectorMode) {
                api.SetDisplayDetectorMode(kAnDisp, 0, kAnDetector);
            }
            // Average mode (0 = off = live trace by default).  When
            // enabled, the back-multiplier + frame count derive from
            // tau + frame_rate exactly as the reference does.
            if (api.SetDisplayAverageMode) {
                api.SetDisplayAverageMode(kAnDisp, 0, kAnAvgMode);
            }
            if (kAnAvgMode != 0) {
                const double avb =
                    std::exp(-1.0 / (kAnFrameRate * kAnTau));
                const int numAvg = std::max(2,
                    std::min(60, static_cast<int>(kAnFrameRate * kAnTau)));
                if (api.SetDisplayAvBackmult) {
                    api.SetDisplayAvBackmult(kAnDisp, 0, avb);
                }
                if (api.SetDisplayNumAverage) {
                    api.SetDisplayNumAverage(kAnDisp, 0, numAvg);
                }
            }
            analyzerOpen_ = true;
            emitLog(QStringLiteral(
                "[wdsp] analyzer: %1 pixels, fft %2, window %3 "
                "(panadapter source)")
                .arg(kAnPixels).arg(kAnFftSize).arg(kAnWindow));
        } else {
            emitLog(QStringLiteral(
                "[wdsp] analyzer: XCreateAnalyzer failed (%1) — no "
                "panadapter").arg(success));
        }
    }

    // Step 3e: bring up PC sound-card playback BEFORE running_ goes
    // true, so feedIq sees a live ring the moment IQ starts flowing.
    // Non-fatal: if the audio device is missing/unsupported we log and
    // continue (the dBFS meter still works, just no sound).
    startAudio();

    // Start the channel (state=running, dmode=0).
    api.SetChannelState(channel_, 1, 0);
    running_ = true;
    emit runningChanged();
    emit spanChanged();     // QML freq scale re-reads spanHz on start
    levelsTimer_.start();   // begin the 5 Hz audioDbFs UI poll

    emitLog(QStringLiteral(
        "[wdsp] channel 0 opened (192k IQ -> 48k audio, USB "
        "200-3000 Hz, AGC MED thr -100 dBFS, binaural mono); "
        "out_size=%1 frames").arg(outSize_));
    return true;
}

void WdspEngine::closeRx1()
{
    if (!opened_) {
        return;  // idempotent
    }
    const WdspApi &api = wdsp_->api();
    // Stop with dmode=1 (blocking flush) so in-flight buffers drain
    // before CloseChannel tears the channel down.
    if (api.SetChannelState) {
        api.SetChannelState(channel_, 0, 1);
    }
    if (nbCreated_ && api.destroy_nobEXT) {
        api.destroy_nobEXT(channel_);
        nbCreated_ = false;
    }
    if (api.CloseChannel) {
        api.CloseChannel(channel_);
    }
    opened_ = false;
    if (running_) {
        running_ = false;
        emit runningChanged();
    }
    levelsTimer_.stop();
    stopAudio();
    if (analyzerOpen_ && api.DestroyAnalyzer) {
        api.DestroyAnalyzer(kAnDisp);
        analyzerOpen_ = false;
    }
    audioDbFs_.store(-200.0, std::memory_order_relaxed);
    accum_.clear();
    emitLog(QStringLiteral("[wdsp] channel 0 closed"));
}

void WdspEngine::setZoom(double z)
{
    z = std::max(1.0, std::min(32.0, z));
    if (std::abs(z - zoom_.load(std::memory_order_relaxed)) < 1e-6) {
        return;
    }
    zoom_.store(z, std::memory_order_relaxed);
    // No analyzer reconfiguration — the next copySpectrum() crops to the
    // new zoom.  spanChanged() refreshes the QML frequency scale.
    emit zoomChanged();
    emit spanChanged();
}

void WdspEngine::computePassband(double *lo, double *hi) const
{
    // Per-mode passband edges (offsets from the tuned centre), matching
    // old Lyra's _wdsp_filter_for.  These map onto the HL2 mirrored
    // baseband so the sideband comes out correct (§14.2).
    //
    // Task #53 — the asymmetric SSB/DIG low edge is now operator-
    // tunable via filterLow_ (was hardcoded 0).  USB/DIGU get the
    // low cut at +filterLow_ (positive baseband side); LSB/DIGL
    // mirror to -filterLow_.  filterLow_=0 reproduces the
    // pre-Task-#53 behaviour exactly (low cut at the carrier
    // centre).  CW filters are pitch-centred, low edge doesn't
    // apply meaningfully — left unchanged.  AM/DSB/FM are
    // symmetric around DC — also left unchanged.
    const double bw   = static_cast<double>(bw_);
    const double half = bw / 2.0;
    const double flo  = filterLow_;   // 0..500 Hz operator-tunable
    if (mode_ == QLatin1String("USB") || mode_ == QLatin1String("DIGU")) {
        *lo = flo;                *hi = bw;
    } else if (mode_ == QLatin1String("LSB") || mode_ == QLatin1String("DIGL")) {
        *lo = -bw;                *hi = -flo;
    } else if (mode_ == QLatin1String("CWU")) {
        *lo = cwPitchHz_ - half;  *hi = cwPitchHz_ + half;
    } else if (mode_ == QLatin1String("CWL")) {
        *lo = -cwPitchHz_ - half; *hi = -cwPitchHz_ + half;
    } else {                       // AM / DSB / FM (symmetric around DC)
        *lo = -half;              *hi = half;
    }
}

void WdspEngine::recomputePassband()
{
    double lo, hi;
    computePassband(&lo, &hi);
    if (lo != passbandLowHz_ || hi != passbandHighHz_) {
        passbandLowHz_  = lo;
        passbandHighHz_ = hi;
        emit passbandChanged();
    }
}

void WdspEngine::applyModeFilter()
{
    if (!opened_) {
        return;   // applied on the next openRx1()
    }
    const WdspApi &api = wdsp_->api();
    if (api.SetRXAMode) {
        api.SetRXAMode(channel_, modeToWdsp(mode_));
    }
    double lo, hi;
    computePassband(&lo, &hi);
    if (api.RXASetPassband) {
        api.RXASetPassband(channel_, lo, hi);
    }
}

int WdspEngine::bandwidthForEdge(double edgeOffsetHz) const
{
    const double a = std::abs(edgeOffsetHz);
    double bw;
    if (mode_ == QLatin1String("USB") || mode_ == QLatin1String("DIGU") ||
        mode_ == QLatin1String("LSB") || mode_ == QLatin1String("DIGL")) {
        bw = a;                                   // asymmetric: edge = cutoff
    } else if (mode_ == QLatin1String("CWU")) {
        bw = 2.0 * std::abs(edgeOffsetHz - cwPitchHz_);
    } else if (mode_ == QLatin1String("CWL")) {
        bw = 2.0 * std::abs(edgeOffsetHz + cwPitchHz_);
    } else {                                       // symmetric around DC
        bw = 2.0 * a;
    }
    return std::clamp(static_cast<int>(bw + 0.5), 50, 12000);
}

int WdspEngine::markerOffsetHz() const
{
    return cwMarkerOffsetForMode(mode_);
}

int WdspEngine::cwMarkerOffsetForMode(const QString &mode) const
{
    // VFO − DDS: the carrier sits +pitch above the DDS in CWU, −pitch
    // below in CWL; identity for every other mode.
    const int p = static_cast<int>(cwPitchHz_ + 0.5);
    if (mode == QLatin1String("CWU")) return  p;
    if (mode == QLatin1String("CWL")) return -p;
    return 0;
}

void WdspEngine::setCwPitchHz(int hz)
{
    hz = std::clamp(hz, 200, 1500);
    if (hz == static_cast<int>(cwPitchHz_ + 0.5)) {
        return;
    }
    cwPitchHz_ = static_cast<double>(hz);
    QSettings().setValue(QStringLiteral("dsp/cwPitchHz"), hz);
    recomputePassband();   // CW filter recentres on the new pitch
    applyModeFilter();
    pushApfState();        // APF peak tracks the CW pitch
    emit cwPitchChanged();
    emit markerOffsetChanged();   // VFO↔DDS offset changed (CW modes)
}

// Task #53 — shared RX+TX filter low edge.  RX-side application:
// triggers recomputePassband() + applyModeFilter() so the WDSP
// RXASetPassband picks up the new low edge live.  TX-side is
// driven separately via HL2Stream::setTxBandpass (the C++-side
// Prefs.filterLow signal connect lives in main.cpp).  Clamp
// 0..500 Hz matches Prefs::setFilterLow.
void WdspEngine::setFilterLowHz(int hz)
{
    hz = std::clamp(hz, 0, 500);
    if (hz == static_cast<int>(filterLow_ + 0.5)) {
        return;
    }
    filterLow_ = static_cast<double>(hz);
    recomputePassband();   // SSB/DIG passband shifts; CW/AM/DSB/FM unaffected
    applyModeFilter();
}

void WdspEngine::setMode(const QString &m)
{
    if (m.isEmpty() || m == mode_) {
        return;
    }
    mode_ = m;
    recomputePassband();   // overlay edges (even when closed)
    applyModeFilter();     // SetRXAMode + new passband (no-op if closed)
    pushSquelchState();    // re-route SSQL/FMSQ/AMSQ for the new mode
    pushApfState();        // APF engages only in CW — re-gate on mode change
    emit modeChanged();
    emit markerOffsetChanged();   // CW carrier offset flips with mode
}

void WdspEngine::setBandwidth(int hz)
{
    hz = std::clamp(hz, 10, 20000);
    if (hz == bw_) {
        return;
    }
    bw_ = hz;
    recomputePassband();   // overlay edges
    applyModeFilter();     // re-push passband for the new bandwidth
    emit bandwidthChanged();
}

void WdspEngine::setSampleRate(int hz)
{
    // HL2 P1 IQ rates only.  48k is intentionally excluded (EP2 cadence
    // — the same reason old Lyra dropped it).
    if (hz != 96000 && hz != 192000 && hz != 384000) {
        return;
    }
    if (hz == cfg_.inRate) {
        return;
    }
    // Hold channelMtx_ across the whole reopen so feedIq (RX worker)
    // can't run fexchange0 on a half-torn-down channel / stale outBuf_.
    std::lock_guard<std::mutex> lk(channelMtx_);
    const bool wasOpen = opened_;
    if (wasOpen) {
        closeRx1();          // blocking flush + close channel/analyzer/audio
    }
    cfg_.inRate = hz;
    // Re-size the fexchange0 output + int16 scratch for the new rate
    // (out_size = in_size * out_rate / in_rate).
    if (cfg_.inRate >= cfg_.outRate) {
        outSize_ = cfg_.inSize / (cfg_.inRate / cfg_.outRate);
    } else {
        outSize_ = cfg_.inSize * (cfg_.outRate / cfg_.inRate);
    }
    outBuf_.assign(static_cast<size_t>(2 * outSize_), 0.0);
    pcm16_.assign(static_cast<size_t>(2 * outSize_), 0);
    accum_.clear();
    if (wasOpen) {
        openRx1();           // reopen at the new rate (recreates analyzer)
    }
    emit spanChanged();      // panadapter span = inRate / zoom
    emitLog(QStringLiteral("[wdsp] IQ sample rate -> %1 kHz").arg(hz / 1000));
}

int WdspEngine::spectrumPixelCount() const
{
    return kAnPixels;
}

int WdspEngine::copySpectrum(float *dst, int maxN)
{
    if (!analyzerOpen_ || dst == nullptr) {
        return 0;
    }
    const WdspApi &api = wdsp_->api();
    if (!api.GetPixels) {
        return 0;
    }
    const int n = std::min(maxN, kAnPixels);

    // Pull the latest full-resolution spectrum into the persistent cache.
    // GetPixels memcpy's + clears its ready-flag on a fresh frame, and
    // leaves the buffer UNTOUCHED when there's none (flag=0).  Because the
    // panadapter AND the waterfall both call this each frame, the second
    // caller always gets flag=0 — so reading into a cache that retains the
    // last good frame is what keeps BOTH consumers fed with valid data
    // (an uninitialised per-call buffer here is what produced the red
    // waterfall garbage).  First-time init to a quiet floor.
    if (static_cast<int>(specCache_.size()) != kAnPixels) {
        specCache_.assign(kAnPixels, -200.0f);
    }
    int flag = 0; double ref = 0.0;
    api.GetPixels(kAnDisp, 0, specCache_.data(), &flag, &ref);
    const float *full = specCache_.data();

    const double z = zoom_.load(std::memory_order_relaxed);
    if (z <= 1.0) {
        // Full span — hand the cached spectrum straight back.
        std::memcpy(dst, full, static_cast<size_t>(n) * sizeof(float));
        return n;
    }

    // Zoomed (old-Lyra method): crop the centre 1/zoom slice of the
    // full-resolution spectrum and linearly resample it up to n display
    // points.  Pure display-side crop — the analyzer is never
    // reconfigured, so the trace can't be corrupted by a live re-setup.
    const double keep = static_cast<double>(kAnPixels) / z;   // bins shown
    const double lo   = (kAnPixels - keep) * 0.5;             // left edge
    const double span = (keep > 1.0) ? (keep - 1.0) : 1.0;
    const int    denom = std::max(1, n - 1);
    for (int i = 0; i < n; ++i) {
        const double srcf =
            lo + (static_cast<double>(i) / denom) * span;
        int    i0   = static_cast<int>(srcf);
        double frac = srcf - i0;
        if (i0 < 0)              { i0 = 0;             frac = 0.0; }
        if (i0 >= kAnPixels - 1) { i0 = kAnPixels - 2; frac = 1.0; }
        dst[i] = static_cast<float>(full[i0] * (1.0 - frac)
                                    + full[i0 + 1] * frac);
    }
    return n;
}

bool WdspEngine::startAudio()
{
    // HL2 onboard-codec output: no QAudioSink — audio is injected into
    // the EP2 stream instead.  Just arm injection.
    if (hl2Out_) {
        if (hl2AudioEnable_) hl2AudioEnable_(true);
        emitLog(QStringLiteral(
            "[wdsp] audio: HL2 audio jack (AK4951) — EP2 injection, "
            "MUTED, volume %1 dB")
            .arg(static_cast<int>(posToDb(volume_.load()))));
        return true;
    }

    QAudioFormat fmt;
    fmt.setSampleRate(cfg_.outRate);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);

    if (devices_.isEmpty() ||
        deviceIndex_ < 0 || deviceIndex_ >= devices_.size()) {
        emitLog(QStringLiteral(
            "[wdsp] audio: no output device — playback disabled "
            "(dBFS meter still live)"));
        return false;
    }
    const QAudioDevice dev = devices_[deviceIndex_];
    if (!dev.isFormatSupported(fmt)) {
        emitLog(QStringLiteral(
            "[wdsp] audio: '%1' does not support 48 kHz/stereo/int16 — "
            "playback disabled (dBFS meter still live)")
            .arg(dev.description()));
        return false;
    }

    const int capFrames     = cfg_.outRate * kRingMs    / 1000;
    const int prefillFrames = cfg_.outRate * kPrefillMs / 1000;
    {
        std::lock_guard<std::mutex> lk(audioMtx_);
        if (audioSink_) {
            return true;  // already up
        }
        audioRing_ = new AudioRing(capFrames);
        audioRing_->prefillSilence(prefillFrames);
        audioRing_->open(QIODevice::ReadOnly);

        audioSink_ = new QAudioSink(dev, fmt, this);
        // Cap the device buffer so audio latency stays low (else Qt's
        // large default makes audio lag the panadapter).  Must be set
        // before start().
        audioSink_->setBufferSize(
            cfg_.outRate * kSinkBufferMs / 1000 * 2 *
            static_cast<int>(sizeof(qint16)));
        audioSink_->start(audioRing_);   // pull mode
    }

    emitLog(QStringLiteral(
        "[wdsp] audio: '%1' @ 48 kHz stereo int16 (ring %2 ms, "
        "prefill %3 ms) — MUTED, volume %4 dB")
        .arg(dev.description()).arg(kRingMs).arg(kPrefillMs)
        .arg(static_cast<int>(posToDb(volume_.load()))));
    return true;
}

void WdspEngine::stopAudio()
{
    // Always disarm EP2 audio injection (no-op if it was never armed).
    if (hl2AudioEnable_) hl2AudioEnable_(false);
    // sink->stop() quiesces Qt's audio thread (no more readData) before
    // we delete the ring; audioMtx_ blocks the RX worker's push().
    std::lock_guard<std::mutex> lk(audioMtx_);
    if (audioSink_) {
        audioSink_->stop();
        delete audioSink_;
        audioSink_ = nullptr;
    }
    if (audioRing_) {
        audioRing_->close();
        delete audioRing_;
        audioRing_ = nullptr;
    }
}

double WdspEngine::volumeDb() const
{
    return posToDb(volume_.load(std::memory_order_relaxed));
}

double WdspEngine::sMeterDbm() const
{
    if (!running_ || !wdsp_) return -200.0;
    const WdspApi &api = wdsp_->api();
    if (!api.GetRXAMeter) return -200.0;
    // 0 = RXA_S_PK (peak signal strength).  WDSP computes this in the
    // RX worker; GetRXAMeter just reads the latest stored value, so it
    // is safe to poll from the UI thread.
    return api.GetRXAMeter(channel_, 0);
}

double WdspEngine::agcGainDb() const
{
    if (!running_ || !wdsp_) return 0.0;
    const WdspApi &api = wdsp_->api();
    if (!api.GetRXAMeter) return 0.0;
    return api.GetRXAMeter(channel_, 4);   // 4 = RXA_AGC_GAIN (gain action, dB)
}

double WdspEngine::agcThreshDb() const
{
    return kAgcThreshDbFs;                 // fixed first-light threshold
}

void WdspEngine::setVolume(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    volume_.store(v, std::memory_order_relaxed);
    QSettings().setValue(QStringLiteral("audio/volume"), v);
    emit volumeChanged();
}

void WdspEngine::setTxMuted(bool m)
{
    // Live MOX-driven mute — wired in main.cpp from
    // HL2Stream::moxActiveChanged.  No persistence, no settings write
    // (transient TX state, not an operator preference).  The gain calc
    // gates this through autoMuteOnTx_ so the operator's master switch
    // takes effect immediately if they toggle it mid-TX.
    const bool prev = txMuted_.exchange(m, std::memory_order_relaxed);
    if (prev != m) emit txMutedChanged();
}

void WdspEngine::setAutoMuteOnTx(bool on)
{
    const bool prev = autoMuteOnTx_.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    QSettings().setValue(QStringLiteral("audio/autoMuteOnTx"), on);
    emit autoMuteOnTxChanged();
    // If the operator flips this OFF while currently transmitting, the
    // change takes effect on the NEXT audio block — the gain calc reads
    // both atomics fresh, so the gate releases without an audible pop
    // (the volume taper handles the transition).  Symmetric on flip ON.
}

void WdspEngine::setMuted(bool m)
{
    muted_.store(m, std::memory_order_relaxed);
    QSettings().setValue(QStringLiteral("audio/muted"), m);
    emit mutedChanged();
    emitLog(m ? QStringLiteral("[wdsp] audio: muted")
              : QStringLiteral("[wdsp] audio: unmuted (volume %1 dB)")
                    .arg(static_cast<int>(posToDb(volume_.load()))));
}

void WdspEngine::setAfGainDb(double db)
{
    db = std::clamp(db, 0.0, 40.0);
    if (std::abs(db - afGainDb_) < 1e-6) return;
    afGainDb_ = db;
    QSettings().setValue(QStringLiteral("audio/afGainDb"), db);
    {   // push to WDSP live (serialise against feedIq's fexchange0)
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (opened_ && wdsp_ && wdsp_->api().SetRXAPanelGain1)
            wdsp_->api().SetRXAPanelGain1(channel_, std::pow(10.0, db / 20.0));
    }
    emit afGainChanged();
}

void WdspEngine::setBalance(double b)
{
    b = std::clamp(b, -1.0, 1.0);
    balance_.store(b, std::memory_order_relaxed);   // applied in feedIq
    QSettings().setValue(QStringLiteral("audio/balance"), b);
    emit balanceChanged();
}

// ── RX DSP operator controls ──────────────────────────────────────
// Each setter: clamp/validate, store, persist (QSettings), push to
// WDSP when the channel is open, emit NOTIFY.  The push helpers below
// are channel-parameterized (use channel_) so RX2 reuses them verbatim.

void WdspEngine::pushNrState()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetRXAEMNRRun) return;   // EMNR not resolved -> nothing to push
    // Mode 1..4 (UI) -> WDSP gain_method 0..3.
    if (api.SetRXAEMNRgainMethod)
        api.SetRXAEMNRgainMethod(channel_, std::clamp(nrMode_, 1, 4) - 1);
    if (api.SetRXAEMNRnpeMethod)
        api.SetRXAEMNRnpeMethod(channel_, std::clamp(npeMethod_, 0, 1));
    if (api.SetRXAEMNRaeRun)
        api.SetRXAEMNRaeRun(channel_, aepfEnabled_ ? 1 : 0);
    // AEPF also engages WDSP's post-filter ("post2") — the dedicated
    // anti-musical-noise stage that stock WDSP leaves off.  Params stay
    // at WDSP's gentle create defaults (0.15/0.15/5.0/0.12); MMSE-LSA
    // upstream keeps the voice natural, so the extra stage removes
    // musical artifacts without a robotic character.
    if (api.SetRXAEMNRpost2Run)
        api.SetRXAEMNRpost2Run(channel_, aepfEnabled_ ? 1 : 0);
    if (api.SetRXAEMNRPosition)
        api.SetRXAEMNRPosition(channel_, 1);   // after AGC (standard position)
    api.SetRXAEMNRRun(channel_, nrEnabled_ ? 1 : 0);
}

void WdspEngine::pushAgcMode()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetRXAAGCMode) return;
    int mode = kAgcModeMed;
    if      (agcMode_ == QLatin1String("off"))  mode = kAgcModeOff;
    else if (agcMode_ == QLatin1String("fast")) mode = kAgcModeFast;
    else if (agcMode_ == QLatin1String("slow")) mode = kAgcModeSlow;
    else                                        mode = kAgcModeMed;
    api.SetRXAAGCMode(channel_, mode);
    // Set the time constants EXPLICITLY per mode (the standard per-mode
    // values) so Fast/Med/Slow are unmistakably distinct — the audible
    // difference is mostly the HANG (Fast/Med = none, Slow = 1 s hold)
    // plus the decay rate.  SetRXAAGCMode sets WDSP internal defaults
    // too, but pushing them ourselves removes any doubt about the values.
    // (Off = FIXD/fixed gain — decay/hang are irrelevant, left alone.)
    if (mode != kAgcModeOff) {
        int decayMs = 250, hangMs = 0, hangThr = 100;   // med
        if (mode == kAgcModeFast)      { decayMs =  50; hangMs =    0; hangThr = 100; }
        else if (mode == kAgcModeSlow) { decayMs = 500; hangMs = 1000; hangThr =   0; }
        if (api.SetRXAAGCDecay)         api.SetRXAAGCDecay(channel_, decayMs);
        if (api.SetRXAAGCHang)          api.SetRXAAGCHang(channel_, hangMs);
        if (api.SetRXAAGCHangThreshold) api.SetRXAAGCHangThreshold(channel_, hangThr);
    }
}

void WdspEngine::setNrEnabled(bool on)
{
    if (nrEnabled_ == on) return;
    nrEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/nrEnabled"), on);
    pushNrState();
    emit nrChanged();
    emitLog(QStringLiteral("[wdsp] NR %1 (Mode %2)")
                .arg(on ? QStringLiteral("on") : QStringLiteral("off"))
                .arg(nrMode_));
}

void WdspEngine::setNrMode(int mode)
{
    mode = std::clamp(mode, 1, 4);
    if (nrMode_ == mode) return;
    nrMode_ = mode;
    QSettings().setValue(QStringLiteral("dsp/nrMode"), mode);
    pushNrState();
    emit nrChanged();
}

void WdspEngine::setAepfEnabled(bool on)
{
    if (aepfEnabled_ == on) return;
    aepfEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/aepf"), on);
    pushNrState();
    emit nrChanged();
}

void WdspEngine::setNpeMethod(int method)
{
    method = std::clamp(method, 0, 1);
    if (npeMethod_ == method) return;
    npeMethod_ = method;
    QSettings().setValue(QStringLiteral("dsp/npeMethod"), method);
    pushNrState();
    emit nrChanged();
}

void WdspEngine::setAgcMode(const QString &mode)
{
    const QString m = mode.toLower();
    if (m != QLatin1String("off") && m != QLatin1String("fast") &&
        m != QLatin1String("slow") && m != QLatin1String("med"))
        return;   // ignore unknown values
    if (agcMode_ == m) return;
    agcMode_ = m;
    QSettings().setValue(QStringLiteral("dsp/agcMode"), m);
    pushAgcMode();
    emit agcModeChanged();
    emitLog(QStringLiteral("[wdsp] AGC mode %1").arg(m));
}

void WdspEngine::pushAnfState()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetRXAANFRun) return;
    if (api.SetRXAANFVals)            // carrier-null defaults (taps/delay/gain/leak)
        api.SetRXAANFVals(channel_, 64, 16, 1.0e-3, 1.0e-7);
    api.SetRXAANFRun(channel_, anfEnabled_ ? 1 : 0);
}

void WdspEngine::pushLmsState()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetRXAANRRun) return;
    if (api.SetRXAANRVals) {
        // strength 0..1 -> taps 32..128 + adapt rate (gain) 8e-5..16e-4.
        const double s    = std::clamp(lmsStrength_, 0.0, 1.0);
        const int    taps = 32 + static_cast<int>(std::lround(96.0 * s));
        const double gain = 8.0e-5 + (16.0e-4 - 8.0e-5) * s;
        api.SetRXAANRVals(channel_, taps, 16, gain, 1.0e-7);
    }
    api.SetRXAANRRun(channel_, lmsEnabled_ ? 1 : 0);
}

void WdspEngine::setAnfEnabled(bool on)
{
    if (anfEnabled_ == on) return;
    anfEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/anfEnabled"), on);
    pushAnfState();
    emit anfChanged();
    emitLog(QStringLiteral("[wdsp] ANF %1")
                .arg(on ? QStringLiteral("on") : QStringLiteral("off")));
}

void WdspEngine::setLmsEnabled(bool on)
{
    if (lmsEnabled_ == on) return;
    lmsEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/lmsEnabled"), on);
    pushLmsState();
    emit lmsChanged();
    emitLog(QStringLiteral("[wdsp] LMS %1 (strength %2%)")
                .arg(on ? QStringLiteral("on") : QStringLiteral("off"))
                .arg(static_cast<int>(lmsStrength_ * 100.0 + 0.5)));
}

void WdspEngine::setLmsStrength(double s)
{
    s = std::clamp(s, 0.0, 1.0);
    if (std::abs(s - lmsStrength_) < 1e-6) return;
    lmsStrength_ = s;
    QSettings().setValue(QStringLiteral("dsp/lmsStrength"), s);
    pushLmsState();   // re-push taps/gain even while enabled (live tune)
    emit lmsChanged();
}

// ── Manual notches (NBP database) ─────────────────────────────────

QVariantList WdspEngine::notches() const
{
    QVariantList out;
    for (const Notch &n : notches_) {
        QVariantMap m;
        m[QStringLiteral("offsetHz")] = n.offsetHz;
        m[QStringLiteral("widthHz")]  = n.widthHz;
        m[QStringLiteral("active")]   = n.active;
        out.append(m);
    }
    return out;
}

void WdspEngine::persistNotches()
{
    QStringList items;
    items.reserve(static_cast<int>(notches_.size()));
    for (const Notch &n : notches_)
        items << QStringLiteral("%1|%2|%3")
                     .arg(n.offsetHz).arg(n.widthHz).arg(n.active ? 1 : 0);
    QSettings().setValue(QStringLiteral("dsp/notches"), items);
}

void WdspEngine::pushNotches()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.RXANBPAddNotch || !api.RXANBPSetNotchesRun) return;
    // Rebuild the WDSP DB from the Lyra list (the DB is recreated empty
    // on every channel open).  Delete existing high->low to avoid the
    // index reshuffle that deleting index 0 repeatedly would cause.
    if (api.RXANBPDeleteNotch) {
        for (int i = wdspNotchCount_ - 1; i >= 0; --i)
            api.RXANBPDeleteNotch(channel_, i);
    }
    for (int i = 0; i < static_cast<int>(notches_.size()); ++i) {
        const Notch &n = notches_[i];
        api.RXANBPAddNotch(channel_, i, n.offsetHz, n.widthHz,
                           n.active ? 1 : 0);
    }
    wdspNotchCount_ = static_cast<int>(notches_.size());
    api.RXANBPSetNotchesRun(
        channel_, (notchEnabled_ && !notches_.empty()) ? 1 : 0);
}

void WdspEngine::setNotchEnabled(bool on)
{
    if (notchEnabled_ == on) return;
    notchEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/notchEnabled"), on);
    pushNotches();
    emit notchesChanged();
}

int WdspEngine::addNotch(double offsetHz, double widthHz)
{
    notches_.push_back({offsetHz, std::max(10.0, widthHz), true});
    notchEnabled_ = true;   // dropping a notch implies NF on
    QSettings().setValue(QStringLiteral("dsp/notchEnabled"), true);
    persistNotches();
    pushNotches();
    emit notchesChanged();
    return static_cast<int>(notches_.size()) - 1;
}

void WdspEngine::removeNotch(int index)
{
    if (index < 0 || index >= static_cast<int>(notches_.size())) return;
    notches_.erase(notches_.begin() + index);
    persistNotches();
    pushNotches();
    emit notchesChanged();
}

void WdspEngine::moveNotch(int index, double offsetHz)
{
    if (index < 0 || index >= static_cast<int>(notches_.size())) return;
    notches_[index].offsetHz = offsetHz;
    persistNotches();
    pushNotches();
    emit notchesChanged();
}

void WdspEngine::setNotchWidth(int index, double widthHz)
{
    if (index < 0 || index >= static_cast<int>(notches_.size())) return;
    notches_[index].widthHz = std::clamp(widthHz, 10.0, 20000.0);
    persistNotches();
    pushNotches();
    emit notchesChanged();
}

void WdspEngine::carveNotches(float *db, int n, double floorDb) const
{
    if (!db || n < 2 || !notchEnabled_ || notches_.empty()) return;
    const double span = static_cast<double>(spanHz());
    if (span <= 0.0) return;
    const float floorF = static_cast<float>(floorDb);
    for (const Notch &nt : notches_) {
        if (!nt.active) continue;
        const double cCol  = (nt.offsetHz / span + 0.5) * (n - 1);
        double       halfW = 0.5 * (nt.widthHz / span) * (n - 1);
        if (halfW < 0.75) halfW = 0.75;             // ≥ ~1.5 columns visible
        int lo = static_cast<int>(std::floor(cCol - halfW));
        int hi = static_cast<int>(std::ceil (cCol + halfW));
        if (lo < 0)      lo = 0;
        if (hi > n - 1)  hi = n - 1;
        for (int i = lo; i <= hi; ++i)
            if (db[i] > floorF) db[i] = floorF;     // clamp down to floor
    }
}

int WdspEngine::notchNear(double offsetHz, double tolHz) const
{
    int best = -1;
    double bestD = tolHz;
    for (int i = 0; i < static_cast<int>(notches_.size()); ++i) {
        const double d = std::abs(notches_[i].offsetHz - offsetHz);
        if (d <= bestD) { bestD = d; best = i; }
    }
    return best;
}

// ── All-mode squelch (SSQL / FMSQ / AMSQ, routed by mode) ─────────

void WdspEngine::pushSquelchState()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    const bool fm  = (mode_ == QLatin1String("FM"));
    const bool am  = (mode_ == QLatin1String("AM") ||
                      mode_ == QLatin1String("SAM") ||
                      mode_ == QLatin1String("DSB"));
    const bool ssb = !fm && !am;   // USB/LSB/CWU/CWL/DIGU/DIGL/SPEC
    const bool on  = squelchEnabled_;
    const double t = std::clamp(squelchThreshold_, 0.0, 1.0);

    // SSQL — SSB/CW/DIG voice-presence squelch.  *0.65 scale puts the
    // WU2O-tested-good default (~0.16) at a comfortable slider zone; the
    // tau pair gives a snappy unmute + a hang that doesn't clamp between
    // syllables (bench-tunable).
    if (api.SetRXASSQLRun) {
        if (api.SetRXASSQLTauMute)   api.SetRXASSQLTauMute(channel_, 0.7);
        if (api.SetRXASSQLTauUnMute) api.SetRXASSQLTauUnMute(channel_, 0.1);
        if (api.SetRXASSQLThreshold) api.SetRXASSQLThreshold(channel_, t * 0.65);
        api.SetRXASSQLRun(channel_, (on && ssb) ? 1 : 0);
    }
    // FM squelch (noise-level threshold; log map so the slider feels even).
    if (api.SetRXAFMSQRun) {
        if (api.SetRXAFMSQThreshold)
            api.SetRXAFMSQThreshold(channel_, std::pow(10.0, -2.0 * t));
        api.SetRXAFMSQRun(channel_, (on && fm) ? 1 : 0);
    }
    // AM squelch (carrier-level threshold, ~-160..-30 dB; short tail).
    if (api.SetRXAAMSQRun) {
        if (api.SetRXAAMSQMaxTail)   api.SetRXAAMSQMaxTail(channel_, 0.5);
        if (api.SetRXAAMSQThreshold)
            api.SetRXAAMSQThreshold(channel_, -160.0 + t * 130.0);
        api.SetRXAAMSQRun(channel_, (on && am) ? 1 : 0);
    }
}

void WdspEngine::setSquelchEnabled(bool on)
{
    if (squelchEnabled_ == on) return;
    squelchEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/squelchEnabled"), on);
    pushSquelchState();
    emit squelchChanged();
    emitLog(QStringLiteral("[wdsp] squelch %1")
                .arg(on ? QStringLiteral("on") : QStringLiteral("off")));
}

void WdspEngine::setSquelchThreshold(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    if (std::abs(t - squelchThreshold_) < 1e-6) return;
    squelchThreshold_ = t;
    QSettings().setValue(QStringLiteral("dsp/squelchThreshold"), t);
    pushSquelchState();
    emit squelchChanged();
}

// ── Noise blanker (EXT NOB-II) ────────────────────────────────────

void WdspEngine::pushNbState()
{
    if (!opened_ || !nbCreated_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (api.SetEXTNOBThreshold) {
        // strength 0..1 -> threshold 12..2.5 (light..heavy).  LOWER
        // threshold = more aggressive blanking; clamp to the working
        // 1.5..50 range (light≈10, heavy≈3 ported from the Python tree).
        double th = 12.0 - 9.5 * std::clamp(nbStrength_, 0.0, 1.0);
        th = std::clamp(th, 1.5, 50.0);
        api.SetEXTNOBThreshold(channel_, th);
    }
    if (api.SetEXTNOBRun)
        api.SetEXTNOBRun(channel_, nbEnabled_ ? 1 : 0);
}

void WdspEngine::setNbEnabled(bool on)
{
    if (nbEnabled_ == on) return;
    nbEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/nbEnabled"), on);
    pushNbState();
    emit nbChanged();
    emitLog(QStringLiteral("[wdsp] NB %1 (strength %2%)")
                .arg(on ? QStringLiteral("on") : QStringLiteral("off"))
                .arg(static_cast<int>(nbStrength_ * 100.0 + 0.5)));
}

void WdspEngine::setNbStrength(double s)
{
    s = std::clamp(s, 0.0, 1.0);
    if (std::abs(s - nbStrength_) < 1e-6) return;
    nbStrength_ = s;
    QSettings().setValue(QStringLiteral("dsp/nbStrength"), s);
    pushNbState();   // re-tune threshold live
    emit nbChanged();
}

// ── APF (CW audio peaking filter — in-chain biquad) ───────────────

void WdspEngine::pushApfState()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetRXABiQuadRun) return;
    const bool cw = (mode_ == QLatin1String("CWU") ||
                     mode_ == QLatin1String("CWL"));
    // Peak centred on the CW pitch, ~75 Hz wide, operator-set gain (dB→linear).
    if (api.SetRXABiQuadFreq)      api.SetRXABiQuadFreq(channel_, cwPitchHz_);
    if (api.SetRXABiQuadBandwidth) api.SetRXABiQuadBandwidth(channel_, 75.0);
    if (api.SetRXABiQuadGain)
        api.SetRXABiQuadGain(channel_, std::pow(10.0, apfGainDb_ / 20.0));
    api.SetRXABiQuadRun(channel_, (apfEnabled_ && cw) ? 1 : 0);
}

void WdspEngine::setApfEnabled(bool on)
{
    if (apfEnabled_ == on) return;
    apfEnabled_ = on;
    QSettings().setValue(QStringLiteral("dsp/apfEnabled"), on);
    pushApfState();
    emit apfChanged();
    emitLog(QStringLiteral("[wdsp] APF %1")
                .arg(on ? QStringLiteral("on") : QStringLiteral("off")));
}

void WdspEngine::setApfGainDb(double db)
{
    db = std::clamp(db, 0.0, 24.0);
    if (std::abs(db - apfGainDb_) < 1e-6) return;
    apfGainDb_ = db;
    QSettings().setValue(QStringLiteral("dsp/apfGainDb"), db);
    pushApfState();   // re-apply the biquad gain live
    emit apfChanged();
}

// ── BIN — binaural pseudo-stereo (Lyra-native Hilbert post-proc) ──

void WdspEngine::buildBinaural()
{
    constexpr double kPi = 3.14159265358979323846;
    const int N = 63;                       // taps (group delay 31)
    binH_.assign(static_cast<size_t>(N), 0.0);
    const int n = (N - 1) / 2;
    // Truncated 2/(pi*k) Hilbert response (odd k), Hamming-windowed.
    for (int k = -n; k <= n; ++k) {
        if (k != 0 && (k % 2) != 0)
            binH_[static_cast<size_t>(k + n)] = 2.0 / (kPi * k);
    }
    for (int i = 0; i < N; ++i)
        binH_[static_cast<size_t>(i)] *=
            0.54 - 0.46 * std::cos(2.0 * kPi * i / (N - 1));
    binHist_.assign(static_cast<size_t>(N), 0.0);
    binPos_ = 0;
}

void WdspEngine::resetBinaural()
{
    std::fill(binHist_.begin(), binHist_.end(), 0.0);
    binPos_ = 0;
}

void WdspEngine::binauralStep(double mono, double *l, double *r)
{
    const int N = static_cast<int>(binHist_.size());   // 63
    binHist_[static_cast<size_t>(binPos_)] = mono;
    // Hilbert-shifted = FIR conv over the history (newest at binPos_).
    double shifted = 0.0;
    int idx = binPos_;
    for (int k = 0; k < N; ++k) {
        shifted += binH_[static_cast<size_t>(k)] *
                   binHist_[static_cast<size_t>(idx)];
        if (--idx < 0) idx = N - 1;
    }
    // In-phase path delayed by the group delay (31) to align with shifted.
    int didx = binPos_ - 31;
    if (didx < 0) didx += N;
    const double delayed = binHist_[static_cast<size_t>(didx)];
    if (++binPos_ >= N) binPos_ = 0;
    // Sum/diff mix + equal-loudness normalization (d=0 → mono).
    const double d  = binDepth_;
    const double ds = d * shifted;
    const double norm = 1.0 / std::sqrt(1.0 + d * d);
    *l = (delayed - ds) * norm;
    *r = (delayed + ds) * norm;
}

void WdspEngine::setBinEnabled(bool on)
{
    if (binEnabled_ == on) return;
    binEnabled_ = on;
    if (on) resetBinaural();   // clean state on engage (click-free)
    QSettings().setValue(QStringLiteral("dsp/binEnabled"), on);
    emit binChanged();
    emitLog(QStringLiteral("[wdsp] BIN %1")
                .arg(on ? QStringLiteral("on") : QStringLiteral("off")));
}

void WdspEngine::setBinDepth(double d)
{
    d = std::clamp(d, 0.0, 1.0);
    if (std::abs(d - binDepth_) < 1e-6) return;
    binDepth_ = d;
    QSettings().setValue(QStringLiteral("dsp/binDepth"), d);
    emit binChanged();
}

void WdspEngine::startNoiseCapture(double seconds)
{
    // Hold channelMtx_ so we can't arm/reset the profile while feedIq is
    // mid-block on the RX worker thread (it feeds noiseProfile_ under the
    // same lock).  (Re)create the profile if its FFT size changed.
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (!noiseProfile_ || noiseProfile_->fftSize() != npFftSize_) {
            noiseProfile_ = std::make_unique<CapturedProfile>(npFftSize_);
        }
        noiseProfile_->begin(cfg_.inRate, seconds);
        noiseProfileValid_.store(false, std::memory_order_relaxed);
        noiseProgress_.store(0.0, std::memory_order_relaxed);
        noiseCapturing_.store(true, std::memory_order_relaxed);
    }
    emitLog(QStringLiteral("Noise capture: %1 s @ %2 Hz, FFT %3")
                .arg(seconds).arg(cfg_.inRate).arg(npFftSize_));
    emit noiseCaptureChanged();
}

void WdspEngine::cancelNoiseCapture()
{
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        noiseCapturing_.store(false, std::memory_order_relaxed);
        if (noiseProfile_) noiseProfile_->cancel();
    }
    emit noiseCaptureChanged();
}

void WdspEngine::setNoiseApply(bool on)
{
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (on) {
            // (Re)create the reducer at the current captured-profile FFT
            // size and load the current profile if one is valid + matches.
            if (!reducer_ || reducer_->fftSize() != npFftSize_) {
                reducer_ = std::make_unique<NoiseReducer>(npFftSize_);
            }
            applyReducerParams();
            if (noiseProfile_ && noiseProfile_->valid() &&
                noiseProfile_->fftSize() == npFftSize_) {
                reducer_->setProfile(noiseProfile_->noisePower());
            }
        }
        applyEnabled_.store(on, std::memory_order_relaxed);
    }
    emitLog(on ? QStringLiteral("Noise apply: ON")
               : QStringLiteral("Noise apply: OFF"));
    emit noiseApplyChanged();
}

void WdspEngine::applyReducerParams()
{
    // Caller holds channelMtx_.  Pushes the persisted tunables onto the
    // reducer (no-op if it doesn't exist yet).
    if (!reducer_) return;
    reducer_->setAlpha(npAlpha_);
    reducer_->setFloorDb(npFloorDb_);
    reducer_->setSmoothing(npSmoothing_);
}

void WdspEngine::setNoiseStrength(double a)
{
    a = std::clamp(a, 1.0, 5.0);
    if (std::abs(a - npAlpha_) < 1e-6) return;
    npAlpha_ = a;
    QSettings().setValue(QStringLiteral("dsp/npAlpha"), a);
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (reducer_) reducer_->setAlpha(a);
    }
    emit noiseTuningChanged();
}

void WdspEngine::setNoiseFloorDb(double db)
{
    db = std::clamp(db, -30.0, -3.0);
    if (std::abs(db - npFloorDb_) < 1e-6) return;
    npFloorDb_ = db;
    QSettings().setValue(QStringLiteral("dsp/npFloorDb"), db);
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (reducer_) reducer_->setFloorDb(db);
    }
    emit noiseTuningChanged();
}

void WdspEngine::setNoiseSmoothing(double sm)
{
    sm = std::clamp(sm, 0.0, 0.95);
    if (std::abs(sm - npSmoothing_) < 1e-6) return;
    npSmoothing_ = sm;
    QSettings().setValue(QStringLiteral("dsp/npSmoothing"), sm);
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (reducer_) reducer_->setSmoothing(sm);
    }
    emit noiseTuningChanged();
}

QStringList WdspEngine::noiseProfiles() const
{
    QStringList names;
    for (const auto &p : profiles_) names << p.name;
    return names;
}

QString WdspEngine::noiseProfileInfo(const QString &name) const
{
    for (const auto &p : profiles_) {
        if (p.name == name) {
            const QString r = (p.rate % 1000 == 0)
                ? QStringLiteral("%1k").arg(p.rate / 1000)
                : QString::number(p.rate);
            return QStringLiteral("%1 · %2 · %3").arg(r).arg(p.fft).arg(p.date);
        }
    }
    return QString();
}

void WdspEngine::setNoiseFftSize(int n)
{
    if (n != 2048 && n != 4096 && n != 8192) return;
    if (n == npFftSize_) return;
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        npFftSize_ = n;
        // A profile is FFT-size-specific: drop the live capture, the
        // reducer, and apply — the operator re-captures at the new size.
        noiseCapturing_.store(false, std::memory_order_relaxed);
        if (noiseProfile_) noiseProfile_->cancel();
        noiseProfileValid_.store(false, std::memory_order_relaxed);
        reducer_.reset();
        applyEnabled_.store(false, std::memory_order_relaxed);
        npActiveName_.clear();
    }
    QSettings().setValue(QStringLiteral("dsp/npFftSize"), n);
    emit noiseSettingsChanged();
    emit noiseCaptureChanged();
    emit noiseApplyChanged();
    emit noiseProfilesChanged();
}

void WdspEngine::setNoiseCaptureSeconds(double sec)
{
    if (sec != 3.0 && sec != 5.0 && sec != 10.0) return;
    if (sec == npCaptureSeconds_) return;
    npCaptureSeconds_ = sec;
    QSettings().setValue(QStringLiteral("dsp/npCaptureSeconds"), sec);
    emit noiseSettingsChanged();
}

QString WdspEngine::profilesDir() const
{
    // Beside the FFT-wisdom folder (wdsp_native): N8SDR/Lyra-cpp/profiles
    // under the platform's generic data location.  Created on demand.
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    const QString dir = QDir::cleanPath(
        base + QStringLiteral("/N8SDR/Lyra-cpp/profiles"));
    QDir().mkpath(dir);
    return dir;
}

QString WdspEngine::noiseProfilesDir() const
{
    return QDir::toNativeSeparators(profilesDir());
}

bool WdspEngine::saveNoiseProfile(const QString &name)
{
    const QString nm = name.trimmed();
    if (nm.isEmpty()) return false;
    StoredProfile sp;
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (!noiseProfile_ || !noiseProfile_->valid()) return false;
        sp.rate  = noiseProfile_->sampleRate();
        sp.fft   = noiseProfile_->fftSize();
        sp.power = noiseProfile_->noisePower();
    }
    sp.name = nm;
    sp.date = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd"));
    // Overwrite a same-named profile (reuse its file), else append.
    StoredProfile *slot = nullptr;
    for (auto &p : profiles_) {
        if (p.name == nm) { sp.file = p.file; p = sp; slot = &p; break; }
    }
    if (!slot) { profiles_.push_back(sp); slot = &profiles_.back(); }
    if (!writeProfileFile(*slot)) return false;
    npActiveName_ = nm;
    emit noiseProfilesChanged();
    return true;
}

bool WdspEngine::loadNoiseProfile(const QString &name)
{
    const StoredProfile *found = nullptr;
    for (const auto &p : profiles_) {
        if (p.name == name) { found = &p; break; }
    }
    if (!found) return false;
    if (found->rate != cfg_.inRate) {
        emitLog(QStringLiteral("Profile '%1' was captured at %2 Hz; current "
                               "rate is %3 Hz — recapture at this rate.")
                    .arg(name).arg(found->rate).arg(cfg_.inRate));
        return false;   // rate mismatch — caller surfaces the hint
    }
    {
        std::lock_guard<std::mutex> lk(channelMtx_);
        if (found->fft != npFftSize_) {
            npFftSize_ = found->fft;   // adopt the profile's FFT size
            QSettings().setValue(QStringLiteral("dsp/npFftSize"), npFftSize_);
        }
        if (!reducer_ || reducer_->fftSize() != npFftSize_) {
            reducer_ = std::make_unique<NoiseReducer>(npFftSize_);
        }
        applyReducerParams();
        reducer_->setProfile(found->power);
        npActiveName_ = name;
    }
    emit noiseSettingsChanged();
    emit noiseProfilesChanged();
    return true;
}

void WdspEngine::deleteNoiseProfile(const QString &name)
{
    for (auto it = profiles_.begin(); it != profiles_.end(); ++it) {
        if (it->name == name) {
            removeProfileFile(*it);
            profiles_.erase(it);
            if (npActiveName_ == name) npActiveName_.clear();
            emit noiseProfilesChanged();
            return;
        }
    }
}

bool WdspEngine::promptSaveProfile()
{
    if (!noiseProfileValid_.load(std::memory_order_relaxed)) {
        QMessageBox::information(nullptr, tr("Save noise profile"),
            tr("Capture a profile first — press 📷 Cap on a quiet, "
               "signal-free frequency."));
        return false;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        nullptr, tr("Save noise profile"),
        tr("Name (e.g. 40m-night, 20m-ESSB):"),
        QLineEdit::Normal, npActiveName_, &ok);
    if (!ok || name.trimmed().isEmpty()) return false;
    return saveNoiseProfile(name);
}

void WdspEngine::loadProfileOrWarn(const QString &name)
{
    if (!loadNoiseProfile(name)) {
        QMessageBox::information(nullptr, tr("Profile not loaded"),
            tr("“%1” was captured at a different sample rate than the "
               "radio is using now.\nSwitch to that rate, or recapture at the "
               "current rate.").arg(name));
    }
}

bool WdspEngine::renameNoiseProfile(const QString &oldName, const QString &newName)
{
    const QString nn = newName.trimmed();
    if (nn.isEmpty() || nn == oldName) return false;
    StoredProfile *p = nullptr;
    for (auto &q : profiles_) if (q.name == oldName) { p = &q; break; }
    if (!p) return false;
    for (const auto &q : profiles_)            // reject collision
        if (&q != p && q.name == nn) return false;
    const QString oldFile = p->file;
    p->name = nn;
    p->file.clear();                           // force a new filename
    if (!writeProfileFile(*p)) { p->name = oldName; p->file = oldFile; return false; }
    if (!oldFile.isEmpty() && oldFile != p->file) QFile::remove(oldFile);
    if (npActiveName_ == oldName) npActiveName_ = nn;
    emit noiseProfilesChanged();
    return true;
}

void WdspEngine::loadProfiles()
{
    profiles_.clear();
    const QDir dir(profilesDir());
    const QStringList files = dir.entryList(
        QStringList{QStringLiteral("*.lnp")}, QDir::Files, QDir::Name);
    for (const QString &fn : files) {
        const QString path = dir.filePath(fn);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QDataStream ds(&f);
        ds.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0, ver = 0;
        ds >> magic >> ver;
        if (magic != 0x4C4E5031u) continue;        // "LNP1"
        StoredProfile sp;
        qint32 rate = 0, fft = 0;
        QByteArray pw;
        ds >> sp.name >> rate >> fft >> sp.date >> pw;
        if (ds.status() != QDataStream::Ok) continue;
        sp.rate = rate; sp.fft = fft; sp.file = path;
        if (sp.name.isEmpty() || fft <= 0) continue;
        if (pw.size() == static_cast<int>(fft * sizeof(double))) {
            sp.power.resize(static_cast<size_t>(fft));
            std::memcpy(sp.power.data(), pw.constData(),
                        static_cast<size_t>(pw.size()));
            profiles_.push_back(std::move(sp));
        }
    }
}

bool WdspEngine::writeProfileFile(StoredProfile &p)
{
    if (p.file.isEmpty()) {
        // Derive a safe, unique filename from the (operator) name.
        QString safe;
        for (const QChar c : p.name)
            safe += (c.isLetterOrNumber() || c == QLatin1Char('_') ||
                     c == QLatin1Char('-') || c == QLatin1Char(' '))
                        ? c : QLatin1Char('_');
        safe = safe.trimmed();
        if (safe.isEmpty()) safe = QStringLiteral("profile");
        const QString dir = profilesDir();
        QString cand = QDir(dir).filePath(safe + QStringLiteral(".lnp"));
        int n = 2;
        while (QFileInfo::exists(cand)) {
            bool ownedByOther = false;     // collision with a DIFFERENT profile
            for (const auto &q : profiles_)
                if (&q != &p && q.file == cand) { ownedByOther = true; break; }
            if (!ownedByOther) break;      // free / ours -> overwrite is fine
            cand = QDir(dir).filePath(
                QStringLiteral("%1_%2.lnp").arg(safe).arg(n++));
        }
        p.file = cand;
    }
    QFile f(p.file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emitLog(QStringLiteral("Profile save FAILED — cannot write %1").arg(p.file));
        return false;
    }
    QDataStream ds(&f);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << quint32(0x4C4E5031u) << quint32(1)
       << p.name << qint32(p.rate) << qint32(p.fft) << p.date;
    const QByteArray pw(reinterpret_cast<const char *>(p.power.data()),
                        static_cast<int>(p.power.size() * sizeof(double)));
    ds << pw;
    f.close();
    emitLog(QStringLiteral("Profile saved → %1").arg(p.file));
    return true;
}

void WdspEngine::removeProfileFile(const StoredProfile &p)
{
    if (!p.file.isEmpty()) QFile::remove(p.file);
}

void WdspEngine::setHl2AudioSink(
        std::function<void(const qint16 *, int)> push,
        std::function<void(bool)> enable)
{
    hl2AudioPush_   = std::move(push);
    hl2AudioEnable_ = std::move(enable);
}

QStringList WdspEngine::audioOutputDevices() const
{
    // Index 0 is always the HL2 onboard codec (old Lyra's default HL2
    // path); the operator's PC output devices follow.
    QStringList names;
    names << QStringLiteral("HL2 audio jack (AK4951)");
    for (const QAudioDevice &d : devices_) {
        names << d.description();
    }
    return names;
}

void WdspEngine::setAudioOutputDevice(int index)
{
    if (index == 0) {
        // HL2 onboard codec: stop the PC sink, route audio to EP2.
        if (!hl2Out_) {
            hl2Out_ = true;
            QSettings().setValue(QStringLiteral("audio/output"),
                                 QStringLiteral("hl2"));
            if (running_) stopAudio();          // drop the QAudioSink
            if (hl2AudioEnable_) hl2AudioEnable_(true);
            emit audioDeviceChanged();
            emitLog(QStringLiteral(
                "[wdsp] audio: output -> HL2 audio jack (AK4951)"));
        }
        return;
    }
    const int devIdx = index - 1;   // PC device list is offset by 1
    if (devIdx < 0 || devIdx >= devices_.size()) {
        return;
    }
    const bool wasHl2 = hl2Out_;
    if (!wasHl2 && devIdx == deviceIndex_) {
        return;   // no change
    }
    hl2Out_      = false;
    deviceIndex_ = devIdx;
    if (hl2AudioEnable_) hl2AudioEnable_(false);   // stop EP2 injection
    QSettings().setValue(QStringLiteral("audio/output"),
                         QStringLiteral("pc"));
    QSettings().setValue(QStringLiteral("audio/deviceName"),
                         devices_[devIdx].description());
    emit audioDeviceChanged();
    emitLog(QStringLiteral("[wdsp] audio: output device -> %1")
                .arg(devices_[devIdx].description()));
    // Live switch: (re)build the sink on the chosen device.
    if (running_) {
        stopAudio();
        startAudio();
    }
}

void WdspEngine::feedIq(const double *iq, int nframes)
{
    // Drop IQ until the channel is live (e.g. samples arriving in the
    // window between stream-open and the deferred openRx1, or after a
    // close).  fexchange0 on a closed channel is undefined.
    if (!running_ || nframes <= 0) {
        return;
    }
    const WdspApi &api = wdsp_->api();
    if (!api.fexchange0) {
        return;
    }

    // Serialise against a main-thread channel reopen (sample-rate
    // switch): hold channelMtx_ for the whole process so the channel +
    // outBuf_ can't be torn down / resized mid-fexchange0.  Re-check the
    // state under the lock (a reopen may have just toggled it).
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!running_ || !opened_) {
        return;
    }

    // Append this datagram's interleaved IQ to the accumulator.
    accum_.insert(accum_.end(), iq,
                  iq + static_cast<size_t>(2 * nframes));

    // Drain whole in_size blocks through WDSP.  in_size frames =
    // 2*in_size interleaved doubles in, 2*outSize_ doubles out.
    const size_t blockDoubles = static_cast<size_t>(2 * cfg_.inSize);
    while (accum_.size() >= blockDoubles) {
        // Noise blanker (EXT): splice the impulse blanker on the raw IQ
        // block BEFORE WDSP's RXA chain — xnobEXT cleans accum_ front into
        // nbBuf_, and the cleaned block then drives BOTH the demod and the
        // analyzer so audio + panadapter match.  NB off → use accum_.
        double *blockPtr = accum_.data();
        if (nbEnabled_ && nbCreated_ && api.xnobEXT &&
            nbBuf_.size() >= blockDoubles) {
            api.xnobEXT(channel_, accum_.data(), nbBuf_.data());
            blockPtr = nbBuf_.data();
        }
        // Captured noise profile (slice 2): during a capture window,
        // average this post-NB IQ block's per-bin noise power.  feed()
        // only READS blockPtr (runs its own STFT internally) and never
        // alters it — observe-only, audio is unchanged.  noiseProfile_ is
        // touched here + in startNoiseCapture, both under channelMtx_.
        if (noiseCapturing_.load(std::memory_order_relaxed) && noiseProfile_) {
            noiseProfile_->feed(blockPtr, cfg_.inSize);
            noiseProgress_.store(noiseProfile_->progress(),
                                 std::memory_order_relaxed);
            if (!noiseProfile_->capturing()) {
                noiseProfileValid_.store(noiseProfile_->valid(),
                                         std::memory_order_relaxed);
                noiseCapturing_.store(false, std::memory_order_relaxed);
                // Auto-load a freshly-captured profile if apply is on.
                if (applyEnabled_.load(std::memory_order_relaxed) && reducer_ &&
                    noiseProfile_->valid() &&
                    reducer_->fftSize() == noiseProfile_->fftSize()) {
                    reducer_->setProfile(noiseProfile_->noisePower());
                }
            }
        }
        // Slice 3 apply: when enabled + a valid profile is loaded, clean
        // the post-NB IQ block (Wiener-from-profile) and run WDSP + the
        // analyzer on the CLEANED block so audio and panadapter match.
        // Same-count interface → just swap the pointer (one window of
        // latency lives inside the reducer).  Off/no-profile → unchanged.
        double *dspPtr = blockPtr;
        if (applyEnabled_.load(std::memory_order_relaxed) && reducer_ &&
            reducer_->ready()) {
            if (static_cast<int>(cleanBuf_.size()) < 2 * cfg_.inSize) {
                cleanBuf_.resize(static_cast<size_t>(2 * cfg_.inSize));
            }
            reducer_->process(blockPtr, cfg_.inSize, cleanBuf_.data());
            dspPtr = cleanBuf_.data();
        }
        api.fexchange0(channel_, dspPtr, outBuf_.data(), &fexErr_);

        // Step 5: feed the SAME block WDSP saw (cleaned when applying) to
        // the panadapter so the trace matches the audio.
        if (analyzerOpen_ && api.Spectrum0) {
            api.Spectrum0(1, kAnDisp, 0, 0, dspPtr);
        }

        // TCI IQ stream tap: copy this block's interleaved I,Q as float32
        // (queued signal → TCI server frames + sends).  Native inRate.
        if (tciIqOn_.load(std::memory_order_relaxed)) {
            const int n = 2 * cfg_.inSize;          // interleaved scalars
            QByteArray b(n * int(sizeof(float)), Qt::Uninitialized);
            float *d = reinterpret_cast<float *>(b.data());
            for (int i = 0; i < n; ++i) d[i] = static_cast<float>(accum_[size_t(i)]);
            emit tciIqBlock(b, cfg_.inRate);
        }

        // Peak |sample| across L+R as the audio-level proxy (Step 3d
        // is a measurement step — no playback).  20*log10(peak) dBFS.
        double peak = 0.0;
        const int outDoubles = 2 * outSize_;
        for (int i = 0; i < outDoubles; ++i) {
            const double a = std::fabs(outBuf_[static_cast<size_t>(i)]);
            if (a > peak) {
                peak = a;
            }
        }
        const double db = (peak > 0.0) ? 20.0 * std::log10(peak) : -200.0;
        audioDbFs_.store(db, std::memory_order_relaxed);

        // TCI audio stream tap: post-DSP demod audio as mono float32 (the
        // demod is mono → L channel), pre operator-volume so the client
        // gets the raw RX audio.  Native outRate (48 kHz).
        if (tciAudioOn_.load(std::memory_order_relaxed)) {
            QByteArray b(outSize_ * int(sizeof(float)), Qt::Uninitialized);
            float *d = reinterpret_cast<float *>(b.data());
            for (int f = 0; f < outSize_; ++f)
                d[f] = static_cast<float>(outBuf_[size_t(2 * f)]);
            emit tciAudioBlock(b, cfg_.outRate);
        }

        // Step 3e/5: apply the operator volume/mute gain, convert to
        // int16 stereo, and route to the active output — the HL2 codec
        // (EP2 injection) or the PC sound card.  gain = 0 when muted
        // (SAFETY default at startup) — applies to BOTH paths.
        {
            // Two mute paths OR together: the operator's manual mute_
            // (Audio panel) AND the auto-mute-on-TX gate (live wire MOX
            // bit, gated by autoMuteOnTx_ pref).  Either silences audio
            // without disturbing the other's persistent state — so a
            // keyup releases the TX gate and audio resumes at the
            // operator's pre-TX volume immediately.  Both reads are
            // memory_order_relaxed because the gain calc tolerates a
            // one-block stale read (worst case = one ~21 ms output
            // block of pre-edge audio, ear-imperceptible).
            const bool m_manual = muted_.load(std::memory_order_relaxed);
            const bool m_tx     = txMuted_.load(std::memory_order_relaxed) &&
                                  autoMuteOnTx_.load(std::memory_order_relaxed);
            double gain =
                (m_manual || m_tx)
                    ? 0.0
                    : posToGain(volume_.load(std::memory_order_relaxed));
            if (hl2Out_) {
                gain *= kHl2OutAtten;   // tame the hotter AK4951 output
            }
            // Stereo balance (−1 left … +1 right): attenuate the opposite
            // channel.  1.0 each at centre.  Applied as the very last stage.
            const double bal = balance_.load(std::memory_order_relaxed);
            const double lBal = (bal > 0.0) ? (1.0 - bal) : 1.0;
            const double rBal = (bal < 0.0) ? (1.0 + bal) : 1.0;
            for (int f = 0; f < outSize_; ++f) {
                double l = outBuf_[static_cast<size_t>(2 * f + 0)] * gain;
                double r = outBuf_[static_cast<size_t>(2 * f + 1)] * gain;
                // BIN — synthesize the stereo pair from the mono (L=R)
                // signal via the Hilbert post-processor (last stage).
                if (binEnabled_)
                    binauralStep(l, &l, &r);
                l *= lBal;
                r *= rBal;
                l = std::clamp(l, -1.0, 1.0);
                r = std::clamp(r, -1.0, 1.0);
                pcm16_[static_cast<size_t>(2 * f + 0)] =
                    static_cast<qint16>(l * 32767.0);
                pcm16_[static_cast<size_t>(2 * f + 1)] =
                    static_cast<qint16>(r * 32767.0);
            }
            if (hl2Out_) {
                // HL2 onboard codec — hand to the EP2 writer's ring.
                if (hl2AudioPush_) hl2AudioPush_(pcm16_.data(), outSize_);
            } else {
                // PC sound card — audioMtx_ guards audioRing_ against a
                // main-thread device switch / teardown.
                std::lock_guard<std::mutex> lk(audioMtx_);
                if (audioRing_) audioRing_->push(pcm16_.data(), outSize_);
            }
        }

        // Drop the consumed block; shift the small remainder down.
        accum_.erase(accum_.begin(),
                     accum_.begin() + static_cast<std::ptrdiff_t>(blockDoubles));
    }
}

} // namespace lyra::dsp
