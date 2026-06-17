// Lyra — Combinator implementation.  See Combinator.h +
// docs/architecture/combinator_design.md.

#include "dsp/Combinator.h"

#include <algorithm>
#include <cmath>

namespace lyra::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kQ  = 0.70710678118654752440;   // 1/sqrt(2) — Butterworth / LR4

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
// One-pole smoothing coefficient for a time constant of `ms` at `fs`.
inline double tcCoef(double ms, double fs) {
    if (ms <= 0.0) return 1.0;
    return 1.0 - std::exp(-1.0 / (ms * 0.001 * fs));
}
// X-OVER knob (relative) -> multiplicative frequency shift of the base set.
// rel = -10 (N8SDR) -> 2^(-0.25) ≈ 0.84 (shift the split down ~16%).
// Curve is Lyra-native + bench-tunable (design note R5).
inline double xoverFactor(double rel) { return std::pow(2.0, rel / 40.0); }
}  // namespace

Combinator::Sec Combinator::butterLP(double fs, double f0) {
    Sec c;
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * kQ), a0 = 1.0 + alpha;
    c.b0 = ((1.0 - cw) / 2.0) / a0;
    c.b1 = (1.0 - cw) / a0;
    c.b2 = c.b0;
    c.a1 = (-2.0 * cw) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}
Combinator::Sec Combinator::butterHP(double fs, double f0) {
    Sec c;
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * kQ), a0 = 1.0 + alpha;
    c.b0 = ((1.0 + cw) / 2.0) / a0;
    c.b1 = (-(1.0 + cw)) / a0;
    c.b2 = c.b0;
    c.a1 = (-2.0 * cw) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}
Combinator::Sec Combinator::allpass2(double fs, double f0) {
    Sec c;
    const double w0 = 2.0 * kPi * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * kQ), a0 = 1.0 + alpha;
    c.b0 = (1.0 - alpha) / a0;
    c.b1 = (-2.0 * cw) / a0;
    c.b2 = (1.0 + alpha) / a0;
    c.a1 = (-2.0 * cw) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

Combinator::Combinator() {
    // Seed the N8SDR default preset (docs/architecture/combinator_design.md §4).
    setAttackMs(11.0);
    setReleaseMs(494.0);
    ratio_.store(3.0, std::memory_order_relaxed);
    threshDb_.store(-34.0, std::memory_order_relaxed);
    makeupDb_.store(0.0, std::memory_order_relaxed);
    mix_.store(1.0, std::memory_order_relaxed);
    xover_.store(-10.0, std::memory_order_relaxed);
    sbcOn_.store(true, std::memory_order_relaxed);
    sbcAmount_.store(0.5, std::memory_order_relaxed);   // deviation half-correct (R3)
    setSbcSpeed(4.0);

    // Per-band THRESH offset / makeup GAIN, LOW -> HIGH.
    const double bt[kNumBands] = { -3.5, -4.0, -1.0, -1.5, -2.0 };
    const double bg[kNumBands] = {  1.5,  1.0,  0.0,  2.5,  3.0 };
    for (int b = 0; b < kNumBands; ++b) {
        bandThreshDb_[b].store(bt[b], std::memory_order_relaxed);
        bandGainDb_[b].store(bg[b], std::memory_order_relaxed);
        bandOn_[b].store(true, std::memory_order_relaxed);
        mGrDb_[b].store(0.0, std::memory_order_relaxed);
        mSbcDb_[b].store(0.0, std::memory_order_relaxed);
        mPeakDb_[b].store(-120.0, std::memory_order_relaxed);
    }
    restageXover();
    // First active set immediately (no audio running yet).
    xActive_ = xStaged_;
    xDirty_.store(false, std::memory_order_relaxed);
}

void Combinator::setSampleRate(double fs) {
    if (fs <= 0.0 || fs == fs_) return;
    fs_ = fs;
    setAttackMs(attMs_);
    setReleaseMs(relMs_);
    setSbcSpeed(sbcSpeed_);
    restageXover();
}

void Combinator::setBypass(bool on) { bypass_.store(on, std::memory_order_relaxed); }

void Combinator::setMix(double frac) {
    mix_.store(clampd(frac, 0.0, 1.0), std::memory_order_relaxed);
}
void Combinator::setAttackMs(double ms) {
    attMs_ = clampd(ms, 0.0, 500.0);
    attCoef_.store(tcCoef(attMs_, fs_), std::memory_order_relaxed);
}
void Combinator::setReleaseMs(double ms) {
    relMs_ = clampd(ms, 1.0, 5000.0);
    relCoef_.store(tcCoef(relMs_, fs_), std::memory_order_relaxed);
}
void Combinator::setRatio(double ratio) {
    ratio_.store(clampd(ratio, 1.0, 50.0), std::memory_order_relaxed);
}
void Combinator::setThreshDb(double db) {
    threshDb_.store(clampd(db, -60.0, 0.0), std::memory_order_relaxed);
}
void Combinator::setMakeupDb(double db) {
    makeupDb_.store(clampd(db, -20.0, 20.0), std::memory_order_relaxed);
}
void Combinator::setXover(double rel) {
    xover_.store(rel, std::memory_order_relaxed);
    restageXover();
}
void Combinator::setSbcEnabled(bool on) { sbcOn_.store(on, std::memory_order_relaxed); }
void Combinator::setSbcSpeed(double speed) {
    sbcSpeed_ = clampd(speed, 0.0, 10.0);
    // speed 0 -> ~2 s tracking (gentle); speed 10 -> ~50 ms (aggressive).
    const double sec = 2.0 * std::pow(0.025, sbcSpeed_ / 10.0);
    sbcCoef_.store(tcCoef(sec * 1000.0, fs_), std::memory_order_relaxed);
}

void Combinator::setBandThreshDb(int b, double db) {
    if (b < 0 || b >= kNumBands) return;
    bandThreshDb_[b].store(clampd(db, -20.0, 20.0), std::memory_order_relaxed);
}
void Combinator::setBandGainDb(int b, double db) {
    if (b < 0 || b >= kNumBands) return;
    bandGainDb_[b].store(clampd(db, -20.0, 20.0), std::memory_order_relaxed);
}
void Combinator::setBandEnabled(int b, bool on) {
    if (b < 0 || b >= kNumBands) return;
    bandOn_[b].store(on, std::memory_order_relaxed);
}

void Combinator::restageXover() {
    const double fac = xoverFactor(xover_.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> lk(xMtx_);
    for (int k = 0; k < kNumXover; ++k) {
        double f = clampd(baseXoverHz_[k] * fac, 20.0, fs_ * 0.45);
        xStaged_[k].lp[0] = xStaged_[k].lp[1] = butterLP(fs_, f);
        xStaged_[k].hp[0] = xStaged_[k].hp[1] = butterHP(fs_, f);
        xStaged_[k].ap    = allpass2(fs_, f);
    }
    xDirty_.store(true, std::memory_order_release);
}

double Combinator::xoverHz(int k) const {
    if (k < 0 || k >= kNumXover) return 0.0;
    const double fac = xoverFactor(xover_.load(std::memory_order_relaxed));
    return clampd(baseXoverHz_[k] * fac, 20.0, fs_ * 0.45);
}

void Combinator::processInterleaved(double *iq, int nPairs) {
    if (bypass_.load(std::memory_order_relaxed) || nPairs <= 0) return;

    // Pick up a fresh crossover coefficient set (rare: fs / X-OVER change).
    if (xDirty_.load(std::memory_order_acquire)) {
        if (xMtx_.try_lock()) {
            xActive_ = xStaged_;
            xDirty_.store(false, std::memory_order_relaxed);
            xMtx_.unlock();
        }
    }

    const double mix    = mix_.load(std::memory_order_relaxed);
    const double att    = attCoef_.load(std::memory_order_relaxed);
    const double rel    = relCoef_.load(std::memory_order_relaxed);
    const double ratio  = ratio_.load(std::memory_order_relaxed);
    const double gThr   = threshDb_.load(std::memory_order_relaxed);
    const double gMk    = makeupDb_.load(std::memory_order_relaxed);
    const bool   sbcOn  = sbcOn_.load(std::memory_order_relaxed);
    const double sbcC   = sbcCoef_.load(std::memory_order_relaxed);
    const double sbcAmt = sbcAmount_.load(std::memory_order_relaxed);
    const double slope  = 1.0 - 1.0 / ratio;

    double bThr[kNumBands], bMk[kNumBands];
    bool   bOn[kNumBands];
    for (int b = 0; b < kNumBands; ++b) {
        bThr[b] = bandThreshDb_[b].load(std::memory_order_relaxed);
        bMk[b]  = bandGainDb_[b].load(std::memory_order_relaxed);
        bOn[b]  = bandOn_[b].load(std::memory_order_relaxed);
    }

    double peak[kNumBands]  = {0,0,0,0,0};
    double grDb[kNumBands]  = {0,0,0,0,0};
    double sbcDb[kNumBands] = {0,0,0,0,0};

    for (int n = 0; n < nPairs; ++n) {
        const double x = iq[2 * n];
        const double dry = x;

        // ── LR4 serial split into 5 bands ────────────────────────────
        const double low1  = run(xActive_[0].lp[1], lpSt_[0][1],
                              run(xActive_[0].lp[0], lpSt_[0][0], x));
        const double high1 = run(xActive_[0].hp[1], hpSt_[0][1],
                              run(xActive_[0].hp[0], hpSt_[0][0], x));
        const double low2  = run(xActive_[1].lp[1], lpSt_[1][1],
                              run(xActive_[1].lp[0], lpSt_[1][0], high1));
        const double high2 = run(xActive_[1].hp[1], hpSt_[1][1],
                              run(xActive_[1].hp[0], hpSt_[1][0], high1));
        const double low3  = run(xActive_[2].lp[1], lpSt_[2][1],
                              run(xActive_[2].lp[0], lpSt_[2][0], high2));
        const double high3 = run(xActive_[2].hp[1], hpSt_[2][1],
                              run(xActive_[2].hp[0], hpSt_[2][0], high2));
        const double low4  = run(xActive_[3].lp[1], lpSt_[3][1],
                              run(xActive_[3].lp[0], lpSt_[3][0], high3));
        const double high4 = run(xActive_[3].hp[1], hpSt_[3][1],
                              run(xActive_[3].hp[0], hpSt_[3][0], high3));

        double band[kNumBands];
        // LOW: phase-comp through crossovers 1,2,3 (the splits below it).
        band[0] = run(xActive_[3].ap, apSt_[0][3],
                  run(xActive_[2].ap, apSt_[0][2],
                  run(xActive_[1].ap, apSt_[0][1], low1)));
        band[1] = run(xActive_[3].ap, apSt_[1][3],
                  run(xActive_[2].ap, apSt_[1][2], low2));
        band[2] = run(xActive_[3].ap, apSt_[2][3], low3);
        band[3] = low4;
        band[4] = high4;

        // ── Optional SBC: nudge each band toward the inter-band mean ──
        if (sbcOn) {
            double sum = 0.0;
            double lvl[kNumBands];
            for (int b = 0; b < kNumBands; ++b) {
                const double a = std::fabs(band[b]);
                sbcEnv_[b] += sbcC * (a - sbcEnv_[b]);
                lvl[b] = 20.0 * std::log10(sbcEnv_[b] + 1e-9);
                sum += lvl[b];
            }
            const double mean = sum / kNumBands;
            for (int b = 0; b < kNumBands; ++b) {
                const double d = clampd((mean - lvl[b]) * sbcAmt, -6.0, 6.0);
                sbcDb[b] = d;   // last value -> meter
                band[b] *= std::pow(10.0, d / 20.0);
            }
        }

        // ── Per-band compressor + makeup ─────────────────────────────
        double wet = 0.0;
        for (int b = 0; b < kNumBands; ++b) {
            const double a = std::fabs(band[b]);
            if (a > peak[b]) peak[b] = a;
            double &env = env_[b];
            env += (a > env ? att : rel) * (a - env);

            double g = 0.0;          // gain reduction, dB (>= 0)
            double mk = 0.0;
            if (bOn[b]) {
                const double lvlDb = 20.0 * std::log10(env + 1e-9);
                const double over = lvlDb - (gThr + bThr[b]);
                if (over > 0.0) g = over * slope;
                mk = gMk + bMk[b];
            }
            grDb[b] = g;
            wet += band[b] * std::pow(10.0, (mk - g) / 20.0);
        }

        iq[2 * n] = mix * wet + (1.0 - mix) * dry;
    }

    // Publish meter snapshots (audio -> UI).
    for (int b = 0; b < kNumBands; ++b) {
        mGrDb_[b].store(grDb[b], std::memory_order_relaxed);
        mSbcDb_[b].store(sbcDb[b], std::memory_order_relaxed);
        mPeakDb_[b].store(20.0 * std::log10(peak[b] + 1e-9),
                          std::memory_order_relaxed);
    }
}

void Combinator::reset() {
    for (int k = 0; k < kNumXover; ++k) {
        lpSt_[k][0] = lpSt_[k][1] = St{};
        hpSt_[k][0] = hpSt_[k][1] = St{};
    }
    for (int b = 0; b < kNumBands; ++b) {
        for (int k = 0; k < kNumXover; ++k) apSt_[b][k] = St{};
        env_[b] = 0.0;
        sbcEnv_[b] = 0.0;
    }
}

double Combinator::bandReductionDb(int b) const {
    return (b >= 0 && b < kNumBands) ? mGrDb_[b].load(std::memory_order_relaxed) : 0.0;
}
double Combinator::bandSbcDb(int b) const {
    return (b >= 0 && b < kNumBands) ? mSbcDb_[b].load(std::memory_order_relaxed) : 0.0;
}
double Combinator::bandPeakDb(int b) const {
    return (b >= 0 && b < kNumBands) ? mPeakDb_[b].load(std::memory_order_relaxed) : -120.0;
}

}  // namespace lyra::dsp
