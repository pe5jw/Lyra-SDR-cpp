// Lyra — captured noise profile (capture).  See capturedprofile.h.

#include "capturedprofile.h"

#include <algorithm>

namespace lyra::dsp {

CapturedProfile::CapturedProfile(int fftSize)
    : stft_(fftSize), fftSize_(fftSize) {
    powSum_.assign(static_cast<size_t>(fftSize_), 0.0);
    profile_.assign(static_cast<size_t>(fftSize_), 0.0);
    // Observe-only frame hook: accumulate per-bin power, leave the
    // spectrum untouched (identity transform → no audio change).  Stops
    // accumulating once the target window is reached so a slightly long
    // final feed() can't bias the average.
    stft_.setGain([this](std::vector<IqStft::Cplx> &spec) {
        if (!capturing_ || frames_ >= target_) {
            return;
        }
        const size_t n = spec.size();
        for (size_t k = 0; k < n; ++k) {
            powSum_[k] += std::norm(spec[k]);   // |X|^2 = re^2 + im^2
        }
        ++frames_;
    });
}

void CapturedProfile::begin(int sampleRate, double seconds) {
    sampleRate_ = sampleRate;
    // One frame is produced per hop (= fftSize/2) input samples.
    const double framesF =
        seconds * static_cast<double>(sampleRate) / static_cast<double>(stft_.hop());
    target_ = std::max<long long>(1, static_cast<long long>(framesF + 0.5));
    frames_ = 0;
    std::fill(powSum_.begin(), powSum_.end(), 0.0);
    valid_ = false;
    capturing_ = true;
    stft_.reset();
}

void CapturedProfile::cancel() {
    capturing_ = false;
    stft_.reset();
}

void CapturedProfile::feed(const double *iqInterleaved, int nframes) {
    if (!capturing_ || nframes <= 0) {
        return;
    }
    scratch_.clear();
    stft_.process(iqInterleaved, nframes, scratch_);   // hook accumulates
    if (frames_ >= target_) {
        const double inv = 1.0 / static_cast<double>(frames_);
        for (size_t k = 0; k < powSum_.size(); ++k) {
            profile_[k] = powSum_[k] * inv;
        }
        valid_ = true;
        capturing_ = false;
    }
}

double CapturedProfile::progress() const {
    if (target_ <= 0) {
        return 0.0;
    }
    const double p = static_cast<double>(frames_) / static_cast<double>(target_);
    return std::clamp(p, 0.0, 1.0);
}

} // namespace lyra::dsp
