// Lyra — outbound paired-buffer ring for the EP2 send path
// (§6 / §10.2 wire layer).
//
// §1-C Stage 4D (sign-off 2026-06-06):  the `OutboundRing` class
// (Lyra-native scaffolding with no reference counterpart) is
// DISSOLVED into namespace-scope free functions operating on the
// reference-mirror RadioNet fields per the §1.1 networking-
// infrastructure exclusion revert.  Sister-pattern of §1-C Stage
// 2A's Router dissolution.
//
// State now lives in RadioNet (per §1-C Stage 4A):
//   - `prn->outLRbufp`   — `_radionet` LR audio scratch
//                          (network.h:65)
//   - `prn->outIQbufp`   — `_radionet` TX I/Q scratch
//                          (network.h:66)
//   - `prn->cv_outbound` + `prn->mu_outbound` + four bool
//     flags + `prn->outbound_stop` — collapsed Lyra-native
//     mirror of reference's `hsendLRSem`/`hsendIQSem`/
//     `hsendEventHandles[2]`/`hobbuffsRun[2]` HANDLE quartet
//     (C↔C++23 idiom translation per §1-C Stage 3 design;
//     C++20 `std::counting_semaphore` lacks a native wait-all
//     primitive so `std::condition_variable` is the
//     idiomatically-correct equivalent of reference's
//     `WaitForMultipleObjects(2, hsendEventHandles, TRUE,
//     INFINITE)` at `networkproto1.c:1220`).
//
// Constants exposed at namespace scope for the wire-layer
// consumers (Ep2SendThread datagram-sample math).

#pragma once

#include <cstddef>

namespace lyra::wire {

// 126 stereo samples per outbound datagram (2 USB frames × 63
// sample-slots), each sample = 2 doubles (L+R for audio, I+Q
// for TX baseband).  Total = 252 doubles per buffer.  Reference
// sizes via `sizeof(complex) * 126` at `networkproto1.c:1227`.
inline constexpr int kOutboundSamplesPerDatagram = 126;
inline constexpr int kOutboundDoublesPerBuffer   =
    2 * kOutboundSamplesPerDatagram;

// ---- Lifecycle ----
//
// Called once at session-open AFTER `prn` is valid + BEFORE any
// producer/consumer call.  Sizes `prn->outLRbufp` +
// `prn->outIQbufp` to the per-datagram buffer size + resets all
// sync flags to the initial state (consumer blocked, producer
// signaled).  No-op if `prn == nullptr`.
void outbound_init();

// ---- Producer API ----
//
// Each `outbound_push_*` blocks (UNBOUNDED) on the matching
// "consumed" flag via `prn->cv_outbound.wait()` so the producer
// never overwrites a buffer the consumer has not yet drained.
// Direct mirror of the reference's blocking producer-side
// semaphore wait pattern (`Inbound` / `obbuffs` ring producers
// wait on `hobbuffsRun[i]` before refilling).  Once the buffer
// is filled, sets the matching "ready" flag and `notify_all`s
// the cv.  No-op if `prn == nullptr` or `src == nullptr`.
void outbound_push_lr(const double* src);
void outbound_push_iq(const double* src);

// ---- Consumer API (Ep2SendThread) ----
//
// Blocks until BOTH `prn->lr_ready` and `prn->iq_ready` are set
// (atomic wait-all semantic — direct mirror of reference's
// `WaitForMultipleObjects(2, hsendEventHandles, TRUE,
// INFINITE)` at `networkproto1.c:1220`).  Returns false if
// `outbound_unblock()` was called (clean shutdown) or
// `prn == nullptr`.
bool outbound_wait_pair_ready();

// Borrow read-only / mutable pointers into `prn->outLRbufp` +
// `prn->outIQbufp`.  Valid only between
// `outbound_wait_pair_ready()` returning true and the subsequent
// `outbound_notify_consumed_pair()` call.  Returns nullptr if
// `prn == nullptr`.
const double* outbound_lr_buf();
const double* outbound_iq_buf();
double*       outbound_lr_buf_mut();
double*       outbound_iq_buf_mut();

// Signal BOTH producers that the buffer pair has been drained
// and is free to refill.  Mirrors the paired
// `ReleaseSemaphore(hobbuffsRun[0/1], 1, 0)` at
// `networkproto1.c:1199-1200`.  No-op if `prn == nullptr`.
void outbound_notify_consumed_pair();

// Sets `prn->outbound_stop = true` + `notify_all`s the cv so
// any party (consumer waiting on outbound_wait_pair_ready,
// producer waiting on lr_consumed/iq_consumed) wakes
// immediately and observes the flag.  Idempotent.  No-op if
// `prn == nullptr`.
void outbound_unblock();

}  // namespace lyra::wire
