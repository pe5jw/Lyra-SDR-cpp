// Lyra — TX speech pre-processor implementation.  See SpeechProcessor.h.

#include "dsp/SpeechProcessor.h"

#include <algorithm>
#include <cmath>

namespace lyra::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
// One-pole smoothing coefficient for a time constant tau (seconds).
inline double onePole(double tauSec, double fs) {
    return std::exp(-1.0 / (tauSec * fs));
}
}  // namespace

SpeechProcessor::SpeechProcessor() { setSampleRate(48000.0); }

void SpeechProcessor::setSampleRate(double fs) {
    if (fs > 0.0) { fs_ = fs; restageDeess(); }
}

SpeechProcessor::Biquad SpeechProcessor::designBandpass(double fs, double f0,
                                                        double q) {
    Biquad c;
    if (fs <= 0.0 || q <= 0.0) return c;
    const double nyq = 0.5 * fs;
    if (f0 < 10.0) f0 = 10.0;
    if (f0 > 0.99 * nyq) f0 = 0.99 * nyq;
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    // RBJ band-pass, constant 0 dB peak gain.
    const double a0 = 1.0 + alpha;
    c.b0 = alpha / a0;  c.b1 = 0.0;  c.b2 = -alpha / a0;
    c.a1 = (-2.0 * cw) / a0;  c.a2 = (1.0 - alpha) / a0;
    return c;
}

void SpeechProcessor::restageDeess() {
    const Biquad c = designBandpass(fs_, deessFreq_, deessQ_);
    { std::lock_guard<std::mutex> lk(bpMtx_); bpStaged_ = c; }
    bpDirty_.store(true, std::memory_order_release);
}

void SpeechProcessor::setGateEnabled(bool on) {
    gateOn_.store(on, std::memory_order_relaxed);
}
void SpeechProcessor::setGateThreshDb(double db) {
    gateOpenLin_.store(dbToLin(std::clamp(db, -80.0, 0.0)),
                       std::memory_order_relaxed);
}
void SpeechProcessor::setGateRangeDb(double db) {
    gateFloorLin_.store(dbToLin(-std::clamp(db, 0.0, 80.0)),
                        std::memory_order_relaxed);
}
void SpeechProcessor::setGateHoldMs(double ms) {
    gateHoldMs_.store(std::clamp(ms, 0.0, 1000.0), std::memory_order_relaxed);
}

void SpeechProcessor::setAgcEnabled(bool on) {
    agcOn_.store(on, std::memory_order_relaxed);
}
void SpeechProcessor::setAgcTargetDb(double db) {
    agcTargetLin_.store(dbToLin(std::clamp(db, -40.0, 0.0)),
                        std::memory_order_relaxed);
}
void SpeechProcessor::setAgcMaxGainDb(double db) {
    agcMaxGainLin_.store(dbToLin(std::clamp(db, 0.0, 40.0)),
                         std::memory_order_relaxed);
}

void SpeechProcessor::setDeessEnabled(bool on) {
    deessOn_.store(on, std::memory_order_relaxed);
}
void SpeechProcessor::setDeessFreqHz(double hz) {
    deessFreq_ = std::clamp(hz, 2000.0, 12000.0);
    restageDeess();
}
void SpeechProcessor::setDeessThreshDb(double db) {
    deessThreshLin_.store(dbToLin(std::clamp(db, -60.0, 0.0)),
                          std::memory_order_relaxed);
}
void SpeechProcessor::setDeessRangeDb(double db) {
    // range = max band attenuation (dB).  Stored as the floor multiplier
    // (1-duck), so the per-sample reduction is a clamp: gr >= floorMul.
    db = std::clamp(db, 0.0, 24.0);
    deessRangeLin_.store(dbToLin(-db), std::memory_order_relaxed);
}

void SpeechProcessor::reset() {
    gateKeyEnv_ = 0.0; gateGain_ = 0.0; gateState_ = false; gateHoldCtr_ = 0;
    agcEnv_ = 0.0; agcGain_ = 1.0;
    bpZ1_ = bpZ2_ = 0.0; deessEnv_ = 0.0;
}

void SpeechProcessor::processInterleaved(double *x, int n) {
    if (bpDirty_.load(std::memory_order_acquire)) {
        if (bpMtx_.try_lock()) {
            bpActive_ = bpStaged_;
            bpDirty_.store(false, std::memory_order_release);
            bpMtx_.unlock();
        }
    }
    const bool gate = gateOn_.load(std::memory_order_relaxed);
    const bool agc  = agcOn_.load(std::memory_order_relaxed);
    const bool de   = deessOn_.load(std::memory_order_relaxed);
    if (!gate && !agc && !de) return;

    const double gOpen  = gateOpenLin_.load(std::memory_order_relaxed);
    const double gClose = gOpen * 0.5;   // -6 dB hysteresis to stop chatter
    const double gFloor = gateFloorLin_.load(std::memory_order_relaxed);
    const int    gHold  = static_cast<int>(
        gateHoldMs_.load(std::memory_order_relaxed) * fs_ / 1000.0);

    const double tgt      = agcTargetLin_.load(std::memory_order_relaxed);
    const double maxG     = agcMaxGainLin_.load(std::memory_order_relaxed);
    const double thr      = deessThreshLin_.load(std::memory_order_relaxed);
    const double floorMul = deessRangeLin_.load(std::memory_order_relaxed);

    const double gKeyAtk = onePole(0.001, fs_), gKeyRel = onePole(0.010, fs_);
    const double gAtk = onePole(0.002, fs_),  gRel = onePole(0.10, fs_);
    const double agcAtk = onePole(0.002, fs_), agcRel = onePole(0.15, fs_);
    const double gSmooth = onePole(0.01, fs_);
    const double dAtk = onePole(0.0005, fs_), dRel = onePole(0.02, fs_);
    const Biquad bp = bpActive_;

    for (int s = 0; s < n; ++s) {
        double v = x[2 * s];
        bool gateOpenNow = true;

        // ── Gate (first) — mute when the input sits below the threshold.
        if (gate) {
            const double a = std::fabs(v);
            const double c = (a > gateKeyEnv_) ? gKeyAtk : gKeyRel;
            gateKeyEnv_ = c * gateKeyEnv_ + (1.0 - c) * a;
            const bool want = gateState_ ? (gateKeyEnv_ > gClose)
                                         : (gateKeyEnv_ > gOpen);
            if (want) { gateState_ = true; gateHoldCtr_ = gHold; }
            else if (gateHoldCtr_ > 0) { --gateHoldCtr_; }
            else gateState_ = false;
            const double tgtG = (gateState_ || gateHoldCtr_ > 0) ? 1.0 : gFloor;
            const double gc = (tgtG > gateGain_) ? gAtk : gRel;
            gateGain_ = gc * gateGain_ + (1.0 - gc) * tgtG;
            v *= gateGain_;
            gateOpenNow = gateGain_ > 0.5;
        }

        // ── Auto-AGC — freeze the gain while the gate is closed so pauses
        //    don't breathe.
        if (agc) {
            if (gateOpenNow) {
                const double a = std::fabs(v);
                const double c = (a > agcEnv_) ? agcAtk : agcRel;
                agcEnv_ = c * agcEnv_ + (1.0 - c) * a;
                double desired = (agcEnv_ > 1e-6) ? tgt / agcEnv_ : maxG;
                desired = std::clamp(desired, 0.1, maxG);
                agcGain_ = gSmooth * agcGain_ + (1.0 - gSmooth) * desired;
            }
            v *= agcGain_;
        }

        if (de) {
            const double hb = bp.b0 * v + bpZ1_;        // sibilance band
            bpZ1_ = bp.b1 * v - bp.a1 * hb + bpZ2_;
            bpZ2_ = bp.b2 * v - bp.a2 * hb;
            const double a = std::fabs(hb);
            const double c = (a > deessEnv_) ? dAtk : dRel;
            deessEnv_ = c * deessEnv_ + (1.0 - c) * a;
            double gr = 1.0;
            if (deessEnv_ > thr && deessEnv_ > 1e-9) {
                gr = thr / deessEnv_;                   // duck toward threshold
                if (gr < floorMul) gr = floorMul;       // limited by range
            }
            v -= hb * (1.0 - gr);                       // remove excess sibilance
        }

        x[2 * s] = v;
    }
}

}  // namespace lyra::dsp
