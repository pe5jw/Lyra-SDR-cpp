// Lyra — TX speech pre-processor rack (#88).  Two Lyra-native stages that
// run BEFORE the EQ in the mic chain:
//
//   1. Auto-AGC — an input leveller.  A peak envelope follower drives a
//      smoothed gain that pulls the mic toward a target level (quiet talk
//      up, loud talk down), capped by a max-gain limit.  Distinct from the
//      post-rack WDSP leveller: this evens the mic BEFORE the EQ/comp see it.
//   2. De-esser — a frequency-dynamic processor (nothing else in the rack
//      does this): a bandpass detector watches the 5–8 kHz sibilance band
//      and ducks only that band when "s/sh" spikes past a threshold.
//
// Order: AGC → De-esser (level first, then tame sibilance).  Both operate
// on the real (I) channel of the interleaved {I=mic, Q=0} TX double buffer,
// same contract as ParamEq::processInterleaved.
//
// Threading: operator/UI thread calls the setters; the audio thread calls
// processInterleaved().  Scalar params are atomics; the de-esser bandpass
// coeffs are published staged->active via a block-start try_lock (the
// ParamEq pattern) so the audio thread never blocks on a UI change.  Filter
// + envelope STATE is audio-thread-owned.

#pragma once

#include <array>
#include <atomic>
#include <mutex>

namespace lyra::dsp {

class SpeechProcessor {
public:
    SpeechProcessor();

    void setSampleRate(double fs);

    // ── Auto-AGC (input leveller) ────────────────────────────────────
    void setAgcEnabled(bool on);
    void setAgcTargetDb(double db);     // desired output level, dBFS (e.g. -16)
    void setAgcMaxGainDb(double db);    // ceiling on make-up gain (e.g. 18)

    // ── De-esser (frequency-dynamic) ─────────────────────────────────
    void setDeessEnabled(bool on);
    void setDeessFreqHz(double hz);     // sibilance band centre (~5–8 kHz)
    void setDeessThreshDb(double db);   // band level above which ducking starts
    void setDeessRangeDb(double db);    // max attenuation applied to the band

    // In-place: filter the I channel of nPairs {I,Q} doubles.  RT-safe.
    void processInterleaved(double *iqPairs, int nPairs);
    void reset();                       // clear envelopes + filter state

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    static Biquad designBandpass(double fs, double f0, double q);
    void restageDeess();                // recompute the detector bandpass

    double fs_ = 48000.0;

    // Auto-AGC params (atomic scalars) + audio-thread state.
    std::atomic<bool>   agcOn_{false};
    std::atomic<double> agcTargetLin_{0.158};   // -16 dBFS
    std::atomic<double> agcMaxGainLin_{7.94};    // +18 dB
    double agcEnv_  = 0.0;
    double agcGain_ = 1.0;

    // De-esser params + state.
    std::atomic<bool>   deessOn_{false};
    std::atomic<double> deessThreshLin_{0.063};  // -24 dBFS
    std::atomic<double> deessRangeLin_{0.398};    // -8 dB floor (1 - range duck)
    double deessFreq_ = 6500.0;
    double deessQ_    = 3.0;

    mutable std::mutex bpMtx_;
    Biquad bpStaged_{};
    std::atomic<bool> bpDirty_{false};
    Biquad bpActive_{};
    double bpZ1_ = 0.0, bpZ2_ = 0.0;   // detector bandpass state
    double deessEnv_ = 0.0;
};

}  // namespace lyra::dsp
