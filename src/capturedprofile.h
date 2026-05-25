// Lyra — captured noise profile (captured-noise-profile slice 2: capture).
//
// Owns an IqStft and, during a capture window, AVERAGES the per-bin noise
// POWER (|X[k]|^2) of the operator's band over a few seconds of (ideally
// signal-free) IQ.  The result is a rate + FFT-size-tagged noise-power
// spectrum that slice 3 turns into a Wiener-from-profile gain.
//
// Capture is OBSERVE-ONLY: it runs the same STFT frames the apply path
// will use (so the profile matches the analysis exactly) but does not
// modify the IQ — feeding a CapturedProfile changes no audio.  The
// transform output is discarded here; only the spectra are measured.
//
// Qt-free + single-threaded (the WdspEngine serialises feed() against the
// capture lifecycle), so it unit-tests standalone like IqStft.  Profiles
// are power spectra (Wiener works in the power domain); persistence /
// naming / QSettings live in slice 4 — this class just produces the
// numbers and exposes them.

#pragma once

#include "iqstft.h"

#include <vector>

namespace lyra::dsp {

class CapturedProfile {
public:
    explicit CapturedProfile(int fftSize);

    // Non-copyable / non-movable: the capture hook installed on the owned
    // IqStft captures `this`, so the address must stay stable.
    CapturedProfile(const CapturedProfile &) = delete;
    CapturedProfile &operator=(const CapturedProfile &) = delete;

    int fftSize()    const { return fftSize_; }
    int sampleRate() const { return sampleRate_; }

    // Arm a capture: average over `seconds` of IQ at `sampleRate`.  Clears
    // any prior accumulation and marks the profile invalid until the
    // window completes.  Call feed() with the live IQ until capturing()
    // goes false.
    void begin(int sampleRate, double seconds);
    // Abort an in-progress capture (leaves any previous valid profile).
    void cancel();

    // Stream interleaved IQ (I,Q,…) while capturing.  No-op once the
    // window is complete (or if never armed).  Observe-only — never
    // alters the samples.
    void feed(const double *iqInterleaved, int nframes);

    bool   capturing() const { return capturing_; }
    // 0..1 fraction of the target window measured so far.
    double progress()  const;
    // True once a capture has completed and noisePower() is meaningful.
    bool   valid()     const { return valid_; }
    // Averaged per-bin noise power (length fftSize, standard FFT bin
    // order).  Empty/zero until valid().
    const std::vector<double> &noisePower() const { return profile_; }

private:
    IqStft              stft_;
    int                 fftSize_    = 0;
    int                 sampleRate_ = 0;
    bool                capturing_  = false;
    bool                valid_      = false;
    long long           frames_     = 0;   // frames measured this window
    long long           target_     = 0;   // frames to measure
    std::vector<double> powSum_;            // running per-bin power sum
    std::vector<double> profile_;           // finalized per-bin mean power
    std::vector<double> scratch_;           // discarded STFT output
};

} // namespace lyra::dsp
