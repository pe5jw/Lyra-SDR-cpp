// Lyra — frequency-calibration carrier measurement.  See FreqCalMeasure.h.

#include "dsp/FreqCalMeasure.h"

#include <algorithm>
#include <cmath>

namespace lyra::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
// Coarse Goertzel scan step (Hz).  Parabolic interpolation on the three bins
// around the peak recovers sub-Hz, so 2 Hz coarse is plenty and cheap.
constexpr double kScanStepHz = 2.0;
// EWMA weight for the running carrier-frequency average across strong windows.
constexpr double kEwmaAlpha = 0.35;
}  // namespace

void FreqCalMeasure::setSampleRate(double hz) {
    if (hz > 0.0) sampleRate_ = hz;
}

void FreqCalMeasure::setTarget(double targetHz, double halfRangeHz) {
    targetHz_  = targetHz;
    halfRange_ = std::max(20.0, halfRangeHz);
}

void FreqCalMeasure::setSnrGateDb(double db) { snrGateDb_ = db; }

void FreqCalMeasure::setWindowSize(int n) {
    if (n >= 1024) winSize_ = n;
}

void FreqCalMeasure::setRequiredWindows(int n) {
    if (n >= 1) reqWindows_ = n;
}

void FreqCalMeasure::start() {
    buf_.assign(static_cast<size_t>(winSize_), 0.0f);
    fill_       = 0;
    measuredHz_ = 0.0;
    snrDb_      = 0.0;
    strongN_    = 0;
    analyzedN_  = 0;
    haveEwma_   = false;
}

void FreqCalMeasure::process(const float* mono, int n) {
    if (!mono || n <= 0) return;
    if (static_cast<int>(buf_.size()) != winSize_)
        buf_.assign(static_cast<size_t>(winSize_), 0.0f);
    for (int i = 0; i < n; ++i) {
        buf_[static_cast<size_t>(fill_++)] = mono[i];
        if (fill_ >= winSize_) {
            analyzeWindow_();
            fill_ = 0;
        }
    }
}

void FreqCalMeasure::analyzeWindow_() {
    const int    N  = winSize_;
    const double sr = sampleRate_;

    // Hann window in place (reduces sidelobe bias on the peak estimate).
    // Cheap once per window (~a few times/sec).
    for (int i = 0; i < N; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (N - 1)));
        buf_[static_cast<size_t>(i)] = static_cast<float>(buf_[static_cast<size_t>(i)] * w);
    }

    // Goertzel magnitude at an arbitrary frequency over the windowed buffer.
    auto goertzel = [&](double freqHz) -> double {
        const double w     = 2.0 * kPi * freqHz / sr;
        const double coeff = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < N; ++i) {
            const double s0 = static_cast<double>(buf_[static_cast<size_t>(i)]) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double re = s1 - s2 * std::cos(w);
        const double im = s2 * std::sin(w);
        return std::sqrt(re * re + im * im);
    };

    // Coarse scan across [target-half, target+half].
    const double lo = std::max(1.0, targetHz_ - halfRange_);
    const double hi = std::min(sr * 0.5 - 1.0, targetHz_ + halfRange_);
    if (hi <= lo) return;
    const int nbins = static_cast<int>((hi - lo) / kScanStepHz) + 1;
    if (nbins < 3) return;

    std::vector<double> mag(static_cast<size_t>(nbins));
    int    peakIdx = 0;
    double peakMag = -1.0;
    for (int k = 0; k < nbins; ++k) {
        const double f = lo + kScanStepHz * k;
        const double m = goertzel(f);
        mag[static_cast<size_t>(k)] = m;
        if (m > peakMag) { peakMag = m; peakIdx = k; }
    }

    // Noise floor = median of the scan (robust to the one strong carrier bin).
    std::vector<double> sorted(mag);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double noise = std::max(1e-12, sorted[sorted.size() / 2]);
    snrDb_ = 20.0 * std::log10(peakMag / noise);

    // Parabolic sub-bin refinement on the three bins around the peak.
    double peakHz = lo + kScanStepHz * peakIdx;
    if (peakIdx > 0 && peakIdx < nbins - 1) {
        const double a = mag[static_cast<size_t>(peakIdx - 1)];
        const double b = mag[static_cast<size_t>(peakIdx)];
        const double c = mag[static_cast<size_t>(peakIdx + 1)];
        const double denom = (a - 2.0 * b + c);
        if (std::abs(denom) > 1e-12) {
            const double delta = 0.5 * (a - c) / denom;   // in bins, [-0.5,0.5]
            if (delta > -1.0 && delta < 1.0)
                peakHz += delta * kScanStepHz;
        }
    }

    // Only strong windows update the running average — a weak/absent carrier
    // (dead band) never corrupts the reading.
    if (snrDb_ >= snrGateDb_) {
        if (!haveEwma_) { measuredHz_ = peakHz; haveEwma_ = true; }
        else            { measuredHz_ += kEwmaAlpha * (peakHz - measuredHz_); }
        ++strongN_;
    }
    ++analyzedN_;   // every window, for the live UI tick
}

} // namespace lyra::dsp
