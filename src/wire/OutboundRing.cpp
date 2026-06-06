// Lyra — OutboundRing implementation.  See OutboundRing.h.
//
// Producer/consumer handshake mirrors the reference's paired-event
// + paired-semaphore pattern (`networkproto1.c:1199-1200, 1220`)
// using C++20 `std::binary_semaphore` primitives.

#include "wire/OutboundRing.h"

#include <chrono>
#include <cstring>

namespace lyra::wire {

// §1-C (2026-06-06): the bounded `kProducerAcquireTimeout = 5s`
// + the `try_acquire_for(timeout)` calls in `push_lr`/`push_iq`
// + the `push_timeouts_*` diagnostic counters were ALL removed.
// Reference (`networkproto1.c` producer side via `Inbound` /
// `obbuffs`) uses unbounded blocking — Lyra now matches verbatim
// per "do as reference, period, NO PATCHING."  The §15.21
// wedged-consumer safety belt rationale that justified the
// bound was a Lyra-native deviation with no reference
// counterpart; reverted.

OutboundRing::OutboundRing()
    : lr_buf_(kDoublesPerBuffer, 0.0),
      iq_buf_(kDoublesPerBuffer, 0.0) {
    // Semaphore initial counts set via in-class initializers:
    // lr_ready_/iq_ready_ = 0 (consumer waits for first fill);
    // lr_consumed_/iq_consumed_ = 1 (producer can do the first
    // fill without blocking — matches the reference's first-
    // iteration behavior).
}

OutboundRing::~OutboundRing() {
    unblock();
}

void OutboundRing::push_lr(const double* src) {
    if (!src) return;
    // Wait for the consumer to have drained the previous fill (or
    // the initial signaled state on first call).  UNBOUNDED —
    // direct mirror of the reference's blocking producer-side
    // semaphore acquire (`Inbound` / `obbuffs` ring producers).
    lr_consumed_.acquire();
    {
        std::lock_guard<std::mutex> lk(pair_lock_);
        std::memcpy(lr_buf_.data(), src,
                    kDoublesPerBuffer * sizeof(double));
    }
    lr_ready_.release();
}

void OutboundRing::push_iq(const double* src) {
    if (!src) return;
    // UNBOUNDED — see push_lr.
    iq_consumed_.acquire();
    {
        std::lock_guard<std::mutex> lk(pair_lock_);
        std::memcpy(iq_buf_.data(), src,
                    kDoublesPerBuffer * sizeof(double));
    }
    iq_ready_.release();
}

bool OutboundRing::wait_pair_ready() {
    // Wait-all semantic matching the reference's
    // `WaitForMultipleObjects(2, hsendEventHandles, TRUE,
    // INFINITE)` at `networkproto1.c:1220`.  Acquired in fixed
    // order (lr first, then iq) — symmetric with `push_lr`/
    // `push_iq` which release in the matching order.  The
    // sequential acquire is equivalent to "wait until BOTH are
    // signaled" because both semaphores must reach count>=1
    // before both acquires can complete; the only observable
    // difference vs the reference is the order in which the
    // consumer is unblocked once the second signal arrives, and
    // that order has no protocol effect (both buffers are
    // consumed before the next datagram).
    while (true) {
        // Use a polling acquire so unblock() can wake the consumer
        // without needing to release the semaphores.  100 ms
        // polling cadence is below the operator-perceptible
        // shutdown latency budget; the hot-path acquire is the
        // bounded `try_acquire_for` below.
        if (lr_ready_.try_acquire_for(std::chrono::milliseconds(100))) {
            // Got LR; now wait for IQ.  Loop with unblock check
            // here too so a stop-during-wait wakes promptly.
            while (true) {
                if (iq_ready_.try_acquire_for(std::chrono::milliseconds(100))) {
                    return true;
                }
                if (stop_request_) {
                    // Release LR back so a future restart can
                    // re-pair cleanly.  (No-op if the ctor sets
                    // initial count 0; releasing brings it to 1
                    // which the next wait then consumes — safe.)
                    lr_ready_.release();
                    return false;
                }
            }
        }
        if (stop_request_) return false;
    }
}

void OutboundRing::notify_consumed_pair() {
    // Paired release matching the reference's
    // `ReleaseSemaphore(hobbuffsRun[0], 1, 0);
    //  ReleaseSemaphore(hobbuffsRun[1], 1, 0);` at
    // `networkproto1.c:1199-1200`.
    lr_consumed_.release();
    iq_consumed_.release();
}

void OutboundRing::unblock() {
    stop_request_ = true;
    // The consumer's `wait_pair_ready()` polls with
    // `try_acquire_for(100ms)` and re-checks `stop_request_`
    // between polls, so the worst-case shutdown latency is one
    // poll interval (~100 ms — far below operator-perceptible).
    //
    // We do NOT call `release()` on `lr_ready_`/`iq_ready_` here.
    // C++20 `std::binary_semaphore::release()` is UB if the
    // semaphore is already at max count (`least_max_value == 1`);
    // if a producer has just released without the consumer having
    // acquired yet, that precondition is violated and we trigger
    // implementation-defined behavior.  The poll loop is the
    // correct shutdown mechanism here.
}

}  // namespace lyra::wire
