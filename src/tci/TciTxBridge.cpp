// Lyra — TciTxBridge.  See TciTxBridge.h.

#include "tci/TciTxBridge.h"

#include <algorithm>

namespace lyra::tci {

TciTxBridge &TciTxBridge::instance() {
    static TciTxBridge inst;
    return inst;
}

void TciTxBridge::pushMono(const std::vector<float> &mono) {
    if (mono.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    q_.insert(q_.end(), mono.begin(), mono.end());
    if (q_.size() > kCapSamples) {
        const std::size_t drop = q_.size() - kCapSamples;
        q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

int TciTxBridge::queuedSamples() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return static_cast<int>(q_.size());
}

void TciTxBridge::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    q_.clear();
}

void TciTxBridge::drainInto(int nsamples, double *buff) {
    if (nsamples <= 0 || buff == nullptr) return;
    std::lock_guard<std::mutex> lk(mtx_);
    const int avail = static_cast<int>(q_.size());
    const int take  = std::min(nsamples, avail);
    int i = 0;
    for (; i < take; ++i) {
        const double s = static_cast<double>(q_.front());
        q_.pop_front();
        buff[2 * i + 0] = s;   // I
        buff[2 * i + 1] = s;   // Q = I  (Task #67: TCI digital is I=Q=mono)
    }
    // Underrun → zero-fill the remainder (reference cmaster.cs:1806-1810).
    for (; i < nsamples; ++i) {
        buff[2 * i + 0] = 0.0;
        buff[2 * i + 1] = 0.0;
    }
}

void TciTxBridge::inboundCb(int nsamples, double *buff) {
    instance().drainInto(nsamples, buff);
}

}  // namespace lyra::tci
