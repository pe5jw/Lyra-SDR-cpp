// Lyra-cpp — #89 Voice keyer / RX recorder, Stage C: capture core.  See ClipRecorder.h.

#include "tx/ClipRecorder.h"

namespace lyra::tx {

void ClipRecorder::start(Source src) {
    std::lock_guard<std::mutex> lk(mtx_);
    buf_.clear();
    buf_.reserve(48000 * 8);            // ~8 s headroom; grows as needed
    source_ = src;
    size_.store(0, std::memory_order_relaxed);
    recording_.store(true, std::memory_order_release);
}

void ClipRecorder::feedMicPairs(const double *iq_pairs, int n_pairs) {
    if (!recording_.load(std::memory_order_acquire) || source_ != Source::Mic) return;
    if (!iq_pairs || n_pairs <= 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!recording_.load(std::memory_order_relaxed) || source_ != Source::Mic) return;
    for (int i = 0; i < n_pairs && buf_.size() < static_cast<std::size_t>(kMaxSamples); ++i)
        buf_.push_back(static_cast<float>(iq_pairs[2 * i]));   // I = mic; Q ignored
    size_.store(static_cast<int>(buf_.size()), std::memory_order_relaxed);
}

void ClipRecorder::feedRxMono(const float *mono, int n) {
    if (!recording_.load(std::memory_order_acquire) || source_ != Source::Rx) return;
    if (!mono || n <= 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!recording_.load(std::memory_order_relaxed) || source_ != Source::Rx) return;
    for (int i = 0; i < n && buf_.size() < static_cast<std::size_t>(kMaxSamples); ++i)
        buf_.push_back(mono[i]);
    size_.store(static_cast<int>(buf_.size()), std::memory_order_relaxed);
}

void ClipRecorder::feedRxStereoDup(const double *audio, int nframes) {
    if (!recording_.load(std::memory_order_acquire) || source_ != Source::Rx) return;
    if (!audio || nframes <= 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!recording_.load(std::memory_order_relaxed) || source_ != Source::Rx) return;
    for (int f = 0; f < nframes && buf_.size() < static_cast<std::size_t>(kMaxSamples); ++f)
        buf_.push_back(static_cast<float>(audio[2 * f]));   // L (== R, mono-dup)
    size_.store(static_cast<int>(buf_.size()), std::memory_order_relaxed);
}

std::vector<float> ClipRecorder::stop() {
    std::lock_guard<std::mutex> lk(mtx_);
    recording_.store(false, std::memory_order_release);
    size_.store(0, std::memory_order_relaxed);
    std::vector<float> out;
    out.swap(buf_);
    return out;
}

int ClipRecorder::durationMs() const {
    return static_cast<int>(size_.load(std::memory_order_relaxed) * 1000ll / 48000);
}

} // namespace lyra::tx
