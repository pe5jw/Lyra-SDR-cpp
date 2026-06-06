// Lyra — OutboundRing implementation.  See OutboundRing.h.
//
// §1-C Stage 4D: `OutboundRing` class dissolved into namespace-
// scope free functions operating on `prn->...` (per §1.1
// networking-infrastructure revert).  Buffers
// (`prn->outLRbufp` / `prn->outIQbufp`) + sync state
// (`prn->cv_outbound` / `prn->mu_outbound` / four bool flags +
// `prn->outbound_stop`) all live in RadioNet — direct mirror
// of reference's `_radionet` fields at `network.h:65-66, 92-95`.

#include "wire/OutboundRing.h"
#include "wire/RadioNet.h"

#include <cstring>
#include <mutex>

namespace lyra::wire {

// ---- outbound_init ----

void outbound_init() {
    if (prn == nullptr) return;
    std::lock_guard<std::mutex> lk(prn->mu_outbound);
    prn->outLRbufp.assign(kOutboundDoublesPerBuffer, 0.0);
    prn->outIQbufp.assign(kOutboundDoublesPerBuffer, 0.0);
    // Initial state: consumer blocked (ready=false), producer
    // signaled (consumed=true so first push doesn't block) —
    // matches reference first-iteration behavior.
    prn->lr_ready     = false;
    prn->iq_ready     = false;
    prn->lr_consumed  = true;
    prn->iq_consumed  = true;
    prn->outbound_stop = false;
}

// ---- outbound_push_lr ----
//
// Producer waits for the consumer to have drained the prior fill
// (lr_consumed == true), then copies + sets lr_ready + notifies.
// Direct mirror of reference's `Inbound`/`obbuffs` ring producer
// pattern (blocking wait on `hobbuffsRun[0]` then refill).

void outbound_push_lr(const double* src) {
    if (prn == nullptr || src == nullptr) return;
    std::unique_lock<std::mutex> lk(prn->mu_outbound);
    prn->cv_outbound.wait(lk,
        [&]{ return prn->lr_consumed || prn->outbound_stop; });
    if (prn->outbound_stop) return;
    std::memcpy(prn->outLRbufp.data(), src,
                kOutboundDoublesPerBuffer * sizeof(double));
    prn->lr_consumed = false;
    prn->lr_ready    = true;
    prn->cv_outbound.notify_all();
}

// ---- outbound_push_iq ----
//
// Same pattern as outbound_push_lr.  Reference's
// `hobbuffsRun[1]` / `hsendEventHandles[1]` pair.

void outbound_push_iq(const double* src) {
    if (prn == nullptr || src == nullptr) return;
    std::unique_lock<std::mutex> lk(prn->mu_outbound);
    prn->cv_outbound.wait(lk,
        [&]{ return prn->iq_consumed || prn->outbound_stop; });
    if (prn->outbound_stop) return;
    std::memcpy(prn->outIQbufp.data(), src,
                kOutboundDoublesPerBuffer * sizeof(double));
    prn->iq_consumed = false;
    prn->iq_ready    = true;
    prn->cv_outbound.notify_all();
}

// ---- outbound_wait_pair_ready ----
//
// Consumer blocks until BOTH lr_ready AND iq_ready are set.
// Direct mirror of reference's `WaitForMultipleObjects(2,
// hsendEventHandles, TRUE, INFINITE)` at
// `networkproto1.c:1220` — atomic wait-all semantic, no
// polling.  On wake (either pair-ready OR stop), clears the
// ready flags + returns `!outbound_stop`.

bool outbound_wait_pair_ready() {
    if (prn == nullptr) return false;
    std::unique_lock<std::mutex> lk(prn->mu_outbound);
    prn->cv_outbound.wait(lk, [&]{
        return (prn->lr_ready && prn->iq_ready) || prn->outbound_stop;
    });
    if (prn->outbound_stop) return false;
    prn->lr_ready = false;
    prn->iq_ready = false;
    return true;
}

// ---- buffer accessors ----
//
// Callers must hold the contract that valid only between a
// successful `outbound_wait_pair_ready()` and the next
// `outbound_notify_consumed_pair()`.  No locking here — the
// consumer holds exclusive access by virtue of the cv
// handshake.

const double* outbound_lr_buf() {
    return (prn == nullptr) ? nullptr : prn->outLRbufp.data();
}
const double* outbound_iq_buf() {
    return (prn == nullptr) ? nullptr : prn->outIQbufp.data();
}
double* outbound_lr_buf_mut() {
    return (prn == nullptr) ? nullptr : prn->outLRbufp.data();
}
double* outbound_iq_buf_mut() {
    return (prn == nullptr) ? nullptr : prn->outIQbufp.data();
}

// ---- outbound_notify_consumed_pair ----
//
// Consumer side: signals BOTH producers that the buffer pair
// has been drained and may be refilled.  Direct mirror of
// reference's paired
// `ReleaseSemaphore(hobbuffsRun[0/1], 1, 0);` at
// `networkproto1.c:1199-1200`.

void outbound_notify_consumed_pair() {
    if (prn == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(prn->mu_outbound);
        prn->lr_consumed = true;
        prn->iq_consumed = true;
    }
    prn->cv_outbound.notify_all();
}

// ---- outbound_unblock ----
//
// Sets the stop flag + wakes all parties.  Idempotent.
// Direct mirror of reference's shutdown via
// `io_keep_running = 0` + handle close (which interrupts
// WaitForMultipleObjects) — in C++23 we set a flag +
// notify_all the cv instead.

void outbound_unblock() {
    if (prn == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(prn->mu_outbound);
        prn->outbound_stop = true;
    }
    prn->cv_outbound.notify_all();
}

}  // namespace lyra::wire
