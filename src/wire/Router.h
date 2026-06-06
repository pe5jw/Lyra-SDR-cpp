// Lyra — Router (xrouter + twist) per signed §5 parity checkpoint.
//
// Source mirror:
//   - `ChannelMaster/router.c` + `router.h` — `xrouter` dispatch
//     primitive with control-word-driven callback table.
//   - `ChannelMaster/networkproto1.c:263-274` — `twist` interleave-
//     then-xrouter helper.
//
// §1-C Stage 2A (sign-off 2026-06-06): refactored from `class
// Router` (with member methods `dispatch`/`register_sink`/
// `set_control_word`/`set_call_count`/`instance`) into a **plain
// data struct + file-scope free functions** — direct mirror of
// the reference's `ROUTER` typedef'd struct + `xrouter()` /
// `LoadRouterAll()` / `LoadRouterControlBit()` / `create_router()`
// / `destroy_router()` / `flush_router()` free functions in
// `router.c`.  The §5.8 signed deviation (std::function callback
// in lieu of reference's function-pointer-id) is retained per
// signed sign-off — that is a genuine C↔C++ idiom translation.
// What §1-C removes is the gratuitous class wrapper that had no
// reference counterpart.
//
// File-scope `g_routers[kRouterMaxInstances]` mirrors reference's
// `prouter[MAX_EXT_ROUTER]` at `router.c:30`.  Lifetime managed
// via RAII (struct ctor registers, dtor unregisters) — small
// C++23 convenience over reference's explicit `create_router`/
// `destroy_router` calls; behaviorally identical.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace lyra::wire {

// Capacity limits for the router callback table.  Sized to cover
// the reference's per-port callback fan-out + control-word
// breadth.  Reference `router.c:29` uses `MAX_EXT_ROUTER=4`.
inline constexpr int kRouterMaxSources    = 4;   // ports (DDC consumer slots)
inline constexpr int kRouterMaxCalls      = 4;   // per-port callback fan-out
inline constexpr int kRouterMaxCtrlWords  = 4;   // control-word axis breadth
inline constexpr int kRouterMaxInstances  = 4;   // global instance registry (= MAX_EXT_ROUTER)

// Sink callback signature: receives a sample count + interleaved
// IQ pair buffer.  §5.8 ⚠ signed acceptable deviation —
// `std::function` in lieu of reference's `int callid[port][i][ctrl]`
// function-pointer-id indirection.  C↔C++23 idiom translation.
using RouterSink = std::function<void(int n_samples, const double* iq_pairs)>;

// Per-(port × call_idx × control_word) sink table types.
using RouterPerCtrl = std::array<RouterSink, kRouterMaxCtrlWords>;
using RouterPerCall = std::array<RouterPerCtrl, kRouterMaxCalls>;

// Plain data struct mirroring reference `ROUTER` typedef
// (`router.h` / `router.c:30`).  Ctor/dtor handle the
// `prouter[id]`-equivalent registration.
struct Router {
    Router();
    ~Router();

    Router(const Router&)            = delete;
    Router& operator=(const Router&) = delete;

    std::atomic<int>                          controlword{0};   // reference name verbatim
    int                                       ncalls{1};        // reference name verbatim
    std::array<RouterPerCall, kRouterMaxSources> sinks{};
    mutable std::mutex                        cs_update;        // reference field name (`a->cs_update` :39)
};

// =================== File-scope free functions ====================
//
// Direct mirror of `router.c`'s function set.  Replace what was
// previously `Router::method(...)` member calls.

// `xrouter()` — reference signature
// `void xrouter(void* ptr, int id, int source, int nsamples,
//               double* data)` at `router.c:71`.  Looks up by id
// if `ptr == nullptr`, else uses `ptr` directly — verbatim
// reference pattern.
void xrouter(Router* ptr,
             int id,
             int source,
             int nsamples,
             const double* data);

// `twist()` — reference signature mirror at
// `networkproto1.c:263-274`.  Interleaves two per-DDC streams
// into 4-tuples `{s0_I, s0_Q, s1_I, s1_Q}` per sample slot,
// then dispatches via `xrouter()` with count `2 * nsamples`.
// Reference reads from `prn->RxBuff[stream0/1]` (per-DDC IQ
// staging) and writes to `prn->RxReadBufp`; Lyra takes the
// per-DDC source buffers + the staging buffer explicitly per
// the §1.1 networking-buffer exclusion (revisited in §1-C
// Stage 4).
void twist(int nsamples,
           const double* stream0_buf,
           const double* stream1_buf,
           double*       staging_buf,
           int           source,
           Router*       router_ptr,
           int           router_id);

// `router_instance(id)` — Lyra-native lookup helper mirroring
// reference's `prouter[id]` direct-array indexing.  Returns
// nullptr if id is out of range OR the slot is empty.
Router* router_instance(int id);

// `register_sink()` — sets the callback at the given
// (port, call_idx, ctrl_word) slot.  Lyra-native equivalent
// of reference's `LoadRouterAll(...)` bulk-loader; signed §5.7
// idiom translation (`Inbound`→Lyra-native callback).  No-op
// if any index is out of range.
void register_sink(Router* a,
                   int port,
                   int call_idx,
                   int ctrl_word,
                   RouterSink sink);

// `set_control_word()` — mirrors reference's
// `LoadRouterControlBit(ptr, id, var_number, bit)` at
// `router.c:146-155` but settable-as-whole-word instead of
// bit-by-bit.  No-op if `ctrl` is out of range.
void set_control_word(Router* a, int ctrl);

// `set_call_count()` — mirrors reference setting `a->ncalls`
// inside `LoadRouterAll` (router.c:129).  Clamps to
// `[1, kRouterMaxCalls]`.
void set_call_count(Router* a, int n);

}  // namespace lyra::wire
