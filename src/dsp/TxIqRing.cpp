// Lyra-cpp — TxIqRing.cpp
//
// See TxIqRing.h for the full design rationale, threading contract,
// and ownership/landmine discipline.

#include "dsp/TxIqRing.h"

#include <algorithm>

namespace lyra::dsp {

namespace {

// Convert one interleaved {I, Q} pair of doubles into a single
// std::complex<float>.  The narrowing cast is the WHOLE POINT of
// doing it here (push time, off the wire-writer hot path) rather
// than per-EP2-tick in the consumer.  See TxIqRing.h.
[[nodiscard]] inline std::complex<float>
to_cf32(const double* iq_pair) noexcept {
    return std::complex<float>{
        static_cast<float>(iq_pair[0]),
        static_cast<float>(iq_pair[1]),
    };
}

}  // namespace

TxIqRing::TxIqRing(std::size_t capacity_samples)
    : buf_(capacity_samples > 0 ? capacity_samples : std::size_t{1}),
      capacity_(buf_.size()) {}

std::size_t TxIqRing::pushFromInterleavedDoubles(const double* iq_pairs,
                                                  int n_samples) noexcept {
    if (n_samples <= 0 || iq_pairs == nullptr) return 0;
    const std::size_t n = static_cast<std::size_t>(n_samples);

    std::lock_guard<std::mutex> lk(mu_);

    std::size_t dropped = 0;

    if (n >= capacity_) {
        // The incoming batch alone is ≥ ring capacity; the ENTIRE
        // current contents plus the leading (n - capacity_) of the
        // incoming batch are lost.  Reset the ring to empty and
        // write the trailing `capacity_` samples from a fresh
        // position 0.
        //
        // This is the wire-clock-master discipline: under burst
        // arrival the FRESHEST samples reach the wire.  See
        // TxIqRing.h.
        dropped = count_ + (n - capacity_);
        head_ = 0;
        tail_ = 0;
        count_ = 0;

        const double* src = iq_pairs + 2 * (n - capacity_);
        for (std::size_t i = 0; i < capacity_; ++i) {
            buf_[i] = to_cf32(src + 2 * i);
        }
        // After writing exactly capacity_ samples starting at 0,
        // the head pointer would wrap back to 0.  Tail also stays
        // at 0; the ring-full state is disambiguated from ring-
        // empty by count_ == capacity_ (vs count_ == 0).
        head_ = 0;
        count_ = capacity_;
    } else {
        // Common case: n < capacity_.  If we'd overflow, advance
        // the tail (consumer's read position) to make room — i.e.
        // discard the oldest `deficit` samples.
        const std::size_t avail = capacity_ - count_;
        if (n > avail) {
            const std::size_t deficit = n - avail;
            tail_ = (tail_ + deficit) % capacity_;
            count_ -= deficit;
            dropped = deficit;
        }
        // Push n samples, wrapping head as needed.  No second-
        // overflow case because we just made room above.
        for (std::size_t i = 0; i < n; ++i) {
            buf_[head_] = to_cf32(iq_pairs + 2 * i);
            head_ = (head_ + 1) % capacity_;
        }
        count_ += n;
    }

    totalDropped_ += dropped;
    return dropped;
}

bool TxIqRing::popBlock126(std::complex<float>* out) noexcept {
    if (out == nullptr) return false;

    std::lock_guard<std::mutex> lk(mu_);

    if (count_ < static_cast<std::size_t>(kBlockSize)) {
        // Underrun.  Non-blocking — the wire-writer falls through
        // to its existing zero-fill path at hl2_stream.cpp:2480-
        // 2488 and increments its own txIqUnderruns_ counter.
        return false;
    }

    // Pop exactly kBlockSize (126) samples, wrapping tail as
    // needed.  This is the inner loop the wire-writer hits ~380
    // times per second; the std::mutex critical section is
    // nanoseconds — see TxIqRing.h threading contract.
    for (int i = 0; i < kBlockSize; ++i) {
        out[i] = buf_[tail_];
        tail_ = (tail_ + 1) % capacity_;
    }
    count_ -= static_cast<std::size_t>(kBlockSize);
    return true;
}

std::size_t TxIqRing::size() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return count_;
}

std::uint64_t TxIqRing::totalDropped() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return totalDropped_;
}

void TxIqRing::clear() noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    head_ = 0;
    tail_ = 0;
    count_ = 0;
    // totalDropped_ is lifetime — NOT reset by clear().  Matches
    // the parent project's totalDropped semantics (operator-
    // visible diagnostic across the run, not per-stream-restart).
}

}  // namespace lyra::dsp
