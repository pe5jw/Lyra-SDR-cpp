// Lyra — OutboundRing implementation.  See OutboundRing.h.
//
// §1-C Stage 3 (sign-off 2026-06-06): producer/consumer
// handshake reworked from four `std::binary_semaphore`s + 100 ms
// polling to a single `std::condition_variable` + paired `bool`
// flags + stop predicate — direct mirror of reference's
// `WaitForMultipleObjects(2, hsendEventHandles, TRUE, INFINITE)`
// at `networkproto1.c:1220` (atomic wait-all, no polling, fully
// interruptible).  All Lyra-native scaffolding (bounded
// try_acquire_for, polling poll-cadence, binary_semaphore-UB
// guard, push_timeouts_* counters) is removed.

#include "wire/OutboundRing.h"

#include <cstring>

namespace lyra::wire {

OutboundRing::OutboundRing()
    : lr_buf_(kDoublesPerBuffer, 0.0),
      iq_buf_(kDoublesPerBuffer, 0.0) {
    // Flag initial state set via in-class default initializers:
    // lr_ready_/iq_ready_  = false  (consumer waits until first fill)
    // lr_consumed_/iq_consumed_ = true (producer's first fill doesn't
    //   block — matches the reference's first-iteration behavior +
    //   §6's prior binary_sem{1} initialization).
}

OutboundRing::~OutboundRing() {
    unblock();
}

// ---- push_lr ----
//
// Producer waits for the consumer to have drained the prior fill
// (lr_consumed_ == true), then copies + sets lr_ready_ + notifies.
// Direct mirror of reference's `Inbound`/`obbuffs` ring producer
// pattern (blocking wait on `hobbuffsRun[0]` then refill).

void OutboundRing::push_lr(const double* src) {
    if (!src) return;
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&]{ return lr_consumed_ || stop_request_; });
    if (stop_request_) return;
    std::memcpy(lr_buf_.data(), src,
                kDoublesPerBuffer * sizeof(double));
    lr_consumed_ = false;
    lr_ready_    = true;
    cv_.notify_all();
}

// ---- push_iq ----
//
// Same pattern as push_lr.  Reference's `hobbuffsRun[1]` /
// `hsendEventHandles[1]` pair.

void OutboundRing::push_iq(const double* src) {
    if (!src) return;
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&]{ return iq_consumed_ || stop_request_; });
    if (stop_request_) return;
    std::memcpy(iq_buf_.data(), src,
                kDoublesPerBuffer * sizeof(double));
    iq_consumed_ = false;
    iq_ready_    = true;
    cv_.notify_all();
}

// ---- wait_pair_ready ----
//
// Consumer blocks until BOTH lr_ready_ AND iq_ready_ are set.
// Direct mirror of reference's `WaitForMultipleObjects(2,
// hsendEventHandles, TRUE, INFINITE)` at
// `networkproto1.c:1220` — atomic wait-all semantic, no
// polling.  On wake (either pair-ready OR stop), clears the
// ready flags + returns `!stop_request_`.
//
// Note: ready flags are cleared HERE (after the wait-all
// observes both true), mirroring the reference's auto-reset
// event semantics — once the consumer acquires both, the
// "filled" state is consumed and the producer's next fill is
// gated again by the cleared flags + the consumed-pair release.

bool OutboundRing::wait_pair_ready() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&]{ return (lr_ready_ && iq_ready_) || stop_request_; });
    if (stop_request_) return false;
    lr_ready_ = false;
    iq_ready_ = false;
    return true;
}

// ---- notify_consumed_pair ----
//
// Consumer side: signals BOTH producers that the buffer pair
// has been drained and may be refilled.  Direct mirror of
// reference's paired
// `ReleaseSemaphore(hobbuffsRun[0/1], 1, 0);` at
// `networkproto1.c:1199-1200`.

void OutboundRing::notify_consumed_pair() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        lr_consumed_ = true;
        iq_consumed_ = true;
    }
    cv_.notify_all();
}

// ---- unblock ----
//
// Sets the stop flag + wakes all parties.  Idempotent.
// Direct mirror of reference's shutdown via `io_keep_running =
// 0` + handle close (which interrupts WaitForMultipleObjects)
// — in C++23 we set a flag + notify_all the cv instead.  No
// UB hazard (the §6-A binary_semaphore::release() UB concern
// is gone; condition_variable::notify_all is always safe).

void OutboundRing::unblock() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_request_ = true;
    }
    cv_.notify_all();
}

}  // namespace lyra::wire
