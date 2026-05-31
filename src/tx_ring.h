// Lyra-cpp — TX-1 component 4a: TX SPSC sample ring.
//
// Single-producer / single-consumer float-sample ring for TX mic
// flow.  Producer is the RX-worker thread that decimates EP6 mic
// bytes to 48 kHz and pushes blocks via Hl2Ep6MicSource.  Consumer
// is the dedicated TX DSP worker (component 4c), which drains
// blockSize samples per wake-up, hands them to TxChannel::process()
// for fexchange0.  Signalling is a counting semaphore — producer
// releases once per complete blockSize accumulated, consumer waits
// indefinitely.  This mirrors the cmbuffs.c "Sem_BuffReady"
// (`CreateSemaphore(0, 0, 1000, 0)` + `WaitForSingleObject(INFINITE)`)
// pattern that's been the proven primitive in the C reference for
// years.
//
// ╔═══════════════════════════════════════════════════════════════╗
// ║                  STRICT SPSC DISCIPLINE                       ║
// ╠═══════════════════════════════════════════════════════════════╣
// ║  PRODUCER touches inIdx_ + unqueued_ + buf_[inIdx_..] ONLY.   ║
// ║  CONSUMER touches outIdx_ + buf_[outIdx_..] ONLY.             ║
// ║  Neither side EVER touches the other's index.                 ║
// ║                                                               ║
// ║  This is load-bearing.  Earlier TX-pump attempts this         ║
// ║  session crashed because a "drop-oldest" overrun path on the  ║
// ║  producer side mutated outIdx_ — racing the consumer's        ║
// ║  outIdx_ write under different mutexes → corrupted outIdx_ →  ║
// ║  out-of-bounds buffer access → crash.  We do NOT drop-oldest. ║
// ║  Ring is sized 8× blockSize.  The steady-state cadence is     ║
// ║  producer ~10 samples / 200 µs datagram = 48 kHz vs consumer  ║
// ║  64 samples / ~1.3 ms = 48 kHz — perfectly matched.  Overrun  ║
// ║  is structurally unreachable in steady state.                 ║
// ║                                                               ║
// ║  Transients (TX-active EP2 lockstep stalls — Qt main-thread   ║
// ║  paint storm, AK4951 buffer jitter, S2 timer slip) CAN tip    ║
// ║  the ring over for a window.  Bench instruments:              ║
// ║    overrunCount()      — pushes the producer rejected         ║
// ║    highWaterSamples()  — peak fill since startup              ║
// ║  Earlier Task #46 attempt papered overruns by bumping the     ║
// ║  ring to 32× (~43 ms headroom); operator-flagged correctly    ║
// ║  as a band-aid masking the cause.  Reverted.  Treat overruns  ║
// ║  + high-water-near-capacity as a diagnostic signal that the   ║
// ║  underlying stall needs investigation, NOT as a sizing knob.  ║
// ║                                                               ║
// ║  If overrun DOES happen, the producer drops the NEW samples   ║
// ║  and ticks the overrun counter — outIdx_ is never touched.    ║
// ╚═══════════════════════════════════════════════════════════════╝
//
// Indices are monotonically growing size_t; the buffer is indexed
// via `idx % capacity_`.  This avoids the "is the ring full or
// empty?" ambiguity that plagues wrapped-index schemes — `filled =
// inIdx - outIdx` is always unambiguous (wrap happens at the 2^64
// horizon, well after the heat-death-of-the-universe horizon).
//
// Threading rules:
//   * push() called from ONE thread only (the RX worker).
//   * popBlock() called from ONE thread only (the TX worker).
//   * shutdown() may be called from any thread; wakes a waiting
//     consumer so popBlock() returns false on the next iteration.
//   * Diagnostic getters (filledSamples / overrunCount) are safe
//     from any thread but may observe a brief inconsistency in a
//     rapid producer/consumer cross-fire — they are bench-instrument
//     only, never load-bearing for safety decisions.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <semaphore>
#include <vector>

namespace lyra::dsp {

class TxRing {
public:
    // Counting-semaphore max count.  At 48 kHz with blockSize=126
    // that's ~380 wake-ups/sec; the consumer would have to be
    // starved for ~2.6 seconds for the count to reach 1000 — well
    // past the point we'd be ticking overruns instead.  Matches
    // the reference's CreateSemaphore(0, 0, 1000, 0).
    static constexpr std::ptrdiff_t kSemMax = 1000;

    // capacitySamples must be ≥ 4 * blockSize for steady-state
    // operation; the ctor enforces that and rounds UP if needed.
    // blockSize is the consumer's per-wake-up drain unit (=
    // TxChannel::in_size = 126 at 48 kHz / 2.625 ms blocks).
    TxRing(int capacitySamples, int blockSize);

    TxRing(const TxRing &)            = delete;
    TxRing &operator=(const TxRing &) = delete;

    // PRODUCER side.  Push `n` samples from `samples` into the
    // ring.  Releases the semaphore once per complete blockSize
    // accumulated since the last release.  Returns the number of
    // samples ACCEPTED (== n on success, 0 on overrun).  On
    // overrun, the new samples are dropped (no partial accept) and
    // overrunCount() ticks — outIdx_ is NEVER touched.
    //
    // Called ONLY from the producer thread.
    int push(const float *samples, int n);

    // CONSUMER side.  Wait for a complete blockSize to be ready,
    // then drain it into `dst` (which must hold at least blockSize
    // floats).  Returns true on success; false if shutdown() has
    // been called and there's nothing left to drain.
    //
    // Called ONLY from the consumer thread.  Blocks indefinitely.
    bool popBlock(float *dst);

    // Wake any waiting consumer; subsequent popBlock() calls
    // return false.  Idempotent.  Safe from any thread.
    void shutdown();

    // Diagnostics.  Safe from any thread (relaxed atomic reads).
    int       blockSize()       const noexcept { return blockSize_; }
    int       capacitySamples() const noexcept { return capacity_; }
    long long overrunCount()    const noexcept {
        return overruns_.load(std::memory_order_relaxed);
    }
    int       filledSamples()   const noexcept;
    // Peak fill (in samples) since startup.  Updated on every push()
    // after the producer accepts the new samples.  Task #46
    // instrument — tells the bench whether the 32× ring bump was
    // actually needed and how close we got to overrun.  Strictly
    // monotonic; no reset path (a session-scope tell, like the
    // overrunCount).
    int       highWaterSamples() const noexcept {
        return highWaterSamples_.load(std::memory_order_relaxed);
    }

private:
    std::vector<float> buf_;
    int blockSize_;
    int capacity_;

    // PRODUCER-OWNED (only the producer thread writes; the
    // producer can read freely; the consumer reads via the atomic
    // ordering established by the producer's release-store).
    std::atomic<std::uint64_t> inIdx_{0};
    int unqueued_ = 0;   // samples queued since last sem release —
                         // producer-thread-only, no atomic needed.

    // CONSUMER-OWNED (symmetric).
    std::atomic<std::uint64_t> outIdx_{0};

    // SHARED signalling.
    std::counting_semaphore<kSemMax> sem_{0};
    std::atomic<bool>      shutdown_{false};
    std::atomic<long long> overruns_{0};
    // High-water mark of ring occupancy in samples.  Producer-side
    // update only (relaxed CAS-loop in push()); readable from any
    // thread.  Task #46 diagnostic.
    std::atomic<int>       highWaterSamples_{0};
};

} // namespace lyra::dsp
