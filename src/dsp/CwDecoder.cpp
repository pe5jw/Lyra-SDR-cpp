// Lyra RX CW decoder — adapter around the faithful fldigi port.  See
// CwDecoder.h and dsp/cw_fldigi/fldigi_cw.h.
#include "dsp/CwDecoder.h"

#include <cmath>

namespace lyra::dsp {

namespace {
constexpr int    kNTaps   = 49;      // decimation LPF length (odd, linear phase)
constexpr double kCutHz   = 3400.0;  // LPF cutoff (below the 4 kHz 8 k-Nyquist)
constexpr double kPi      = 3.14159265358979323846;

inline double sinc(double x) {
    return (std::fabs(x) < 1e-9) ? 1.0 : std::sin(kPi * x) / (kPi * x);
}
}  // namespace

CwDecoder::CwDecoder() {
    rx_.onText = [this](const std::string& s) { if (onText) onText(s); };
    rx_.onWpm  = [this](int w)                { if (onWpm)  onWpm(w);  };
    rebuildDecimator();
}

void CwDecoder::rebuildDecimator() {
    decim_ = std::max(1, (int)std::lround(inRate_ / 8000.0));

    // Hamming-windowed sinc low-pass at kCutHz (glue to hand fldigi 8 kHz
    // audio; NOT part of fldigi's decode).  Normalised to unity DC gain.
    firCoef_.assign(kNTaps, 0.0);
    const double fc = kCutHz / inRate_;            // normalised (0..0.5)
    const int    M  = kNTaps - 1;
    double sum = 0.0;
    for (int i = 0; i < kNTaps; ++i) {
        const double w = 0.54 - 0.46 * std::cos(2.0 * kPi * i / M);   // Hamming
        const double h = 2.0 * fc * sinc(2.0 * fc * (i - M / 2.0)) * w;
        firCoef_[i] = h;
        sum += h;
    }
    if (sum != 0.0) for (double& c : firCoef_) c /= sum;

    hist_.assign(kNTaps, 0.0);
    histPos_ = 0;
    phase_   = 0;
    out8k_.clear();
    out8k_.reserve(2048);
}

void CwDecoder::setSampleRate(double hz) {
    if (hz > 0.0 && hz != inRate_) { inRate_ = hz; rebuildDecimator(); }
}
void CwDecoder::setToneHz(double hz)        { toneHz_ = hz; rx_.setToneHz(hz); }
void CwDecoder::setBandwidthHz(int hz)      { rx_.setBandwidthHz(hz); }
void CwDecoder::setSpeedWpm(int wpm)        { rx_.setSpeedWpm((double)wpm); }
void CwDecoder::setTracking(bool on)        { rx_.setTracking(on); }
void CwDecoder::setMatchedFilter(bool on)   { rx_.setMatchedFilter(on); }
void CwDecoder::setSquelch(bool on, double value) { rx_.setSquelch(on, value); }

void CwDecoder::reset() {
    rx_.reset();
    hist_.assign(kNTaps, 0.0);
    histPos_ = 0;
    phase_   = 0;
    out8k_.clear();
}

void CwDecoder::process(const float* mono, int nframes) {
    if (nframes <= 0) return;
    out8k_.clear();
    for (int i = 0; i < nframes; ++i) {
        hist_[histPos_] = (double)mono[i];
        histPos_ = (histPos_ + 1) % kNTaps;
        if (++phase_ >= decim_) {
            phase_ = 0;
            // dot product: newest sample is at (histPos_-1)
            double y = 0.0;
            int idx = (histPos_ - 1 + kNTaps) % kNTaps;
            for (int k = 0; k < kNTaps; ++k) {
                y += firCoef_[k] * hist_[idx];
                idx = (idx - 1 + kNTaps) % kNTaps;
            }
            out8k_.push_back(y);
        }
    }
    if (!out8k_.empty())
        rx_.rxProcess(out8k_.data(), (int)out8k_.size());
}

}  // namespace lyra::dsp
