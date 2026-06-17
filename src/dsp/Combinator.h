// Lyra — native TX Combinator (5-band multiband compressor + optional
// automatic Spectral Balance Control).  Task #51.
//
// A pre-WDSP-TXA rack stage on the mic stream, after the EQ, before the
// Plate (chain: EQ -> Combinator -> Plate -> ALC).  Lyra-native DSP
// (standard Linkwitz-Riley crossover + feed-forward compressor math —
// NOT a port; the reference device's proprietary algorithm is not
// copied, only the documented behaviour reproduced).  See
// docs/architecture/combinator_design.md for the full design note and
// the N8SDR default preset.
//
// Signal: mono mic carried as the real (I) channel of interleaved
// {I=mic, Q=0} doubles — same contract as ParamEq/SpeechProcessor.
//
//   x -> LR4 24 dB/oct split -> 5 bands [LOW LO-MID MID HI-MID HIGH]
//          each: peak env -> gain computer (thr, ratio, att, rel) -> makeup
//          [+ SBC: extra per-band gain nudging bands toward balance]
//        -> sum = WET ;  out = MIX*WET + (1-MIX)*DRY
//
// Threading: operator/UI thread calls setters + reads the meter getters;
// the audio thread calls processInterleaved().  Scalar params are
// atomics.  Crossover coefficients (change only on sample-rate / X-OVER)
// are published staged->active via a block-start try_lock (the ParamEq
// pattern) so the audio thread never blocks.  Filter + envelope STATE is
// audio-thread-owned.

#pragma once

#include <array>
#include <atomic>
#include <mutex>

namespace lyra::dsp {

class Combinator {
public:
    static constexpr int kNumBands  = 5;   // 0=LOW 1=LO-MID 2=MID 3=HI-MID 4=HIGH
    static constexpr int kNumXover  = 4;   // crossover points between the 5 bands

    Combinator();

    void setSampleRate(double fs);
    void setBypass(bool on);               // whole-stage skip (true = pass-through)

    // ── Global controls ──────────────────────────────────────────────
    void setMix(double frac);              // 0..1 wet (1 = full wet)
    void setAttackMs(double ms);
    void setReleaseMs(double ms);
    void setRatio(double ratio);           // >= 1  (global, all bands)
    void setThreshDb(double db);           // global TRIM threshold, -40..0 dBFS
    void setMakeupDb(double db);           // global TRIM makeup,    -10..+10 dB
    void setXover(double rel);             // relative shift of the 4-point set
    void setSbcEnabled(bool on);
    void setSbcSpeed(double speed);        // 0..10 (SBC aggressiveness)

    // ── Per-band controls (b: 0=LOW .. 4=HIGH) ───────────────────────
    void setBandThreshDb(int b, double db);  // per-band offset, -10..+10
    void setBandGainDb(int b, double db);    // per-band makeup, -10..+10
    void setBandEnabled(int b, bool on);     // off = band passes flat (no comp)

    // ── Audio ─────────────────────────────────────────────────────────
    void processInterleaved(double *iqPairs, int nPairs);
    void reset();                          // clear all filter + envelope state

    // ── Meters (UI thread reads; lock-free atomic snapshots) ─────────
    double bandReductionDb(int b) const;   // current gain reduction (>= 0)
    double bandSbcDb(int b) const;         // current SBC balance gain (signed)
    double bandPeakDb(int b) const;        // current band peak level, dBFS

    // Crossover frequencies currently in use (after the X-OVER shift) —
    // UI thread, for drawing the band split on the meters.
    double xoverHz(int k) const;

private:
    // Coefficient-only biquad section (no state) — staged under mutex,
    // copied to the audio thread at block start.
    struct Sec { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct St  { double z1 = 0, z2 = 0; };
    static inline double run(const Sec &c, St &s, double x) {
        const double y = c.b0 * x + s.z1;
        s.z1 = c.b1 * x - c.a1 * y + s.z2;
        s.z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    static Sec butterLP(double fs, double f0);   // 2nd-order Butterworth (Q=1/√2)
    static Sec butterHP(double fs, double f0);
    static Sec allpass2(double fs, double f0);    // 2nd-order allpass (Q=1/√2)

    // One crossover point's LR4 sections: LP4 = lp[0]∘lp[1], HP4 = hp[0]∘hp[1];
    // ap = the 2nd-order allpass (= LP4+HP4) used to phase-compensate the
    // lower bands so the recombined sum stays magnitude-flat at unity.
    struct XSec { Sec lp[2], hp[2], ap; };

    void restageXover();                  // recompute staged_ from fs_ + xover_

    double baseXoverHz_[kNumXover] = { 150.0, 500.0, 1500.0, 5000.0 };

    double fs_ = 48000.0;
    // UI-thread copies of time params (for coef recompute on fs change).
    double attMs_ = 11.0, relMs_ = 494.0, sbcSpeed_ = 4.0;
    std::atomic<bool>   bypass_{false};

    // Globals (atomic scalars).
    std::atomic<double> mix_{1.0};
    std::atomic<double> attCoef_{0.0};    // one-pole attack coefficient
    std::atomic<double> relCoef_{0.0};    // one-pole release coefficient
    std::atomic<double> ratio_{3.0};
    std::atomic<double> threshDb_{-34.0};
    std::atomic<double> makeupDb_{0.0};
    std::atomic<double> xover_{0.0};      // relative shift value
    std::atomic<bool>   sbcOn_{false};
    std::atomic<double> sbcCoef_{0.0};    // SBC level-tracking coefficient
    std::atomic<double> sbcAmount_{0.0};  // SBC nudge fraction per update

    // Per-band params.
    std::array<std::atomic<double>, kNumBands> bandThreshDb_{};
    std::array<std::atomic<double>, kNumBands> bandGainDb_{};
    std::array<std::atomic<bool>,   kNumBands> bandOn_{};

    // Crossover coeffs: staged (UI) -> active (audio).
    mutable std::mutex xMtx_;
    std::array<XSec, kNumXover> xStaged_{};
    std::atomic<bool> xDirty_{false};
    std::array<XSec, kNumXover> xActive_{};

    // Audio-thread filter state.
    std::array<St, 2> lpSt_[kNumXover];   // LP4 sections per crossover
    std::array<St, 2> hpSt_[kNumXover];   // HP4 sections per crossover
    // Phase-comp allpass state: apSt_[band][k] applies crossover-k's
    // allpass to the lower band 'band' (only k > band entries used).
    St apSt_[kNumBands][kNumXover];

    // Compressor + SBC audio-thread state.
    double env_[kNumBands]   = {0,0,0,0,0};   // peak detector env (linear)
    double sbcEnv_[kNumBands]= {0,0,0,0,0};   // slow SBC level env (linear)

    // Meter snapshots (audio writes, UI reads).
    std::array<std::atomic<double>, kNumBands> mGrDb_{};
    std::array<std::atomic<double>, kNumBands> mSbcDb_{};
    std::array<std::atomic<double>, kNumBands> mPeakDb_{};
};

}  // namespace lyra::dsp
