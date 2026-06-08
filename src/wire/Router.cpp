// Lyra — Router (xrouter + twist) implementation per signed §5
// + §1-C Stage 2A (sign-off 2026-06-06).
//
// See Router.h for the source-mapping commentary.  This file
// mirrors the reference's `router.c` structure verbatim:  file-
// scope `g_routers[]` array (= reference's `prouter[]` at
// `router.c:30`), free functions for every operation (= reference's
// `xrouter` / `LoadRouterAll` / `LoadRouterControlBit` /
// `create_router` / `destroy_router`).  The `class Router` wrapper
// was Lyra-native scaffolding with no reference counterpart;
// removed per "do as reference, period, NO PATCHING."

#include "wire/Router.h"

#include <array>

namespace lyra::wire {

// =================== File-scope router registry ===================
//
// Reference: `__declspec(align(16)) ROUTER prouter[MAX_EXT_ROUTER];`
// at `router.c:30` — array of ROUTER pointers indexed by router-id.
// Lyra mirrors as a file-scope array of `Router*` (slot 0 is the
// default HL2 RX-route Router instance).

namespace {
std::array<Router*, kRouterMaxInstances> g_routers{};
std::mutex                               g_router_registry_lock;

void registry_register(int id, Router* self) {
    std::lock_guard<std::mutex> lk(g_router_registry_lock);
    if (id >= 0 && id < kRouterMaxInstances && g_routers[id] == nullptr) {
        g_routers[id] = self;
    }
}

void registry_unregister(Router* self) {
    std::lock_guard<std::mutex> lk(g_router_registry_lock);
    for (auto& slot : g_routers) {
        if (slot == self) slot = nullptr;
    }
}
}  // namespace

// =================== Router struct lifecycle ======================
//
// Ctor auto-registers at slot 0 (Lyra-native RAII convenience over
// reference's explicit `create_router(0)` call; behavior identical).
// Dtor unregisters.  Reference equivalents at `router.c:32-50`
// (`create_router` / `destroy_router`).

Router::Router() {
    registry_register(0, this);
}

Router::~Router() {
    registry_unregister(this);
}

// =================== Free function: router_instance ===============
//
// Lyra-native lookup helper for the `prouter[id]` direct-array
// indexing reference does inline at every `xrouter` call site.

Router* router_instance(int id) {
    std::lock_guard<std::mutex> lk(g_router_registry_lock);
    if (id < 0 || id >= kRouterMaxInstances) return nullptr;
    return g_routers[id];
}

// =================== Free function: create_router =================
//
// Mirror of reference's `create_router(id)` at `router.c:32-50`.
// Reference does `prouter[id] = (ROUTER)malloc(sizeof(router))` +
// zero-init; Lyra's `new Router` runs the ctor which self-
// registers at slot 0 (current single-Router posture; multi-id
// support deferred to v0.4-class multi-radio work).  Idempotent:
// if the slot is already occupied, no-op (matches reference's
// implicit "don't double-create" expectation — reference relies
// on a single call-site at `create_cmaster()`).  Intentional
// process-lifetime leak: matches reference posture of "malloc at
// create_cmaster, free at destroy_cmaster" with the matching free
// in destroy_router below.

void create_router(int id) {
    if (id < 0 || id >= kRouterMaxInstances) return;
    if (router_instance(id) != nullptr) return;  // idempotent
    (void) new Router();   // ctor self-registers at slot 0
    (void) id;             // multi-id slot support TBD (v0.4 multi-radio)
}

// =================== Free function: destroy_router ================
//
// Mirror of reference's `destroy_router(id, variant)` at
// `router.c:32-50`.  Deletes the Router; dtor self-unregisters.
// `variant` is accepted for reference-signature compatibility
// (the reference variant parameter selects which prouter[]
// variant to free; Lyra single-instance build ignores it).
// Caller-discipline note: ALL consumers (Ep6RecvThread,
// hl2_stream worker, etc.) MUST have stopped dispatching through
// the router before this call — otherwise an in-flight xrouter()
// dispatches into a freed Router.  The matching call site in
// HL2Stream::close() is ordered AFTER the EP6 thread joins.

void destroy_router(int id, int /*variant*/) {
    Router* r = router_instance(id);
    if (r) delete r;   // dtor calls registry_unregister
}

// =================== Free function: register_sink =================

void register_sink(Router* a,
                   int port,
                   int call_idx,
                   int ctrl_word,
                   RouterSink sink) {
    if (a         == nullptr)                              return;
    if (port      < 0 || port      >= kRouterMaxSources)   return;
    if (call_idx  < 0 || call_idx  >= kRouterMaxCalls)     return;
    if (ctrl_word < 0 || ctrl_word >= kRouterMaxCtrlWords) return;

    std::lock_guard<std::mutex> lk(a->cs_update);
    a->sinks[port][call_idx][ctrl_word] = std::move(sink);
}

// =================== Free function: set_control_word ==============

void set_control_word(Router* a, int ctrl) {
    if (a == nullptr) return;
    if (ctrl < 0 || ctrl >= kRouterMaxCtrlWords) return;
    a->controlword.store(ctrl, std::memory_order_release);
}

// =================== Free function: set_call_count ================

void set_call_count(Router* a, int n) {
    if (a == nullptr) return;
    if (n < 1) n = 1;
    if (n > kRouterMaxCalls) n = kRouterMaxCalls;
    std::lock_guard<std::mutex> lk(a->cs_update);
    a->ncalls = n;
}

// =================== Free function: xrouter (dispatch) ============
//
// Reference: `void xrouter(void* ptr, int id, int source, int
// nsamples, double* data)` at `router.c:71-108`.  Reference's
// inner loop dispatches via `a->function[port][i][ctrl]` switch
// (case 1 = Inbound, case 2 = InboundBlock de-interleave); Lyra
// uses the signed §5.8 `std::function` callback equivalent,
// invoked at slot `a->sinks[port][i][ctrl]`.
//
// Reference does `if (ptr == 0) a = prouter[id]; else a = ptr;`
// then `EnterCriticalSection(&a->cs_update)` for the dispatch
// loop.  Lyra mirrors verbatim.

void xrouter(Router* ptr,
             int id,
             int source,
             int nsamples,
             const double* data) {
    Router* a = ptr ? ptr : router_instance(id);
    if (a == nullptr)                                 return;
    if (source < 0 || source >= kRouterMaxSources)    return;
    if (nsamples <= 0 || data == nullptr)             return;

    const int ctrl = a->controlword.load(std::memory_order_acquire);
    if (ctrl < 0 || ctrl >= kRouterMaxCtrlWords)      return;

    int n_calls;
    std::array<RouterSink, kRouterMaxCalls> snapshot;
    {
        std::lock_guard<std::mutex> lk(a->cs_update);
        n_calls = a->ncalls;
        for (int i = 0; i < n_calls; ++i) {
            snapshot[i] = a->sinks[source][i][ctrl];
        }
    }

    for (int i = 0; i < n_calls; ++i) {
        if (snapshot[i]) snapshot[i](nsamples, data);
    }
}

// =================== Free function: twist =========================
//
// Reference `networkproto1.c:263-274`: interleave two per-DDC
// streams into 4-tuples `{s0_I, s0_Q, s1_I, s1_Q}` per sample
// slot, then dispatch via `xrouter()` with count `2 * nsamples`.
// Lyra mirrors verbatim (§5-A fix preserved).

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

    // §5-A fix: pass `2 * nsamples` to xrouter, matching reference
    // `networkproto1.c:273` verbatim.
    xrouter(router_ptr, router_id, source, 2 * nsamples, staging_buf);
}

}  // namespace lyra::wire
