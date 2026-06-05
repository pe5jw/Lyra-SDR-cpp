// Lyra — Router (xrouter + twist) per signed §5 parity checkpoint.
//
// Source mirror:
//   - `ChannelMaster/router.c` + `router.h` — `xrouter` dispatch
//     primitive with control-word-driven callback table
//   - `ChannelMaster/networkproto1.c:263-274` — `twist` interleave-
//     then-xrouter helper
//
// Per the locked 2026-06-05 "do as the reference does" discipline,
// `Router` lives in its own file (matching `router.c/h` separation
// in the reference) and implements the structural pattern verbatim:
// per-port callback table, runtime control word, multi-callback
// per port.  The HL2 RX-only wire-inert build today exercises only
// the simplest path (one callback per port, default control word
// 0); the full control-word complexity pays back when v0.3 PureSignal
// work re-routes per-DDC output via control-word changes without
// requiring an infrastructure rewrite.
//
// Idiom translation: the reference's C callback-table is an array
// of function-pointer ids that index into a global router state
// (`a->callid[port][i][ctrl]`).  Lyra uses `std::function`-backed
// sinks indexed by (port, callback_idx, control_word) — same
// dispatch semantics, C++23 idiom.  Signed ⚠ ACCEPTABLE DEVIATION
// per §5.8.
//
// Also: the reference's terminal dispatch via `Inbound()` (channel-
// master buffer push) → Lyra's `RouterSink` callback (operator/host
// registers what to do with the samples).  Lyra does not have a
// channel-master system; this is the §5.7 "Inbound → Lyra-native
// callback" idiom translation.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace lyra::wire {

// Capacity limits for the router callback table.  Sized to cover
// the reference's largest known per-port callback fan-out + control-
// word breadth (control-word 0..3 covers the documented MOX × PS
// state-product axis from the prior project's history).
inline constexpr int kRouterMaxSources    = 4;   // ports (DDC consumer slots)
inline constexpr int kRouterMaxCalls      = 4;   // per-port callback fan-out
inline constexpr int kRouterMaxCtrlWords  = 4;   // control-word axis breadth
inline constexpr int kRouterMaxInstances  = 4;   // global instance registry (matches reference's `prouter[]`)

// Sink callback signature: receives a sample count + interleaved
// IQ pair buffer.  Caller must NOT retain the pointer past the
// callback's return (the underlying buffer is reused on the next
// EP6 datagram).
using RouterSink = std::function<void(int n_samples, const double* iq_pairs)>;

class Router {
public:
    Router();
    ~Router();

    // Global instance registry (mirrors reference's `prouter[id]`
    // global array).  Returns nullptr if id is out of range.
    static Router* instance(int id);

    // Register a sink callback at the given (port, call_idx, ctrl)
    // slot.  Operator/host code wires sinks at session start.
    // No-op if any index is out of range.
    void register_sink(int port, int call_idx, int ctrl_word, RouterSink sink);

    // Set the runtime control word.  Used by future PS state
    // transitions to re-route DDC consumers without re-registering
    // sinks.  Default 0.
    void set_control_word(int ctrl);

    // Configure the active callback fan-out per port.  Reference
    // default = 1 (one callback per port).
    void set_call_count(int n);

    // Dispatch entry point — invokes the registered sink at slot
    // (port, [0..n_calls-1], control_word) for each active call.
    // Matches reference `xrouter` semantics from `router.c:71`.
    void dispatch(int source, int nsamples, const double* data);

private:
    std::atomic<int> control_word_{0};
    int              n_calls_{1};

    // Callback table: sinks_[port][call_idx][control_word].
    using PerCtrl  = std::array<RouterSink, kRouterMaxCtrlWords>;
    using PerCall  = std::array<PerCtrl,    kRouterMaxCalls>;
    std::array<PerCall, kRouterMaxSources> sinks_{};

    mutable std::mutex update_lock_;
};

// =================== Free functions (reference parity) =============
//
// `xrouter()` matches reference signature
// `void xrouter(void* ptr, int id, int source, int nsamples, double* data)`
// at `router.c:71`.  Either uses the explicit Router pointer or
// looks up the default instance by id.
//
// Source: `router.c:71-90+`.
void xrouter(Router* ptr,
             int id,
             int source,
             int nsamples,
             const double* data);

// `twist()` matches reference signature
// `void twist(int nsamples, int stream0, int stream1, int source)`
// at `networkproto1.c:263-274`.
//
// Reference reads from `prn->RxBuff[stream0/1]` (per-DDC IQ
// staging) and writes to `prn->RxReadBufp` (interleave staging),
// then calls `xrouter()` to dispatch.  Lyra takes the per-DDC
// source buffers + the staging buffer explicitly to honor the
// §1.1 networking-buffer exclusion from `RadioNet` (buffers live
// in `Ep6RecvThread` per §5 Q4).
//
// Layout: produces 4-tuples `{stream0_I, stream0_Q, stream1_I,
// stream1_Q}` per sample slot.  Output count is `2 * nsamples`
// (interleaved IQ-pairs from two source streams).
void twist(int nsamples,
           const double* stream0_buf,
           const double* stream1_buf,
           double*       staging_buf,
           int           source,
           Router*       router_ptr,
           int           router_id);

}  // namespace lyra::wire
