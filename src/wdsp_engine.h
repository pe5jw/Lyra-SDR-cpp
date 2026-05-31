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
#include <QVariantList>
#include <QVariantMap>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class QAudioSink;

namespace lyra::dsp {

// Defined in wdsp_engine.cpp — a QIODevice the QAudioSink pulls audio
// from, backed by a mutex-protected stereo int16 ring.
class AudioRing;

// Captured noise profile (slice 2) + reducer (slice 3).  Held by
// unique_ptr; only the .cpp needs the complete types.
class CapturedProfile;
class NoiseReducer;

// Per-channel sample rates + buffer size.  Defaults match the working
// HL2 setup: 1024-frame 192 kHz IQ in, 4096-sample
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
    // Auto-mute-on-TX (task #26): when the wire MOX bit settles true,
    // RX1 audio is force-muted so the operator doesn't self-deafen
    // off their own TX coupling.  Separate from the operator's manual
    // mute (muted_): both are OR'd into the gain calc so either path
    // silences audio without disturbing the other's state.  txMuted is
    // the live MOX-driven flag; autoMuteOnTx is the operator's master
    // switch (persisted, default ON — sane safety posture).  When
    // autoMuteOnTx is OFF, txMuted has no effect on the gain.
    Q_PROPERTY(bool   txMuted     READ txMuted     NOTIFY txMutedChanged)
    Q_PROPERTY(bool   autoMuteOnTx READ autoMuteOnTx NOTIFY autoMuteOnTxChanged)
    // AF makeup gain (WDSP RXA panel gain), 0..+40 dB — a pre-volume
    // output trim so you can set a comfortable WDSP level and ride Volume
    // on top.  Default 0 dB (unity).  Stereo BALANCE (−1 left … +1 right)
    // pans the mono demod across L/R in the output stage (Lyra-side).
    Q_PROPERTY(double afGainDb    READ afGainDb    NOTIFY afGainChanged)
    Q_PROPERTY(double balance     READ balance     NOTIFY balanceChanged)
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
    // centred filter (standard HF SDR convention).
    Q_PROPERTY(int cwPitchHz READ cwPitchHz WRITE setCwPitchHz
               NOTIFY cwPitchChanged)
    // VFO − DDS, Hz: +pitch in CWU, −pitch in CWL, 0 otherwise.  The
    // tuning layer uses it (VFO = DDS + markerOffset) and the panadapter
    // draws the carrier marker offset by it.
    Q_PROPERTY(int markerOffsetHz READ markerOffsetHz NOTIFY markerOffsetChanged)
    // ── RX DSP operator controls (ported from old Lyra's DSP+AUDIO
    // panel).  Noise reduction = WDSP EMNR.  nrMode 1..4 picks the
    // gain function (Wiener+SPP / Wiener / MMSE-LSA / trained); AEPF is
    // the anti-musical-noise post-filter; NPE 0=OSMS 1=MCRA picks the
    // noise-power estimator.  All persisted via QSettings; pushed to
    // WDSP on every change and re-applied on channel (re)open. ───────
    Q_PROPERTY(bool nrEnabled READ nrEnabled NOTIFY nrChanged)
    Q_PROPERTY(int  nrMode     READ nrMode    NOTIFY nrChanged)
    Q_PROPERTY(bool aepfEnabled READ aepfEnabled NOTIFY nrChanged)
    Q_PROPERTY(int  npeMethod  READ npeMethod  NOTIFY nrChanged)
    // AGC mode as an operator-facing string: off / fast / med / slow.
    // (Long / Auto / Custom land with the rest of the AGC surface.)
    Q_PROPERTY(QString agcMode READ agcMode NOTIFY agcModeChanged)
    // Live AGC gain action (WDSP RXA_AGC_GAIN), dB — re-read at the 5 Hz
    // levels poll.  agcThreshDb is the (currently fixed) AGC threshold the
    // readout shows alongside it, matching old Lyra's "thr / gain" cells.
    Q_PROPERTY(double agcGainDb   READ agcGainDb   NOTIFY levelsChanged)
    Q_PROPERTY(double agcThreshDb READ agcThreshDb CONSTANT)
    // ANF — auto-notch (LMS predictor that nulls carriers/heterodynes).
    Q_PROPERTY(bool anfEnabled READ anfEnabled NOTIFY anfChanged)
    // LMS — line enhancer (ANR predictor that lifts CW/tones).  strength
    // 0..1 scales taps (32..128) + adapt rate; 0.5 ≈ WDSP-class default.
    Q_PROPERTY(bool   lmsEnabled  READ lmsEnabled  NOTIFY lmsChanged)
    Q_PROPERTY(double lmsStrength READ lmsStrength NOTIFY lmsChanged)
    // Manual notches (NF) — master run + the notch list.  Each notch is
    // a QVariantMap { offsetHz, widthHz, active } where offsetHz is the
    // baseband offset from the tuned centre (= what the panadapter
    // shows).  The panadapter draws the red bands + does right-click /
    // drag / wheel placement against this list.
    Q_PROPERTY(bool notchEnabled READ notchEnabled NOTIFY notchesChanged)
    Q_PROPERTY(QVariantList notches READ notches NOTIFY notchesChanged)
    // All-mode squelch (SQ).  One operator threshold 0..1; the engine
    // routes it to SSQL / FMSQ / AMSQ per the current demod mode.
    Q_PROPERTY(bool squelchEnabled READ squelchEnabled NOTIFY squelchChanged)
    Q_PROPERTY(double squelchThreshold READ squelchThreshold NOTIFY squelchChanged)
    // Noise blanker (NB) — impulse blanker on the raw IQ.  strength 0..1
    // maps to the NOB detection threshold (higher strength = lower
    // threshold = more aggressive blanking).
    Q_PROPERTY(bool nbEnabled READ nbEnabled NOTIFY nbChanged)
    Q_PROPERTY(double nbStrength READ nbStrength NOTIFY nbChanged)
    // APF — CW audio peaking filter, centred on the CW pitch.  Engages
    // only in CWU/CWL; the button is live in any mode but does nothing
    // outside CW (gated by the engine).
    Q_PROPERTY(bool apfEnabled READ apfEnabled NOTIFY apfChanged)
    // APF peak gain (dB) the operator picks for how hard the CW peak lifts.
    Q_PROPERTY(double apfGainDb READ apfGainDb NOTIFY apfChanged)
    // BIN — binaural pseudo-stereo (headphone soundstage widening).  A
    // Lyra-native Hilbert post-processor on the mono output (not a WDSP
    // call).  depth 0..1: 0 = mono, 1 = full Hilbert pair.
    Q_PROPERTY(bool binEnabled READ binEnabled NOTIFY binChanged)
    Q_PROPERTY(double binDepth READ binDepth NOTIFY binChanged)
    // Captured noise profile (IQ-domain spectral subtraction — Lyra's
    // signature NR).  Slice 2 = capture only: noiseCapturing is true
    // while a 📷 Cap window is averaging the band's noise power;
    // noiseCaptureProgress is 0..1; noiseProfileValid flips true once a
    // capture completes.  Apply (the toggle that actually subtracts) and
    // the named-profile store land in slices 3/4.
    Q_PROPERTY(bool noiseCapturing READ noiseCapturing NOTIFY noiseCaptureChanged)
    Q_PROPERTY(double noiseCaptureProgress READ noiseCaptureProgress
               NOTIFY noiseCaptureChanged)
    Q_PROPERTY(bool noiseProfileValid READ noiseProfileValid
               NOTIFY noiseCaptureChanged)
    // Slice 3: the apply toggle.  When on AND a valid profile is loaded,
    // feedIq cleans the IQ (Wiener-from-profile) before WDSP.  Default
    // OFF — the operator opts in; adds ~one STFT window of RX latency.
    Q_PROPERTY(bool noiseApplyEnabled READ noiseApplyEnabled
               NOTIFY noiseApplyChanged)
    // Slice 4: capture FFT size (2048/4096/8192) + capture duration
    // (3/5/10 s), and the named-profile store (manual-curated; each
    // profile is rate + FFT-size tagged).  noiseProfiles = saved names;
    // noiseActiveProfile = the loaded one ("" = none / unsaved live).
    Q_PROPERTY(int noiseFftSize READ noiseFftSize NOTIFY noiseSettingsChanged)
    Q_PROPERTY(double noiseCaptureSeconds READ noiseCaptureSeconds
               NOTIFY noiseSettingsChanged)
    Q_PROPERTY(QStringList noiseProfiles READ noiseProfiles
               NOTIFY noiseProfilesChanged)
    Q_PROPERTY(QString noiseActiveProfile READ noiseActiveProfile
               NOTIFY noiseProfilesChanged)
    // Slice 4B: apply tunables (live).  strength = over-subtraction α
    // (1 = standard, higher = more aggressive); floorDb = max attenuation
    // (deeper = more noise cut, more musical-noise risk); smoothing =
    // per-bin mask smoothing 0..0.95 (higher = steadier / less twinkle).
    Q_PROPERTY(double noiseStrength READ noiseStrength NOTIFY noiseTuningChanged)
    Q_PROPERTY(double noiseFloorDb READ noiseFloorDb NOTIFY noiseTuningChanged)
    Q_PROPERTY(double noiseSmoothing READ noiseSmoothing NOTIFY noiseTuningChanged)

public:
    explicit WdspEngine(WdspNative *wdsp, QObject *parent = nullptr);
    ~WdspEngine() override;

    bool isRunning() const { return running_; }

    double audioDbFs() const {
        return audioDbFs_.load(std::memory_order_relaxed);
    }

    // In-passband RX signal strength (WDSP RXA_S_PK), in WDSP's raw
    // dBm-ish units — the standard WDSP S-meter source.
    // Returns -200 when the RX channel isn't running.  Safe to call
    // from the UI thread (just reads WDSP's latest stored meter value).
    double sMeterDbm() const;
    // Live AGC gain action in dB (WDSP RXA_AGC_GAIN); 0 when not running.
    double agcGainDb() const;
    // The configured AGC threshold the readout shows (dBFS, currently the
    // fixed first-light value; becomes operator-tunable with the AGC
    // threshold control later).
    double agcThreshDb() const;

    double volume() const { return volume_.load(std::memory_order_relaxed); }
    double volumeDb() const;   // slider position -> dB (for UI readout)
    bool   muted()  const { return muted_.load(std::memory_order_relaxed); }
    bool   txMuted() const { return txMuted_.load(std::memory_order_relaxed); }
    bool   autoMuteOnTx() const { return autoMuteOnTx_.load(std::memory_order_relaxed); }
    double afGainDb() const { return afGainDb_; }
    double balance()  const { return balance_.load(std::memory_order_relaxed); }
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
    // internally, as standard SDR apps rely on).  Plain C++ (not Q_INVOKABLE):
    // the panadapter is a C++ QQuickPaintedItem, not QML JS.
    int  spectrumPixelCount() const;
    int  copySpectrum(float *dst, int maxN);
    // Display-only notch cut: pull the columns under each
    // active manual notch down to floorDb in a display dB array of `n`
    // points spanning the CURRENT displayed span.  Called by the
    // panadapter (floor = its noise floor) + waterfall (floor = scale
    // bottom) AFTER their noise-floor / auto-scale math, so the notch
    // visibly carves the trace + waterfall without skewing those
    // estimates.  No-op when NF is off or there are no notches.
    void carveNotches(float *db, int n, double floorDb) const;

    // Step 3e operator audio controls (call from the UI / main thread).
    // setVolume: linear gain 0.0..1.0 applied before int16 conversion.
    // setMuted:  hard mute (gain 0) without losing the volume setting.
    // audioOutputDevices: the operator's PC output devices, by name.
    // setAudioOutputDevice: switch output device live (restarts sink).
    Q_INVOKABLE void setVolume(double v);
    Q_INVOKABLE void setMuted(bool m);
    // Auto-mute-on-TX driver: setTxMuted is wired to HL2Stream's
    // moxActiveChanged(bool) signal in main.cpp — fires true at the end
    // of the keydown TR-delay (wire MOX bit settled) and false at the
    // end of the keyup ptt_out_delay (RF gone).  No persistence — pure
    // transient state.  setAutoMuteOnTx is the operator's master switch
    // (persisted; default ON).
    Q_INVOKABLE void setTxMuted(bool m);
    Q_INVOKABLE void setAutoMuteOnTx(bool on);
    Q_INVOKABLE void setAfGainDb(double db);   // 0..+40 dB makeup (WDSP panel gain)
    Q_INVOKABLE void setBalance(double b);     // -1 (L) .. +1 (R)

    // RX DSP operator controls.  Getters are cheap reads of the
    // persisted state; setters store, persist (QSettings), push to WDSP
    // when the channel is open, and emit the NOTIFY signal.
    bool nrEnabled()   const { return nrEnabled_; }
    int  nrMode()      const { return nrMode_; }
    bool aepfEnabled() const { return aepfEnabled_; }
    int  npeMethod()   const { return npeMethod_; }
    QString agcMode()  const { return agcMode_; }
    Q_INVOKABLE void setNrEnabled(bool on);
    Q_INVOKABLE void setNrMode(int mode);        // 1..4
    Q_INVOKABLE void setAepfEnabled(bool on);
    Q_INVOKABLE void setNpeMethod(int method);   // 0=OSMS 1=MCRA
    Q_INVOKABLE void setAgcMode(const QString &mode);  // off/fast/med/slow
    bool anfEnabled()    const { return anfEnabled_; }
    bool lmsEnabled()    const { return lmsEnabled_; }
    double lmsStrength() const { return lmsStrength_; }
    Q_INVOKABLE void setAnfEnabled(bool on);
    Q_INVOKABLE void setLmsEnabled(bool on);
    Q_INVOKABLE void setLmsStrength(double s);   // 0..1
    // ── Manual notches ──
    bool notchEnabled() const { return notchEnabled_; }
    QVariantList notches() const;                 // {offsetHz,widthHz,active}…
    Q_INVOKABLE void setNotchEnabled(bool on);
    // Add a notch at `offsetHz` from centre, default 200 Hz wide.
    // Returns the new notch index.  Auto-enables NF.
    Q_INVOKABLE int  addNotch(double offsetHz, double widthHz = 200.0);
    Q_INVOKABLE void removeNotch(int index);
    Q_INVOKABLE void moveNotch(int index, double offsetHz);
    Q_INVOKABLE void setNotchWidth(int index, double widthHz);
    Q_INVOKABLE int  notchCount() const { return static_cast<int>(notches_.size()); }
    // Nearest notch to `offsetHz` within `tolHz`, else -1 (panadapter
    // hit-testing for drag / wheel / right-click-remove).
    Q_INVOKABLE int  notchNear(double offsetHz, double tolHz) const;
    // ── All-mode squelch ──
    bool   squelchEnabled()   const { return squelchEnabled_; }
    double squelchThreshold() const { return squelchThreshold_; }
    Q_INVOKABLE void setSquelchEnabled(bool on);
    Q_INVOKABLE void setSquelchThreshold(double t);   // 0..1
    bool   nbEnabled()  const { return nbEnabled_; }
    double nbStrength() const { return nbStrength_; }
    Q_INVOKABLE void setNbEnabled(bool on);
    Q_INVOKABLE void setNbStrength(double s);          // 0..1
    bool apfEnabled() const { return apfEnabled_; }
    double apfGainDb() const { return apfGainDb_; }
    Q_INVOKABLE void setApfEnabled(bool on);
    Q_INVOKABLE void setApfGainDb(double db);   // peak gain, dB
    bool binEnabled() const { return binEnabled_; }
    double binDepth() const { return binDepth_; }
    Q_INVOKABLE void setBinEnabled(bool on);
    Q_INVOKABLE void setBinDepth(double d);     // 0..1
    // ── Captured noise profile (slice 2: capture) ──
    bool   noiseCapturing() const {
        return noiseCapturing_.load(std::memory_order_relaxed);
    }
    double noiseCaptureProgress() const {
        return noiseProgress_.load(std::memory_order_relaxed);
    }
    bool   noiseProfileValid() const {
        return noiseProfileValid_.load(std::memory_order_relaxed);
    }
    // Arm a noise capture over `seconds` (3/5/10) at the current IQ rate.
    // Observe-only: averages the band's per-bin noise power; does NOT
    // alter audio.  Safe to call while running (serialised via channelMtx_).
    Q_INVOKABLE void startNoiseCapture(double seconds);
    Q_INVOKABLE void cancelNoiseCapture();
    // Slice 3: turn captured-profile noise reduction on/off.  Loads the
    // current captured profile into the reducer on enable; a fresh
    // capture completed while enabled is auto-loaded.  No-op-on-audio
    // until a valid profile exists.
    bool noiseApplyEnabled() const {
        return applyEnabled_.load(std::memory_order_relaxed);
    }
    Q_INVOKABLE void setNoiseApply(bool on);
    // ── Captured-profile settings + named-profile store (slice 4) ──
    int    noiseFftSize()        const { return npFftSize_; }
    double noiseCaptureSeconds() const { return npCaptureSeconds_; }
    QStringList noiseProfiles()  const;
    QString noiseActiveProfile() const { return npActiveName_; }
    // FFT size 2048/4096/8192 — changing it cancels any capture, drops the
    // reducer + apply (a profile is size-specific), and persists.
    Q_INVOKABLE void setNoiseFftSize(int n);
    Q_INVOKABLE void setNoiseCaptureSeconds(double s);  // 3/5/10
    // Capture using the configured duration (convenience for the UI).
    Q_INVOKABLE void startNoiseCaptureDefault() { startNoiseCapture(npCaptureSeconds_); }
    // Save the current (just-captured) profile under `name`; false if no
    // valid capture exists.  Overwrites a same-named profile.
    Q_INVOKABLE bool saveNoiseProfile(const QString &name);
    // Load a saved profile into the reducer.  Adopts the profile's FFT
    // size; refuses (returns false) if its capture RATE differs from the
    // current IQ rate (caller shows a recapture hint).
    Q_INVOKABLE bool loadNoiseProfile(const QString &name);
    Q_INVOKABLE void deleteNoiseProfile(const QString &name);
    // Rename a saved profile (and its file).  False if not found or the
    // new name is empty / already in use.
    Q_INVOKABLE bool renameNoiseProfile(const QString &oldName,
                                        const QString &newName);
    // UI helpers that use NATIVE top-level dialogs (QInputDialog /
    // QMessageBox) — QML Popups/Dialogs are clipped to the panel's
    // QQuickWidget rect, so the name prompt + warnings must be native.
    Q_INVOKABLE bool promptSaveProfile();                 // ask name → save
    Q_INVOKABLE void loadProfileOrWarn(const QString &name);  // load, warn on rate mismatch
    // Human-readable tag for a saved profile ("192k · 4096 · 2026-05-25").
    Q_INVOKABLE QString noiseProfileInfo(const QString &name) const;
    // Folder where profiles are stored (one .lnp file each), so the UI
    // can tell the operator where to find / back them up.
    Q_INVOKABLE QString noiseProfilesDir() const;
    // Apply tunables (slice 4B) — persisted; pushed to the reducer live
    // and re-applied whenever the reducer is (re)created.
    double noiseStrength()  const { return npAlpha_; }
    double noiseFloorDb()   const { return npFloorDb_; }
    double noiseSmoothing() const { return npSmoothing_; }
    Q_INVOKABLE void setNoiseStrength(double a);    // 1.0 .. 3.0
    Q_INVOKABLE void setNoiseFloorDb(double db);    // -24 .. -3
    Q_INVOKABLE void setNoiseSmoothing(double s);   // 0 .. 0.95
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
    void txMutedChanged();
    void autoMuteOnTxChanged();
    void afGainChanged();
    void balanceChanged();
    void audioDeviceChanged();
    void zoomChanged();
    void spanChanged();   // displayed span changed (rate OR zoom)
    void modeChanged();
    void bandwidthChanged();
    void passbandChanged();
    void cwPitchChanged();
    void markerOffsetChanged();
    void nrChanged();        // NR enable / mode / AEPF / NPE
    void agcModeChanged();
    void anfChanged();
    void lmsChanged();       // LMS enable / strength
    void notchesChanged();   // NF run / list add / remove / edit
    void squelchChanged();   // SQ enable / threshold
    void nbChanged();        // NB enable / strength
    void apfChanged();       // APF enable
    void binChanged();       // BIN enable / depth
    void noiseCaptureChanged();   // captured-profile capture state/progress
    void noiseApplyChanged();     // captured-profile apply toggle
    void noiseSettingsChanged();  // FFT size / capture duration
    void noiseProfilesChanged();  // saved-profile list / active profile
    void noiseTuningChanged();    // strength / floor / smoothing
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
    // Push the current NR (EMNR) state to WDSP — run + gain method +
    // NPE method + AEPF + position.  No-op when the channel is closed
    // (re-applied on the next openRx1).  Channel-parameterized so RX2
    // can reuse it unchanged.
    void pushNrState();
    // Push the current AGC mode (SetRXAAGCMode).  No-op when closed.
    void pushAgcMode();
    // Push ANF (auto-notch) + LMS (line enhancer) run/vals.  No-op when
    // closed; channel-parameterized for RX2 reuse.
    void pushAnfState();
    void pushLmsState();
    // Rebuild the WDSP NBP notch database from notches_ + set the run
    // flag.  No-op when closed; re-applied on openRx1 (the DB is recreated
    // empty on every channel open).  Channel-parameterized for RX2.
    void pushNotches();
    void persistNotches();   // serialize notches_ to QSettings
    // Route the squelch to SSQL / FMSQ / AMSQ per the current mode_ and
    // push run + threshold; disables the inactive modules.  No-op closed.
    void pushSquelchState();
    // Push NB threshold (from strength) + run.  No-op until the EXT
    // blanker is created in openRx1.
    void pushNbState();
    // Push the APF (CW peaking biquad) shape + run; engages only in CW.
    void pushApfState();
    // BIN: Lyra-native Hilbert pseudo-stereo.  buildBinaural() makes the
    // FIR (once); binauralStep() turns one mono sample into an L/R pair
    // with persistent FIR + delay state; resetBinaural() clears it.
    void buildBinaural();
    void binauralStep(double mono, double *l, double *r);
    void resetBinaural();
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
    // Auto-mute-on-TX (task #26).  txMuted_ tracks the live wire MOX bit
    // (false at boot; toggled by HL2Stream::moxActiveChanged).  Not
    // persisted — pure transient.  autoMuteOnTx_ is the operator's
    // master switch (persisted under audio/autoMuteOnTx, default ON =
    // safe posture: a brand-new install can't self-deafen on first key).
    // Gain calc OR's (muted_) with (autoMuteOnTx_ AND txMuted_).
    std::atomic<bool>   txMuted_{false};
    std::atomic<bool>   autoMuteOnTx_{true};
    // AF makeup gain (dB, main-thread; pushed to WDSP SetRXAPanelGain1 as
    // a linear gain).  Balance −1..+1 is applied Lyra-side in feedIq, so
    // it's an atomic the RX worker reads.
    double              afGainDb_ = 0.0;
    std::atomic<double> balance_{0.0};
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
    // RX DSP operator state (main-thread only; restored from QSettings
    // in the ctor, pushed to WDSP on open + on each setter).  NR off by
    // default; Mode 3 (MMSE-LSA, WDSP default) + AEPF on + NPE OSMS
    // match old Lyra's first-light defaults.  AGC med.
    bool    nrEnabled_   = false;
    int     nrMode_      = 3;            // 1..4 (UI) -> gain_method 0..3
    bool    aepfEnabled_ = true;
    int     npeMethod_   = 0;            // 0=OSMS 1=MCRA
    QString agcMode_     = QStringLiteral("med");
    bool    anfEnabled_  = false;
    bool    lmsEnabled_  = false;
    double  lmsStrength_ = 0.5;          // 0..1 (0.5 ≈ WDSP-class default)
    // Manual notch list (offsets in Hz from the tuned centre).  Lyra-side
    // is the source of truth; pushNotches() rebuilds the WDSP DB from it.
    struct Notch { double offsetHz; double widthHz; bool active; };
    std::vector<Notch> notches_;
    bool    notchEnabled_  = false;      // NF master run
    int     wdspNotchCount_ = 0;         // notches currently in the WDSP DB
    bool    squelchEnabled_   = false;
    double  squelchThreshold_ = 0.20;    // 0..1 operator knob
    bool    nbEnabled_   = false;
    double  nbStrength_  = 0.5;          // 0..1 (higher = more blanking)
    bool    nbCreated_   = false;        // EXT NOB created for this channel
    std::vector<double> nbBuf_;          // xnobEXT output (2*inSize doubles)
    bool    apfEnabled_  = false;        // CW peaking filter (CW-gated)
    double  apfGainDb_   = 12.0;         // operator-set peak gain (dB)
    // Captured noise profile (slice 2: capture).  Created lazily at the
    // operator's captured-profile FFT size (slice 4 makes npFftSize_
    // settable; default 4096).  noiseProfile_ is created/armed on the
    // main thread and fed on the RX worker — both under channelMtx_, so
    // a capture can't be armed mid-fexchange0.  The scalar
    // capturing/progress/valid mirrors are atomics the UI polls.
    std::unique_ptr<CapturedProfile> noiseProfile_;
    int                 npFftSize_ = 4096;          // 2048/4096/8192
    std::atomic<bool>   noiseCapturing_{false};
    std::atomic<double> noiseProgress_{0.0};
    std::atomic<bool>   noiseProfileValid_{false};
    bool                npLastCapturing_ = false;   // main-thread edge latch
    // Slice 3: apply.  reducer_ created on enable (and recreated on FFT-
    // size change); applyEnabled_ gates the feedIq route; cleanBuf_ holds
    // one cleaned block.  All touched under channelMtx_ except the atomic.
    std::unique_ptr<NoiseReducer> reducer_;
    std::atomic<bool>   applyEnabled_{false};
    std::vector<double> cleanBuf_;                  // 2*inSize cleaned IQ
    double              npCaptureSeconds_ = 5.0;    // 3/5/10
    double              npAlpha_     = 1.0;          // over-subtraction
    double              npFloorDb_   = -12.0;        // max attenuation
    double              npSmoothing_ = 0.6;          // mask smoothing
    QString             npActiveName_;              // loaded profile ("" = none)
    void applyReducerParams();   // push α/floor/smoothing to reducer_ (lock held)
    // Named-profile store (manual-curated).  One ".lnp" file per profile
    // in a findable folder (see noiseProfilesDir) — browsable / backup-
    // able / shareable.  Each is rate + FFT-size tagged.  Main-thread only.
    struct StoredProfile {
        QString             name;
        int                 rate = 0;
        int                 fft  = 0;
        QString             date;
        QString             file;    // absolute path of its .lnp
        std::vector<double> power;   // per-bin noise power (length fft)
    };
    std::vector<StoredProfile> profiles_;
    QString profilesDir() const;            // ensures the folder exists
    void    loadProfiles();                 // ctor: scan *.lnp
    bool    writeProfileFile(StoredProfile &p);   // write one (sets p.file)
    void    removeProfileFile(const StoredProfile &p);  // delete its file
    // BIN — 63-tap Hilbert FIR pseudo-stereo (mono in → L/R out).
    bool    binEnabled_  = false;
    double  binDepth_    = 0.7;          // 0 = mono, 1 = full Hilbert pair
    std::vector<double> binH_;           // Hilbert FIR taps (63)
    std::vector<double> binHist_;        // ring history (63) — FIR + delay tap
    int     binPos_      = 0;            // newest-sample index in binHist_
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
