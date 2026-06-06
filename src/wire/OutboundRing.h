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
// Two paired binary semaphores per side gate the producer/consumer
// handshake:
//   - `lr_ready_`/`iq_ready_`        : signaled by the producer
//                                       when the buffer is filled;
//                                       consumer waits on BOTH
//                                       before reading.
//   - `lr_consumed_`/`iq_consumed_`  : signaled by the consumer
//                                       when the buffer has been
//                                       drained; producer waits on
//                                       the appropriate one before
//                                       refilling.  Initially
//                                       signaled (count=1) so the
//                                       first producer fill does not
//                                       block.
//
// Source mirror:
//   - `prn->outLRbufp`         — `RADIONET` LR audio scratch
//   - `prn->outIQbufp`         — `RADIONET` TX I/Q scratch
//   - `prn->hsendEventHandles[2]` — paired "producer filled" signal
//                                     consumed by
//                                     `WaitForMultipleObjects(...,
//                                     TRUE, INFINITE)` at
//                                     `networkproto1.c:1220`
//   - `prn->hobbuffsRun[2]`    — paired "consumer drained" signal
//                                  released at `:1199-1200`
//
// Per the locked §6 Q2 + Q4 + §1.1 networking-infrastructure
// exclusion, this state lives OFF `RadioNet` in its own
// `OutboundRing` instance owned by the wire-layer thread set.

#pragma once

#include <cstddef>
#include <mutex>
#include <semaphore>
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
    // Each `push_*` blocks on the matching "consumed" semaphore so
    // the producer never overwrites a buffer the consumer has not
    // yet drained.  UNBOUNDED acquire — direct mirror of the
    // reference's blocking semaphore wait pattern at the producer
    // side (the reference `Inbound`/`obbuffs` producer blocks
    // unbounded on the matching `hobbuffsRun[i]` semaphore).  Per
    // operator directive 2026-06-06 "do as reference, period, NO
    // PATCHING" the prior bounded 5 s `try_acquire_for` Lyra-native
    // safety belt (§6-A signed addition) was reverted in §1-C.
    // Once the buffer is filled, releases the matching "ready"
    // semaphore.

    // Fill the LR audio buffer from `src` (must point to at least
    // `kDoublesPerBuffer` doubles of interleaved L+R audio).
    void push_lr(const double* src);

    // Fill the TX I/Q buffer from `src` (must point to at least
    // `kDoublesPerBuffer` doubles of interleaved I+Q).
    void push_iq(const double* src);

    // ---- Consumer API (Ep2SendThread) ----
    //
    // `wait_pair_ready()` blocks until BOTH lr_ready_ and iq_ready_
    // are signaled (mirrors `WaitForMultipleObjects(2,
    // hsendEventHandles, TRUE, INFINITE)` — wait-all semantic).
    // Returns false if `unblock()` was called (clean shutdown).
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
    // Wake any consumer parked in `wait_pair_ready()` so it can
    // observe a stop flag and exit cleanly.  Idempotent.
    void unblock();

    // §1-C (2026-06-06): `push_timeouts_*` diagnostic counters
    // removed alongside the bounded `try_acquire_for(5s)` revert —
    // the reference does not track producer-side timeouts (it has
    // no timeout to track), so neither does Lyra under the strict
    // "do as reference, period" rule.

private:
    // The two outbound buffers — sized once in the ctor, never
    // re-allocated on the hot path.
    std::vector<double> lr_buf_;
    std::vector<double> iq_buf_;

    // Producer→consumer "buffer is filled" signals.  Released by
    // producer in `push_*`; acquired by consumer in
    // `wait_pair_ready`.  Initial count 0 = consumer blocks on
    // first wait until producer first fills.
    std::binary_semaphore lr_ready_{0};
    std::binary_semaphore iq_ready_{0};

    // Consumer→producer "buffer is drained" signals.  Released by
    // consumer in `notify_consumed_pair`; acquired by producer in
    // `push_*`.  Initial count 1 = producer's first fill does not
    // block (matches the reference's first-iteration behavior
    // where the producer fills before the consumer has signaled).
    std::binary_semaphore lr_consumed_{1};
    std::binary_semaphore iq_consumed_{1};

    // Buffer-pair mutex covers the actual fill/drain memcpy
    // moments so a producer pushing into lr_buf_ cannot race with
    // the consumer reading lr_buf_ via `lr_buf()`.  Semaphores
    // alone are sufficient for the handshake, but the lock is a
    // belt-and-suspenders guard against producer-side bugs that
    // forget to honor `lr_consumed_`.
    std::mutex pair_lock_;

    // Stop flag honored by `wait_pair_ready` for clean shutdown.
    bool stop_request_ = false;
};

}  // namespace lyra::wire
