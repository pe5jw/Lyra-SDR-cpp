// Lyra-cpp — TX-1 component 4a: TX SPSC sample ring (impl).
// See tx_ring.h for the load-bearing SPSC discipline.

#include "tx_ring.h"

#include <algorithm>

namespace lyra::dsp {

TxRing::TxRing(int capacitySamples, int blockSize)
    : blockSize_(blockSize)
    , capacity_(std::max(capacitySamples, 4 * blockSize))
{
    buf_.assign(static_cast<size_t>(capacity_), 0.0f);
}

int TxRing::push(const float *samples, int n)
{
    if (n <= 0) return 0;

    // Producer-side reads: own inIdx_ (relaxed — we wrote it),
    // peer's outIdx_ (acquire — pair with the consumer's release).
    const std::uint64_t in  = inIdx_.load(std::memory_order_relaxed);
    const std::uint64_t out = outIdx_.load(std::memory_order_acquire);
    const std::uint64_t filled = in - out;

    if (filled + static_cast<std::uint64_t>(n) >
            static_cast<std::uint64_t>(capacity_)) {
        // Overrun.  Drop the new samples wholesale; do NOT touch
        // outIdx_.  This is the load-bearing SPSC discipline (see
        // header).  Steady-state we never get here; if we do, the
        // counter rises and a future operator diagnostic can spot
        // the systemic stall that caused it.
        overruns_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Linear-or-wrapped copy.  Two memcpys at most.
    const int cap   = capacity_;
    const int start = static_cast<int>(in % static_cast<std::uint64_t>(cap));
    const int first = std::min(n, cap - start);
    std::copy(samples, samples + first, buf_.begin() + start);
    if (first < n) {
        std::copy(samples + first, samples + n, buf_.begin());
    }

    // Release-store the new inIdx_ so the consumer's
    // acquire-load (in popBlock) sees the data we wrote above.
    inIdx_.store(in + static_cast<std::uint64_t>(n),
                 std::memory_order_release);

    // High-water update.  Producer-thread-only writer (CAS-loop
    // against any reader's eventual-consistent view).  Approximate
    // — uses the just-loaded `out` (acquire) + the new in-index;
    // a slightly-stale `out` only causes a slightly-pessimistic
    // (= larger) high-water reading, which is the safe direction
    // for an early-warning instrument.  Task #46.
    const int newFilled = static_cast<int>(
        (in + static_cast<std::uint64_t>(n)) - out);
    int prevHigh = highWaterSamples_.load(std::memory_order_relaxed);
    while (newFilled > prevHigh &&
           !highWaterSamples_.compare_exchange_weak(
               prevHigh, newFilled,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
        // CAS retries on contention; loop body intentionally empty.
    }

    // Block-grained semaphore release — once per complete
    // blockSize accumulated since the last release.  Mirrors the
    // reference's Inbound() releasing ONCE PER COMPLETE BLOCK.
    // This is producer-thread-only state — no atomic needed.
    unqueued_ += n;
    while (unqueued_ >= blockSize_) {
        unqueued_ -= blockSize_;
        sem_.release();
    }
    return n;
}

bool TxRing::popBlock(float *dst)
{
    sem_.acquire();   // blocks indefinitely; on shutdown() the
                      // releaser side bumps the count so we wake.
    if (shutdown_.load(std::memory_order_acquire)) {
        return false;
    }

    // Consumer-side reads: own outIdx_ (relaxed), peer's inIdx_
    // (acquire — pair with producer's release-store).  In a
    // correctly-running ring, in - out ≥ blockSize_ at this point
    // because the producer only releases the semaphore after
    // accumulating a full block; we don't need to re-check.
    const std::uint64_t out = outIdx_.load(std::memory_order_relaxed);
    (void)inIdx_.load(std::memory_order_acquire);   // synchronize-with

    const int cap   = capacity_;
    const int start = static_cast<int>(out % static_cast<std::uint64_t>(cap));
    const int first = std::min(blockSize_, cap - start);
    std::copy(buf_.begin() + start,
              buf_.begin() + start + first,
              dst);
    if (first < blockSize_) {
        std::copy(buf_.begin(),
                  buf_.begin() + (blockSize_ - first),
                  dst + first);
    }

    outIdx_.store(out + static_cast<std::uint64_t>(blockSize_),
                  std::memory_order_release);
    return true;
}

void TxRing::shutdown()
{
    // Idempotent.  The release wakes a waiter; subsequent waiters
    // see shutdown_ and exit immediately.  Release a generous
    // count so any in-flight popBlock() at the threshold sees us.
    if (shutdown_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // One release is enough for the single consumer, but be
    // defensive in case popBlock isn't quite at the acquire yet —
    // sem_ caps at kSemMax so this can't overflow.
    sem_.release();
}

int TxRing::filledSamples() const noexcept
{
    const std::uint64_t in  = inIdx_.load(std::memory_order_relaxed);
    const std::uint64_t out = outIdx_.load(std::memory_order_relaxed);
    return static_cast<int>(in - out);
}

} // namespace lyra::dsp
