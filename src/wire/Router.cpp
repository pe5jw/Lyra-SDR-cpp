// Lyra — Router (xrouter + twist) implementation per signed §5.
//
// See Router.h for the source-mapping commentary and idiom-
// translation rationale.  This file mirrors the reference's
// `router.c` (`xrouter`) + `networkproto1.c:263-274` (`twist`)
// behavior verbatim.

#include "wire/Router.h"

#include <algorithm>
#include <array>

namespace lyra::wire {

// =================== Global instance registry =====================
//
// Reference: `router.c` keeps an array of Router state pointers
// (`prouter[id]`) indexed by router-id.  Lyra mirrors the array as
// a file-scope static initialised lazily.  Slot 0 is the default
// HL2 RX-route Router instance.

namespace {
std::array<Router*, kRouterMaxInstances> g_routers{};
std::mutex                               g_router_registry_lock;

Router* ensure_registered(int id, Router* self) {
    std::lock_guard<std::mutex> lk(g_router_registry_lock);
    if (id >= 0 && id < kRouterMaxInstances && g_routers[id] == nullptr) {
        g_routers[id] = self;
    }
    return (id >= 0 && id < kRouterMaxInstances) ? g_routers[id] : nullptr;
}
}  // namespace

Router::Router() {
    // Auto-register at slot 0 if free (mirrors reference's
    // `create_router(0, ...)` first-instance behavior).
    ensure_registered(0, this);
}

Router::~Router() {
    std::lock_guard<std::mutex> lk(g_router_registry_lock);
    for (auto& slot : g_routers) {
        if (slot == this) slot = nullptr;
    }
}

Router* Router::instance(int id) {
    std::lock_guard<std::mutex> lk(g_router_registry_lock);
    if (id < 0 || id >= kRouterMaxInstances) return nullptr;
    return g_routers[id];
}

// =================== Sink table mutation ==========================

void Router::register_sink(int port,
                           int call_idx,
                           int ctrl_word,
                           RouterSink sink) {
    if (port      < 0 || port      >= kRouterMaxSources)   return;
    if (call_idx  < 0 || call_idx  >= kRouterMaxCalls)     return;
    if (ctrl_word < 0 || ctrl_word >= kRouterMaxCtrlWords) return;

    std::lock_guard<std::mutex> lk(update_lock_);
    sinks_[port][call_idx][ctrl_word] = std::move(sink);
}

void Router::set_control_word(int ctrl) {
    if (ctrl < 0 || ctrl >= kRouterMaxCtrlWords) return;
    control_word_.store(ctrl, std::memory_order_release);
}

void Router::set_call_count(int n) {
    if (n < 1) n = 1;
    if (n > kRouterMaxCalls) n = kRouterMaxCalls;
    std::lock_guard<std::mutex> lk(update_lock_);
    n_calls_ = n;
}

// =================== Dispatch (reference parity) ==================
//
// Reference `router.c:71` xrouter loop: for (i=0; i<a->ncall; ++i)
// invoke a->callid[port][i][ctrl] with (nsamples, data).  Lyra
// mirrors verbatim, replacing the function-pointer-id indirection
// with a direct std::function call.

void Router::dispatch(int source, int nsamples, const double* data) {
    if (source < 0 || source >= kRouterMaxSources) return;
    if (nsamples <= 0 || data == nullptr) return;

    const int ctrl = control_word_.load(std::memory_order_acquire);
    if (ctrl < 0 || ctrl >= kRouterMaxCtrlWords) return;

    int n_calls;
    std::array<RouterSink, kRouterMaxCalls> snapshot;
    {
        std::lock_guard<std::mutex> lk(update_lock_);
        n_calls = n_calls_;
        for (int i = 0; i < n_calls; ++i) {
            snapshot[i] = sinks_[source][i][ctrl];
        }
    }

    for (int i = 0; i < n_calls; ++i) {
        if (snapshot[i]) snapshot[i](nsamples, data);
    }
}

// =================== Free functions ===============================

void xrouter(Router* ptr,
             int id,
             int source,
             int nsamples,
             const double* data) {
    Router* r = ptr ? ptr : Router::instance(id);
    if (!r) return;
    r->dispatch(source, nsamples, data);
}

// twist: interleave two per-DDC streams into 4-tuples
// `{s0_I, s0_Q, s1_I, s1_Q}` per sample slot, then dispatch via
// xrouter.  Mirrors `networkproto1.c:263-274` layout exactly:
// each source stream supplies IQ pairs (so input stride is 2 doubles
// per sample for each stream), and the output staging buffer holds
// `4 * nsamples` doubles total.

void twist(int nsamples,
           const double* stream0_buf,
           const double* stream1_buf,
           double*       staging_buf,
           int           source,
           Router*       router_ptr,
           int           router_id) {
    if (nsamples <= 0)        return;
    if (!stream0_buf)         return;
    if (!stream1_buf)         return;
    if (!staging_buf)         return;

    // Interleave: for each sample slot s,
    //   staging[4s + 0] = stream0_I[s]
    //   staging[4s + 1] = stream0_Q[s]
    //   staging[4s + 2] = stream1_I[s]
    //   staging[4s + 3] = stream1_Q[s]
    for (int s = 0; s < nsamples; ++s) {
        staging_buf[4 * s + 0] = stream0_buf[2 * s + 0];
        staging_buf[4 * s + 1] = stream0_buf[2 * s + 1];
        staging_buf[4 * s + 2] = stream1_buf[2 * s + 0];
        staging_buf[4 * s + 3] = stream1_buf[2 * s + 1];
    }

    xrouter(router_ptr, router_id, source, nsamples, staging_buf);
}

}  // namespace lyra::wire
