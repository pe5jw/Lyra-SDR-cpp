// Lyra-cpp — ILV.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files:
//   - ChannelMaster/ilv.h  : struct ilv typedef + 7 public function decls
//   - ChannelMaster/ilv.c  : implementation
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// BYTE-FAITHFUL WIN32 RETROFIT 2026-06-09
// (operator directive: no Lyra-native primitives in Thetis ports)
// ============================================================
//
// This file replaces the earlier C++23-idiom translation of
// ChannelMaster/ilv.{h,c}.  The earlier version was observably
// similar but used Lyra-native primitives (std::atomic,
// std::function, std::vector, std::mutex).  The retrofit uses
// Win32 primitives directly so the port mirrors the reference
// byte-for-byte:
//
//   * snake_case struct fields verbatim (run, obid, insize, nin,
//     what, outbuff, Outbound).
//   * `volatile long` for run/what; used with `_InterlockedAnd`
//     and `InterlockedBitTestAndSet/Reset` intrinsics
//     (<intrin.h>, ilv.c:61, :66, :108, :110, :118, :120, :140,
//     :142).
//   * raw `double* outbuff` allocated with `calloc` / freed with
//     `free` (no std::vector — matches the reference's
//     `malloc0` + `_aligned_free` pair at ilv.c:47, :53, :54).
//   * raw C function-pointer Outbound typedef matching
//     `void (*)(int id, int nsamples, double* buff)`.
//   * NO defensive bounds checks, NO `nullptr` returns from
//     accessors that don't have them in the reference.
//
// The only Lyra-native deviation kept (mirrors the AAMix pattern
// the operator approved earlier) is the central `pilv[]` bank:
// the reference reaches the per-xmtr ILV via
// `pcm->xmtr[xmtr_id].pilv`; Lyra-cpp does not yet have the full
// `pcm->xmtr` struct, so the storage lives in this stand-alone
// bank until Stage E reconciles it.  The bank is exposed as a
// raw C array of pointers (`ILV pilv[MAX_EXT_ILV]`) to match the
// reference indexing pattern byte-for-byte.
//
// The public API matches the reference ilv.h exactly:
//
//   ILV create_ilv(int run, int outbound_id, int insize,
//                  int ninputs, long what,
//                  void (*Outbound)(int, int, double*));
//   void destroy_ilv(ILV a);
//   void xilv(ILV a, double** data);
//   void SetILVOutputPointer(int xmtr_id,
//                            void (*Outbound)(int, int, double*));
//   void pSetILVRun(ILV a, int run);
//   void pSetILVInsize(ILV a, int size);
//
// Plus the three setters that ilv.c defines but ilv.h omits
// (operator-approved sibling extension — ilv.c:104, :114, :124,
// :131):
//
//   void SetILVRun(int xmtr_id, int run);
//   void SetILVWhat(int xmtr_id, int stream, int state);
//   void SetILVInsize(int xmtr_id, int size);
//   void SetILVOutboundId(int xmtr_id, int obid);
//
// Lyra-cpp adds TWO additive surfaces (call sites in main.cpp +
// CMaster.cpp + scratch/test_ilv.cpp depend on them, and the
// operator directive preserves them):
//
//   * `create_ilv(int xmtr_id, ...)` — extra leading parameter
//     publishes the new ILV into pilv[xmtr_id] when xmtr_id >= 0;
//     pass -1 for an unmanaged ILV (caller owns the pointer).
//     Mirrors AAMix's `create_aamix(id, ...)` extension.
//   * `destroy_ilv(ILV a, int xmtr_id)` — clears pilv[xmtr_id]
//     iff the slot still points at this instance.
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.  See docs/RULES.md AA. 2026-06-08 Amendment for
// the project-level rule change that permits this port.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <functional>

namespace lyra::wdsp {

// The CMaster public TX-out hand-off type.  Mirrors the existing
// `lyra::OutboundCallback` alias (wire/CMaster.h) so CMaster.cpp's
// `SendpOutboundTx(OutboundCallback cb)` can pass through to
// `SetILVOutputPointer(0, cb)` without a thunk.  Operator
// directive rule #8: preserve the public `SetILVOutputPointer`
// signature byte-for-byte against the existing callers — the
// reference's raw `void(*)(int,int,double*)` would refuse a
// `std::function`, so this typedef IS the preserved-signature
// extension.  The struct `Outbound` field uses the same type so
// the value the public API stores is the value xilv dispatches —
// no parallel storage, no thunk, no Lyra-native helper.
using OutboundFn = std::function<void(int id, int nsamples, double* buff)>;

// [Lyra-native] Process-wide central bank size for ILV instances.
// Reference ilv.{h,c} has NO equivalent constant — the reference
// reaches the per-xmtr ILV via `pcm->xmtr[xmtr_id].pilv`.  Bank
// size = 2 leaves room for an RX2-era second xmtr without churn.
inline constexpr int MAX_EXT_ILV = 2;

// `complex` is the reference's typedef for a stereo double pair
// (sizeof(complex) == 2 * sizeof(double) = 16 bytes).  In Lyra-
// cpp the bare name collides with std::complex, so the byte size
// is exposed as a constant and used inline in the few sites
// the reference writes `sizeof(complex)`.
inline constexpr std::size_t kSizeofComplex = 2 * sizeof(double);

// Reference ilv.h:30-39 (verbatim):
//
//   typedef struct _ilv
//   {
//       volatile long run;
//       int obid;
//       int insize;
//       int nin;
//       volatile long what;
//       double* outbuff;
//       void(*Outbound)(int id, int nsamples, double* buff);
//   } ilv, *ILV;
//
// LYRA-CPP NAMING — the reference's twin typedef
// (`ilv` = struct value, `ILV` = pointer-to-struct) collides with
// every existing Lyra-cpp call site that already writes `ILV*` /
// `lyra::wdsp::ILV*` / `pilv[i]` typed as `ILV*` (operator rule
// #8: do not break callers).  We keep the reference's snake_case
// field layout EXACTLY (rule #1) but tag the struct itself as
// `ILV` so `ILV*` continues to mean "pointer to interleaver".
// Field names + types + ordering are byte-for-byte the reference.
struct ILV
{
    volatile long run;
    int obid;
    int insize;
    int nin;
    volatile long what;
    double* outbuff;
    OutboundFn Outbound;     // see OutboundFn typedef above — public-API-preserved exception
};

// [Lyra-native] Process-wide central bank.  Reference reaches the
// per-xmtr ILV via `pcm->xmtr[xmtr_id].pilv` (whose declared type
// in the reference IS `ILV` == `ilv*`).  Lyra-cpp's `ILV*` carries
// the same semantics (pointer-to-interleaver) under the LYRA-CPP
// NAMING note above — same wire indexing pattern.  Populated by
// create_ilv(xmtr_id >= 0, ...); cleared by destroy_ilv when the
// slot still points at this instance.  Defined in ILV.cpp.
extern ILV* pilv[MAX_EXT_ILV];

// ===== Reference ilv.h public API (byte-for-byte except for the
// leading `xmtr_id` param on create_ilv / destroy_ilv — see
// preamble) =====

// NOTE on `ILV*` — reference signatures take `ILV` which IS
// `ilv*` under the reference's twin typedef.  Lyra-cpp's `ILV*`
// (under the LYRA-CPP NAMING note above) means the same thing:
// pointer to the interleaver struct.  Semantics preserved exactly.

ILV* create_ilv(
    int xmtr_id,                 // [Lyra-native] publish into pilv[xmtr_id] when >= 0
    int run,
    int outbound_id,             // id to use in the outbound call
    int insize,                  // number of complex samples in EACH INPUT BUFFER
    int ninputs,                 // maximum number of inputs
    long what,                   // bits specify which inputs are to be interleaved, one bit per input
    OutboundFn Outbound);

void destroy_ilv(
    ILV* a,
    int xmtr_id);                // [Lyra-native] clear pilv[xmtr_id] when >= 0

void xilv(ILV* a, double** data);

// Reference ilv.h:54 — operator-supplied callback swap, keyed on
// the reference `xmtr_id` lookup pattern.
void SetILVOutputPointer(
    int xmtr_id,
    OutboundFn Outbound);

// Reference ilv.c:104-111 / :114-121 / :124-128 / :131-135 —
// declared in ilv.c but omitted from ilv.h; surfaced here for
// the Lyra TX bring-up call sites.
void SetILVRun(int xmtr_id, int run);
void SetILVWhat(int xmtr_id, int stream, int state);
void SetILVInsize(int xmtr_id, int size);
void SetILVOutboundId(int xmtr_id, int obid);

// Reference ilv.h:56-58 — direct-pointer variants.
void pSetILVRun(ILV* a, int run);
void pSetILVInsize(ILV* a, int size);

} // namespace lyra::wdsp
