// Lyra-cpp — ILV.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files:
//   - ChannelMaster/ilv.h: struct ilv typedef + 7 public function decls
//   - ChannelMaster/ilv.c: implementation (ported in Stage C.1+)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// **C → C++23 idiom translations applied** (every translation
// preserves observable behaviour; see per-symbol comments for
// exceptions):
//
//   - `struct ilv` typedef         → `struct ILV` with in-class
//                                    initializers + RAII members.
//                                    Layout is Lyra-private (no
//                                    DLL-ABI constraint); semantic
//                                    contract preserved.
//   - `volatile long run/what`     → `std::atomic<std::uint32_t>`
//      + `_InterlockedAnd / Inter-   with .load() & mask for the
//      lockedBitTestAndSet / Reset`  reference InterlockedAnd reads
//                                    and fetch_or(1u<<bit) /
//                                    fetch_and(~(1u<<bit)) for the
//                                    bit-test-and-set/reset writes.
//                                    Used at ilv.c:61 (run AND-1),
//                                    :66 (what AND-0xffffffff),
//                                    :108/:110 (SetILVRun bit 0),
//                                    :118/:120 (SetILVWhat stream
//                                    bit), :140/:142 (pSetILVRun).
//                                    Cross-thread: xilv (xmtr pump
//                                    thread) reads vs SetILV*
//                                    operator-setter writes.
//   - `double* outbuff`            → `std::vector<double>` RAII;
//      via `malloc0(...)` /          sized `nin * insize *
//      `_aligned_free(...)`          sizeof(complex)` = 2 * nin *
//                                    insize doubles, allocated in
//                                    create_ilv.  Zero-init matches
//                                    malloc0 semantics; freed on
//                                    ILV destruction.
//   - `void(*Outbound)(int id,     → `std::function<void(int, int,
//      int nsamples, double* buff)`   double*)>` (per docs/RULES.md
//                                    §5.8 signed-off translation;
//                                    matches AAMix.h pattern + the
//                                    CMaster Stage A
//                                    OutboundCallback alias exactly
//                                    so SendpOutboundTx's stored
//                                    callback can be passed straight
//                                    through SetILVOutputPointer).
//   - reference `pcm->xmtr[id].    → Lyra-native `pilv[MAX_EXT_ILV]`
//      pilv` lookup pattern in       central bank (mirror of
//      every SetILV* setter          paamix[] from AAMix).  The
//                                    `int xmtr_id` setter parameter
//                                    semantically maps to a bank
//                                    index in this stage; Stage E
//                                    (TxChannel/xmtr reconciliation)
//                                    may later migrate the storage
//                                    into a pcm->xmtr struct without
//                                    changing setter signatures.
//                                    Identical Lyra-native sidestep
//                                    to the AAMix choice — preserves
//                                    reference call-site portability
//                                    and avoids painting Stage E
//                                    into a corner.
//   - `__declspec(dllexport)` /    → DROPPED.  Lyra-cpp's ILV is
//      `PORT` annotations on         in-process C++ code, not a
//      reference public funcs        DLL-exported surface (the
//                                    cmaster pump invokes it
//                                    directly).
//   - `void* ptr` opaque ILV       → typed `ILV*`.  No reference
//      handles in the reference      ptr-vs-id dispatch pattern in
//      API                           ilv.h (unlike aamix.h:106 et
//                                    al) — ilv's setters use only
//                                    the `xmtr_id` path.  Lyra-cpp
//                                    setters accept the same
//                                    `xmtr_id` argument; the typed
//                                    pointer is exposed only via
//                                    `create_ilv` return value +
//                                    `pSetILV*` direct-pointer
//                                    variants (matching ilv.h:56-58
//                                    `pSetILVRun` / `pSetILVInsize`
//                                    byte-for-byte).
//
// `[Lyra-native]` markers identify additions that are not part
// of the reference port.  Stage C is pure port; the only
// [Lyra-native] addition is the `pilv[]` central bank pattern
// (documented above) that mirrors AAMix's identical sidestep.
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.  See docs/RULES.md AA. 2026-06-08 Amendment for
// the project-level rule change that permits this port.
//
// ---------------------------------------------------------------
//
// **Stage C sub-commit plan** (per docs/THETIS_DIRECT_PORT_PLAN.md
// §Stage Index; mirrors Stage B's B.0-B.6 cadence):
//
//   Stage C.0 (THIS COMMIT): ILV.h struct typedef + 7 function
//     decls + full attribution + idiom-translation log + central
//     bank declaration.  No implementation yet (the .cpp body
//     lands in C.1-C.2).  Wire-inert (decls only — nothing
//     constructs an ILV).
//
//   Stage C.1 (PENDING): create_ilv + destroy_ilv + xilv pump
//     body (the j/k interleave loop + run-off memcpy fallback).
//     ILV constructable but no setters yet.
//
//   Stage C.2 (PENDING): 7 setters (SetILV{OutputPointer, Run,
//     What, Insize, OutboundId} + pSetILV{Run, Insize}) with
//     atomic semantics + central bank lookup + a synthetic
//     2-stream unit test asserting bit-exact output.
//
//   Stage C.3 (DEFER): wire `SetILVOutputPointer(0, pcm->
//     OutboundTx)` into Stage A's `CMaster::SendpOutboundTx`
//     stub (the architectural hand-off that Stage A wired the
//     storage slot for at CMaster.cpp:82-86, line 85's
//     `// Stage C PENDING:` comment).  This is the
//     wire-effective commit; operator HL2 bench-gate matters
//     here.  Stage C.3 will mirror Stage B.6.b's discipline:
//     audit for any Stage-A no-op stub that could clobber
//     ILV->Outbound (the root cause of the Stage B silent-audio
//     defect at RadioNet.cpp:272 — do NOT re-detonate).
//
//   Stage C.4 (DEFER): build verify + RX-only smoke (Stage C
//     is TX-quiescent; nothing in C.0-C.3 exercises a live TX
//     yet — the xmtr pump body that drives xilv lands in
//     Stage D xcmaster port).  Confirms no regression in the
//     already-shipped Stage B AAMix RX path.
//
// Each C.x sub-stage ships independently with a build-green
// verify and a push.  HL2 bench gate per stage = wire-inert
// (TX path untouched) until C.3 (CMaster wire-up).

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lyra::wdsp {

// [Lyra-native] Process-wide central bank size for ILV instances.
// Reference ilv has NO equivalent constant — its setters reach the
// per-xmtr ILV via `pcm->xmtr[xmtr_id].pilv`.  Lyra-cpp does not yet
// have the full pcm->xmtr struct (that lands in Stage E xmtr
// reconciliation work), so this central bank serves the same role
// as AAMix's paamix[] until Stage E migrates the storage.  Bank
// size = 2 leaves room for an RX2-era second xmtr without churn.
inline constexpr int MAX_EXT_ILV = 2;

// Reference `struct _ilv` typedef at ilv.h:30-39.
//
// Lyra-cpp uses `struct ILV` (not class) to mirror the reference's
// plain-aggregate posture — every field is public and the
// operations are free functions (matching the reference API
// surface).  In-class initializers replace the reference's implicit
// zero-init via malloc0.
struct ILV {
    // Reference ilv.h:32 — interleaver runs when bit 0 is set.
    // Atomic for cross-thread coordination between the xmtr pump
    // thread (calls xilv) and operator setters (SetILVRun /
    // pSetILVRun toggle bit 0).
    std::atomic<std::uint32_t> run{0};

    // Reference ilv.h:33 — id to use in the Outbound() call.
    int obid = 0;

    // Reference ilv.h:34 — number of complex samples in EACH input
    // buffer (the j-loop count in xilv at ilv.c:64).
    int insize = 0;

    // Reference ilv.h:35 — maximum number of inputs (sizes the
    // outbuff allocation: nin * insize * sizeof(complex)).
    int nin = 0;

    // Reference ilv.h:36 — bit mask of inputs that should be
    // interleaved (bit i = input i is in the mix).  Cross-thread:
    // xilv reads vs SetILVWhat operator writes.
    std::atomic<std::uint32_t> what{0};

    // Reference ilv.h:37 — `double* outbuff`.  RAII via std::vector;
    // sized 2 * nin * insize doubles (the complex tuple count
    // matches the reference malloc0 expression at ilv.c:47).
    std::vector<double> outbuff;

    // Reference ilv.h:38 — operator-supplied output dispatcher.
    // std::function per the docs/RULES.md §5.8 idiom.  Signature
    // matches CMaster Stage A OutboundCallback exactly so
    // SetILVOutputPointer can accept the same callable type the
    // SendpOutboundTx setter stores.
    std::function<void(int id, int nsamples, double* buff)> Outbound;

    // [Lyra-native] Mutex protecting the Outbound swap.  Reference
    // does an unguarded function-pointer write at ilv.c:100 +
    // unguarded function-pointer read at ilv.c:88 — Windows /
    // x86_64 word-sized pointer writes happen to be atomic, but
    // std::function is NOT a word-sized pointer (it carries small
    // buffer storage + a vtable pointer + a heap allocation).  A
    // racy std::function read during assignment is UB.  This mutex
    // is held for the brief swap in SetILVOutputPointer and for the
    // (*Outbound)(...) dispatch inside xilv — keeps the operator-
    // setter / pump-thread race safe.  Same pattern AAMix uses for
    // the Outbound field (single mutex, micro-contention).
    std::mutex outbound_mutex;
};

// [Lyra-native] Process-wide central bank.  See preamble for the
// pcm->xmtr[id].pilv ↔ pilv[id] sidestep rationale.  Populated by
// create_ilv (when id >= 0), looked up by SetILV* setters via
// resolve_ilv(xmtr_id).  Defined in ILV.cpp.
extern std::array<ILV*, MAX_EXT_ILV> pilv;

// ===== Public API (matches ilv.h:41-58 byte-for-byte where the
// reference API is `xmtr_id`-keyed; Lyra-cpp adds the id parameter
// to create_ilv to publish into pilv[id] mirror of paamix[]) =====

// Reference ilv.h:41-48 — `create_ilv(...)` constructor.
// Allocates an ILV, sizes outbuff, stores all configuration, returns
// the new ILV.
//
// **[Lyra-native]** extra leading param `xmtr_id` — when non-negative,
// publishes the returned pointer into pilv[xmtr_id] so the SetILV*
// setters can resolve it.  Pass -1 to construct an unmanaged ILV
// (caller owns the pointer; only the pSetILV* direct-pointer
// setters work).  Single-param divergence from reference; matches
// the analogous AAMix `create_aamix(id, ...)` pattern.
ILV* create_ilv(
    int xmtr_id,
    int run,
    int outbound_id,    // id to use in the outbound call
    int insize,         // number of complex samples in EACH input buffer
    int ninputs,        // maximum number of inputs
    long what,          // bits specify which inputs are to be interleaved
    std::function<void(int id, int nsamples, double* buff)> Outbound);

// Reference ilv.h:50 — `destroy_ilv(ILV a)`.  Frees outbuff (RAII)
// and the ILV itself.  Lyra-cpp also clears the central bank slot
// if `xmtr_id >= 0`; pass -1 to skip the bank clear (for unmanaged
// ILVs constructed with create_ilv(xmtr_id=-1, ...)).
void destroy_ilv(ILV* a, int xmtr_id);

// Reference ilv.h:52 — `xilv(ILV a, double** data)`.  Synchronous
// interleaver pump body.  When `run` bit 0 is set, interleaves the
// inputs indicated by `what` sample-by-sample into outbuff (the j/k
// double-loop at ilv.c:62-80).  When `run` is off, memcpy's
// input[0] directly into outbuff (the bypass at ilv.c:82-87).
// Calls (*Outbound)(obid, k, outbuff) on completion (ilv.c:88).
void xilv(ILV* a, double** data);

// Reference ilv.h:54 — `SetILVOutputPointer(int xmtr_id, void(*Outbound)(...))`.
// Replaces the Outbound dispatcher on the ILV resolved via pilv[xmtr_id].
void SetILVOutputPointer(
    int xmtr_id,
    std::function<void(int id, int nsamples, double* buff)> Outbound);

// Reference ilv.c:104-111 — `PORT void SetILVRun(int xmtr_id, int run)`.
// Sets / resets bit 0 of `run` on the ILV resolved via pilv[xmtr_id].
void SetILVRun(int xmtr_id, int run);

// Reference ilv.c:114-121 — `PORT void SetILVWhat(int xmtr_id, int stream, int state)`.
// Sets / resets bit `stream` of `what` on the ILV resolved via pilv[xmtr_id].
void SetILVWhat(int xmtr_id, int stream, int state);

// Reference ilv.c:124-128 — `PORT void SetILVInsize(int xmtr_id, int size)`.
// Updates `insize` on the ILV resolved via pilv[xmtr_id].  Plain int
// store (reference is also unguarded — j-loop reads stale within one
// pump iteration is acceptable; the reference comment in xilv hints
// at this single-pass tolerance).
void SetILVInsize(int xmtr_id, int size);

// Reference ilv.c:131-135 — `PORT void SetILVOutboundId(int xmtr_id, int obid)`.
// Updates `obid` on the ILV resolved via pilv[xmtr_id].
void SetILVOutboundId(int xmtr_id, int obid);

// Reference ilv.c:137-143 — `void pSetILVRun(ILV a, int run)`.
// Same as SetILVRun but takes the ILV* directly (skipping the
// pilv[] lookup).  Used by callers that hold the ILV pointer from
// create_ilv directly.
void pSetILVRun(ILV* a, int run);

// Reference ilv.c:145-148 — `void pSetILVInsize(ILV a, int size)`.
// Same as SetILVInsize but takes the ILV* directly.
void pSetILVInsize(ILV* a, int size);

} // namespace lyra::wdsp

// =========================================================================
// Contract notes — reference ilv.c:29 verbatim ("ilv is a simple
// SYNCHRONOUS interleaver: it assumes all inputs are available when it
// is called").
// =========================================================================
//
// Threading: xilv is called by the xmtr pump thread (Stage D xcmaster).
// SetILV* setters may be called from operator threads.  All cross-thread
// shared state uses std::atomic (run, what) or the outbound_mutex
// (Outbound std::function swap).  insize/obid/nin are plain int stores
// per reference (single-pass tolerance — a torn read produces one
// off-by-N pump iteration, then converges).
//
// Lifecycle: create_ilv allocates outbuff sized for the maximum
// ninputs * insize complex tuples.  insize / obid / what can be changed
// at runtime via setters; nin is fixed at creation time (resizing
// outbuff at runtime would race xilv).  destroy_ilv frees everything.
//
// Bypass mode: when run bit 0 is OFF, xilv treats input[0] as already-
// formed output and memcpy's directly into outbuff (no interleave).
// This matches the reference fast-path at ilv.c:82-87 for single-stream
// configurations.
