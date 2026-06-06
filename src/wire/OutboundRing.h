// Lyra — outbound paired-buffer ring for the EP2 send path
// (§6 / §10.2 wire layer).
//
// Holds the two outbound buffers consumed by `Ep2SendThread`:
//   - `lr_buf_`  : 252 doubles = 4 × 63 interleaved L/R audio pairs
//                  (per-USB-frame: 63 stereo samples × 4 doubles)
//                  for a 2-USB-frame datagram = 126 stereo samples
//                  × 2 doubles per sample = 252 doubles total.  The
//                  producer is the audio-mixer thread.
//   - `iq_buf_`  : 252 doubles = 4 × 63 interleaved TX I/Q pairs;
//                  same sample-budget as lr_buf_.  The producer is
//                  the TX-DSP worker thread.
//
// §1-C Stage 3 (sign-off 2026-06-06):  reworked from four
// `std::binary_semaphore`s + 100 ms polling on the consumer wait
// to a SINGLE `std::condition_variable` + paired `bool` flags +
// `bool stop_request_` predicate.  This is a direct mirror of
// the reference's `WaitForMultipleObjects(2, hsendEventHandles,
// TRUE, INFINITE)` atomic wait-all semantic (`networkproto1.c:
// 1220`) — both buffers signaled-and-acquired together, no
// polling, fully interruptible by the stop flag via
// `cv_.notify_all()`.  Removes the 100-200 ms shutdown-latency
// floor the polling loop imposed AND removes the C++20 UB
// hazard around `binary_semaphore::release()` at max count
// (§6-A fix #5 was the workaround that the polling loop
// required).  The `pair_lock_` Lyra-native mutex is subsumed
// by the single `mu_` that guards the cv predicate; one lock,
// no double-locking discipline.
//
// Source mirror:
//   - `prn->outLRbufp`         — `RADIONET` LR audio scratch
//   - `prn->outIQbufp`         — `RADIONET` TX I/Q scratch
//   - `prn->hsendEventHandles[2]` — paired "producer filled" signal
//                                     consumed by
//                                     `WaitForMultipleObjects(...,
//                                     TRUE, INFINITE)` at
//                                     `networkproto1.c:1220`
//                                     → mirrored as `lr_ready_`
//                                       + `iq_ready_` bool flags
//                                       under one cv predicate
//   - `prn->hobbuffsRun[2]`    — paired "consumer drained" signal
//                                  released at `:1199-1200`
//                                  → mirrored as `lr_consumed_`
//                                    + `iq_consumed_` bool flags
//                                    under the same cv predicate
//
// Per the locked §6 Q2 + Q4 + §1.1 networking-infrastructure
// exclusion, this state lives OFF `RadioNet` in its own
// `OutboundRing` instance.  (§1.1 itself is under
// §1-C Stage 4 revisit — buffers will move into RadioNet then.)

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace lyra::wire {

class OutboundRing {
public:
    // 126 stereo samples per outbound datagram (2 USB frames × 63
    // sample-slots), each sample = 2 doubles (L+R for audio, I+Q
    // for TX baseband).  Total = 252 doubles per buffer.  Reference
    // sizes via `sizeof(complex) * 126` at `networkproto1.c:1227`.
    static constexpr int kSamplesPerDatagram = 126;
    static constexpr int kDoublesPerBuffer   = 2 * kSamplesPerDatagram;

    OutboundRing();
    ~OutboundRing();

    OutboundRing(const OutboundRing&)            = delete;
    OutboundRing& operator=(const OutboundRing&) = delete;

    // ---- Producer API ----
    //
    // Each `push_*` blocks (UNBOUNDED) on the matching "consumed"
    // flag so the producer never overwrites a buffer the consumer
    // has not yet drained.  Direct mirror of the reference's
    // blocking producer-side semaphore wait pattern (`Inbound` /
    // `obbuffs` ring producers wait on `hobbuffsRun[i]` before
    // refilling).  Once the buffer is filled, sets the matching
    // "ready" flag and `notify_all`s the cv.

    // Fill the LR audio buffer from `src` (must point to at least
    // `kDoublesPerBuffer` doubles of interleaved L+R audio).
    void push_lr(const double* src);

    // Fill the TX I/Q buffer from `src` (must point to at least
    // `kDoublesPerBuffer` doubles of interleaved I+Q).
    void push_iq(const double* src);

    // ---- Consumer API (Ep2SendThread) ----
    //
    // Blocks until BOTH `lr_ready_` and `iq_ready_` are set
    // (atomic wait-all semantic — direct mirror of reference's
    // `WaitForMultipleObjects(2, hsendEventHandles, TRUE,
    // INFINITE)` at `networkproto1.c:1220`).  Returns false if
    // `unblock()` was called (clean shutdown).
    bool wait_pair_ready();

    // Borrow read-only pointers into the buffer pair for the
    // consumer's processing pass.  Valid only between
    // `wait_pair_ready()` returning true and the subsequent
    // `notify_consumed_pair()` call.
    const double* lr_buf() const { return lr_buf_.data(); }
    const double* iq_buf() const { return iq_buf_.data(); }

    // Mutable pointers for the in-place transforms the consumer
    // performs per `networkproto1.c:1227, 1231-1239` (MOX-edge
    // IQ zeroing + optional L/R swap).
    double* lr_buf_mut() { return lr_buf_.data(); }
    double* iq_buf_mut() { return iq_buf_.data(); }

    // Signal BOTH producers that the buffer pair has been drained
    // and is free to refill.  Mirrors the paired
    // `ReleaseSemaphore(hobbuffsRun[0/1], 1, 0)` at
    // `networkproto1.c:1199-1200`.
    void notify_consumed_pair();

    // ---- Lifecycle ----
    //
    // Sets the stop flag + `notify_all`s the cv so any party
    // (consumer waiting on wait_pair_ready, producer waiting on
    // *_consumed_) wakes immediately and observes the flag.
    // Idempotent.
    void unblock();

private:
    // The two outbound buffers — sized once in the ctor, never
    // re-allocated on the hot path.
    std::vector<double> lr_buf_;
    std::vector<double> iq_buf_;

    // Single mutex + condition variable guard all four flags.
    // Replaces the four `std::binary_semaphore`s + the separate
    // `pair_lock_` mutex of the §6 design.  Direct mirror of
    // reference's two paired HANDLE arrays under one
    // `WaitForMultipleObjects` wait-all primitive.
    mutable std::mutex      mu_;
    std::condition_variable cv_;

    // Producer→consumer "buffer is filled" flags.  Set by producer
    // in `push_*`; cleared by consumer when both are observed in
    // `wait_pair_ready`.  Initial state false = consumer blocks
    // until producer fills.
    bool lr_ready_   = false;
    bool iq_ready_   = false;

    // Consumer→producer "buffer is drained" flags.  Set by
    // consumer in `notify_consumed_pair`; cleared by producer
    // when consumed in `push_*`.  Initial state true so the
    // producer's first fill does not block (matches reference's
    // first-iteration behavior and §6's prior binary_sem{1}
    // initialization).
    bool lr_consumed_ = true;
    bool iq_consumed_ = true;

    // Stop flag honored by `wait_pair_ready` AND `push_*` for
    // clean shutdown.  Set once via `unblock()`; never cleared
    // (Lyra session is one-shot wrt OutboundRing).
    bool stop_request_ = false;
};

}  // namespace lyra::wire
