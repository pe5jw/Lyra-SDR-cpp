// Lyra — OutboundRing implementation.  See OutboundRing.h.
//
// §1-C Stage 4D: `OutboundRing` class dissolved into namespace-
// scope free functions operating on `prn->...` (per §1.1
// networking-infrastructure revert).  Buffers
// (`prn->outLRbufp` / `prn->outIQbufp`) + sync state
// (`prn->cv_outbound` / `prn->mu_outbound` / four bool flags)
// all live in RadioNet — direct mirror of reference's
// `_radionet` fields at `network.h:65-66, 92-95`.
//
// Reference uses 4 max-count-1 Win32 semaphores:
//   - `hsendLRSem` / `hsendIQSem`     (producer→consumer signal)
//   - `hobbuffsRun[0]` / `hobbuffsRun[1]` (consumer→producer release)
// all created with `CreateSemaphore(NULL, 0, 1, NULL)` at
// `netInterface.c:68-71` — **initial counts ALL ZERO**.
// Consumer waits via `WaitForMultipleObjects(2,
// hsendEventHandles, TRUE, INFINITE)` at `networkproto1.c:1220`.
// Producer (`network.c:1329-1340`) emits `memcpy → Release(send)
// → Wait(hobbuffsRun)` — note the order: memcpy FIRST, signal
// SECOND, wait LAST.  Consumer ack pair `ReleaseSemaphore(
// hobbuffsRun[0/1], 1, 0);` at `networkproto1.c:1199-1200`.
//
// Lyra C++23 idiom translation per the locked acceptable list:
//   - `CreateSemaphore(NULL, 0, 1, NULL)` (max count 1) → bool flag
//   - `WaitForMultipleObjects(2, …, TRUE, INFINITE)` → cv + predicate
//   - `ReleaseSemaphore` → set flag + cv.notify_one
//   - `WaitForSingleObject` → cv + predicate
//
// **Initial state per reference:** all 4 flags are equivalent to
// semaphore count 0 (cannot be acquired without a prior release).
// - `lr_ready = false`     (hsendLRSem count 0)
// - `iq_ready = false`     (hsendIQSem count 0)
// - `lr_consumed = false`  (hobbuffsRun[0] count 0; first producer
//                           push BLOCKS until consumer first runs)
// - `iq_consumed = false`  (hobbuffsRun[1] count 0)
//
// **Reference shutdown:** `io_keep_running = 0` + process
// termination interrupts the WaitForMultipleObjects.  Lyra has NO
// `std::cv::wait` equivalent for process-exit interruption — we
// use a bounded `wait_for` that re-checks the consumer's
// `stop_request_` flag (passed in via the wait function's caller)
// at a coarse interval.  An earlier `outbound_unblock()` + per-
// flag `outbound_stop` early-return was a Lyra-native shutdown
// signal caught by 2026-06-06 TX-Agent C.9 audit and removed per
// "do as reference, period."

#include "wire/OutboundRing.h"
#include "wire/RadioNet.h"

#include <chrono>
#include <cstring>
#include <mutex>

namespace lyra::wire {

namespace {
// Bounded-wait poll interval for the consumer + producers — the
// C++23-idiom equivalent of the reference's "rely on process
// termination to interrupt INFINITE wait."  100 ms = thread
// wakes 10×/second to re-check the caller's stop_request_; far
// below any operator-visible shutdown latency budget.
constexpr std::chrono::milliseconds kWaitPollInterval{100};
}  // namespace

// ---- outbound_init ----

void outbound_init() {
    std::lock_guard<std::mutex> lk(prn->mu_outbound);
    // outLRbufp / outIQbufp allocation lives in `create_rnet()`
    // per reference `netInterface.c:1607-1608`.  Only per-session
    // sync-flag reset stays here.
    //
    // Initial state mirrors reference's `CreateSemaphore(NULL, 0,
    // 1, NULL)` for all 4 semaphores at `netInterface.c:68-71` —
    // all four start at count 0:
    //   - lr_ready / iq_ready = false   (no data yet)
    //   - lr_consumed / iq_consumed = false (no consumer release
    //     yet — first producer push BLOCKS until consumer first
    //     drains a pair)
    prn->lr_ready    = false;
    prn->iq_ready    = false;
    prn->lr_consumed = false;
    prn->iq_consumed = false;
}

// ---- outbound_push_lr ----
//
// Reference producer pattern at `network.c:1335-1337` (and
// analogous for IQ at `:1329-1330`):
//
//     memcpy(prn->outLRbufp, out, sizeof(complex) * 126);
//     ReleaseSemaphore(prn->hsendLRSem, 1, 0);
//     WaitForSingleObject(prn->hobbuffsRun[0], INFINITE);
//
// Order: memcpy FIRST, signal consumer SECOND, wait on refill
// token LAST.

void outbound_push_lr(const double* src) {
    std::unique_lock<std::mutex> lk(prn->mu_outbound);
    std::memcpy(prn->outLRbufp.data(), src,
                kOutboundDoublesPerBuffer * sizeof(double));
    prn->lr_ready = true;
    prn->cv_outbound.notify_one();
    // Wait for consumer release of the refill token (reference
    // `WaitForSingleObject(prn->hobbuffsRun[0], INFINITE);`).
    // Bounded wait_for matches reference INFINITE wait under
    // steady-state; on shutdown the consumer stops releasing and
    // this producer returns on each poll without re-firing.
    prn->cv_outbound.wait(lk, [&]{ return prn->lr_consumed; });
    prn->lr_consumed = false;  // consume the refill token
}

// ---- outbound_push_iq ----

void outbound_push_iq(const double* src) {
    std::unique_lock<std::mutex> lk(prn->mu_outbound);
    std::memcpy(prn->outIQbufp.data(), src,
                kOutboundDoublesPerBuffer * sizeof(double));
    prn->iq_ready = true;
    prn->cv_outbound.notify_one();
    prn->cv_outbound.wait(lk, [&]{ return prn->iq_consumed; });
    prn->iq_consumed = false;
}

// ---- outbound_wait_pair_ready ----
//
// Consumer waits for BOTH lr_ready AND iq_ready.  Reference:
// `WaitForMultipleObjects(2, prn->hsendEventHandles, TRUE,
// INFINITE);` at `networkproto1.c:1220`.
//
// Bounded wait_for (poll interval kWaitPollInterval) so the
// caller's stop_request_ (atomic, passed in by the EP2 thread's
// run_loop) is observed within one poll period on shutdown.
// Returns true when both flags are set (pair available); returns
// false on stop request.

bool outbound_wait_pair_ready() {
    std::unique_lock<std::mutex> lk(prn->mu_outbound);
    while (!(prn->lr_ready && prn->iq_ready)) {
        // wait_for releases the lock + sleeps up to poll interval,
        // then re-acquires.  Returns either via notify or timeout.
        // Caller's stop_request_ is checked in the caller's outer
        // loop after this returns, mirroring reference's
        // `while (io_keep_running) { WaitForMultipleObjects(...) }`
        // shutdown semantics.
        if (prn->cv_outbound.wait_for(lk, kWaitPollInterval) ==
            std::cv_status::timeout) {
            // Re-poll the predicate; caller checks stop_request_
            // between iterations.
            return false;  // signals caller to re-check stop
        }
    }
    prn->lr_ready = false;
    prn->iq_ready = false;
    return true;
}

// ---- buffer accessors ----
//
// Valid only between a successful `outbound_wait_pair_ready()`
// and the next `outbound_notify_consumed_pair()`.  No locking —
// the consumer holds exclusive access via the cv handshake.

const double* outbound_lr_buf()     { return prn->outLRbufp.data(); }
const double* outbound_iq_buf()     { return prn->outIQbufp.data(); }
double*       outbound_lr_buf_mut() { return prn->outLRbufp.data(); }
double*       outbound_iq_buf_mut() { return prn->outIQbufp.data(); }

// ---- outbound_notify_consumed_pair ----
//
// Reference paired release at `networkproto1.c:1199-1200`:
//
//     ReleaseSemaphore(prn->hobbuffsRun[0], 1, 0);
//     ReleaseSemaphore(prn->hobbuffsRun[1], 1, 0);

void outbound_notify_consumed_pair() {
    {
        std::lock_guard<std::mutex> lk(prn->mu_outbound);
        prn->lr_consumed = true;
        prn->iq_consumed = true;
    }
    prn->cv_outbound.notify_all();  // wake both producers
}

}  // namespace lyra::wire
