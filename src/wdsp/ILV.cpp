// Lyra-cpp — ILV.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream:    https://github.com/mi0bot/OpenHPSDR-Thetis
// Source file: ChannelMaster/ilv.c
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
//
// See ILV.h for the full per-file GPL attribution + the
// C → C++23 idiom-translation log + the multi-stage port plan
// + the public-API contract notes ported verbatim from ilv.c:29.
//
// ---------------------------------------------------------------
//
// **Stage C.2 (THIS COMMIT) — pilv[] central bank + resolve_ilv
// helper + 7 setters (SetILVOutputPointer + SetILV{Run, What,
// Insize, OutboundId} + pSetILV{Run, Insize}).**  Wires the
// reference `pcm->xmtr[xmtr_id].pilv` lookup pattern through the
// Lyra-native pilv[] bank (mirror of paamix[]).  Activates the
// `xmtr_id`-bank publication in create_ilv + the bank-slot clear
// in destroy_ilv that C.1 left as forward-compatible stubs.
// Companion scratch/test_ilv.cpp asserts bit-exact interleave
// output via a synthetic 2-stream / 4-stream check.
//
// Still wire-inert: no Lyra consumer constructs an ILV yet.  The
// cmaster TX pump that drives xilv lands in Stage D xcmaster
// port; the CMaster::SendpOutboundTx hand-off lands in Stage C.3.
//
// **Already shipped in Stage C.1**: create_ilv (bank stub),
// destroy_ilv (bank stub), xilv (full body).
//
// **Deliberately deferred to Stage C.3**:
//   - SetILVOutputPointer(0, pcm->OutboundTx) wire-up into the
//     Stage A CMaster::SendpOutboundTx stub at CMaster.cpp:82-86
//     (line 85 `// Stage C PENDING:` comment).  Stage C.3 mirrors
//     Stage B.6.b discipline: audit for any Stage-A no-op stub
//     that could clobber ILV->Outbound (the silent-audio defect
//     root cause -- do NOT re-detonate).

#include "wdsp/ILV.h"

#include <cstring>

namespace lyra::wdsp {

// ===== pilv[] central bank + resolve_ilv helper =====
//
// [Lyra-native] Mirrors AAMix's paamix[] pattern.  The reference
// reaches the per-xmtr ILV via `pcm->xmtr[xmtr_id].pilv`; until
// Stage E reconciles the full pcm->xmtr struct, Lyra-cpp keeps
// the storage in this stand-alone bank so the SetILV* setter
// signatures port byte-for-byte.  Process-lifetime; populated by
// create_ilv(xmtr_id >= 0, ...); cleared by destroy_ilv when the
// pointer matches.

std::array<ILV*, MAX_EXT_ILV> pilv{};

namespace {
// Resolve an xmtr_id into the bank slot.  Returns nullptr when the
// id is out of range OR the slot is empty -- setters early-return
// in either case (matches AAMix's resolve_aamix discipline of
// safely no-op'ing rather than crashing on a not-yet-created
// mixer; the reference would dereference a null pcm->xmtr[].pilv
// and crash, but Lyra-cpp's pre-Stage-D pump-construction window
// makes the safer no-op the correct posture for now).
ILV* resolve_ilv(int xmtr_id) noexcept
{
    if (xmtr_id < 0 || xmtr_id >= MAX_EXT_ILV) {
        return nullptr;
    }
    return pilv[xmtr_id];
}
} // namespace

// ===== create_ilv =====
//
// Reference ilv.c:31-49:
//
//   ILV create_ilv (
//       int run, int outbound_id, int insize, int ninputs,
//       long what,
//       void (*Outbound) (int id, int nsamples, double* buff))
//   {
//       ILV a = (ILV) malloc0 (sizeof (ilv));
//       a->run        = run;
//       a->obid       = outbound_id;
//       a->insize     = insize;
//       a->nin        = ninputs;
//       a->what       = what;
//       a->Outbound   = Outbound;
//       a->outbuff = (double *) malloc0 (a->nin * a->insize * sizeof(complex));
//       return a;
//   }
//
// Lyra-cpp translation notes:
//   - `malloc0(sizeof(ilv))` -> `new ILV{}` (in-class initializers
//     zero everything; the explicit field stores below match the
//     reference one-to-one).
//   - `malloc0(nin * insize * sizeof(complex))` -> outbuff.assign(
//     2 * nin * insize, 0.0).  sizeof(complex) in the reference is
//     `sizeof(double[2])` = 16 bytes; dividing by sizeof(double) =
//     8 gives the 2x multiplier for the .assign() count.
//   - The `int run` / `long what` parameters are stored into the
//     `std::atomic<std::uint32_t>` fields via direct widening
//     assignment (reference truncates / promotes implicitly; we
//     keep the same observable contract).
//   - **[Lyra-native]** extra leading `xmtr_id` parameter -- when
//     >= 0, publishes the new ILV into pilv[xmtr_id] so the
//     SetILV* setters can resolve it.  The bank slot definition +
//     the resolve_ilv helper land in Stage C.2; until then this
//     parameter is accepted-and-stored-but-bank-publication is a
//     no-op (the pilv[] array isn't defined yet).  Stage C.2's
//     wire is: `if (xmtr_id >= 0 && xmtr_id < MAX_EXT_ILV)
//     pilv[xmtr_id] = a;`.

ILV* create_ilv(
    int xmtr_id,
    int run,
    int outbound_id,
    int insize,
    int ninputs,
    long what,
    std::function<void(int id, int nsamples, double* buff)> Outbound)
{
    auto* a = new ILV{};
    a->run.store(static_cast<std::uint32_t>(run), std::memory_order_release);
    a->obid     = outbound_id;
    a->insize   = insize;
    a->nin      = ninputs;
    a->what.store(static_cast<std::uint32_t>(what), std::memory_order_release);
    a->Outbound = std::move(Outbound);
    // Reference: `malloc0(nin * insize * sizeof(complex))` -- complex
    // is two doubles, so the byte count is `2 * nin * insize *
    // sizeof(double)`.  Convert to a double count: `2 * nin * insize`.
    a->outbuff.assign(
        static_cast<std::size_t>(2) *
            static_cast<std::size_t>(ninputs) *
            static_cast<std::size_t>(insize),
        0.0);
    // Stage C.2: publish into the central bank when the caller
    // supplied a non-negative xmtr_id (matches AAMix's
    // create_aamix(id >= 0, ...) -> paamix[id] = a pattern).  An
    // out-of-range id is silently ignored; the returned pointer
    // is still valid for the unmanaged direct-pointer use case.
    if (xmtr_id >= 0 && xmtr_id < MAX_EXT_ILV) {
        pilv[xmtr_id] = a;
    }
    return a;
}

// ===== destroy_ilv =====
//
// Reference ilv.c:51-55:
//
//   void destroy_ilv (ILV a)
//   {
//       _aligned_free (a->outbuff);
//       _aligned_free (a);
//   }
//
// Lyra-cpp translation:
//   - outbuff is a std::vector -- freed automatically by ~ILV.
//   - `_aligned_free(a)` -> `delete a`.
//   - **[Lyra-native]** extra `xmtr_id` parameter -- when >= 0,
//     clears the pilv[xmtr_id] bank slot.  Bank slot definition
//     lands in Stage C.2; until then this is a no-op so the
//     signature is forward-compatible.

void destroy_ilv(ILV* a, int xmtr_id)
{
    if (a == nullptr) {
        return;
    }
    // Stage C.2: clear the central bank slot iff the bank still
    // points at this instance (defensive against the race where a
    // newer create_ilv(xmtr_id, ...) has already replaced the
    // slot before this destroy_ilv ran).
    if (xmtr_id >= 0 && xmtr_id < MAX_EXT_ILV && pilv[xmtr_id] == a) {
        pilv[xmtr_id] = nullptr;
    }
    delete a;
}

// ===== xilv =====
//
// Reference ilv.c:57-89:
//
//   void xilv (ILV a, double** data)
//   {
//       int i, j, k;
//       int what, mask;
//       if (_InterlockedAnd(&a->run, 1))
//       {
//           k = 0;
//           for (j = 0; j < a->insize; j++)
//           {
//               what = _InterlockedAnd(&a->what, 0xffffffff);
//               i = 0;
//               while (what != 0)
//               {
//                   mask = 1 << i;
//                   if ((mask & what) != 0)
//                   {
//                       a->outbuff[2 * k + 0] = data[i][2 * j + 0];
//                       a->outbuff[2 * k + 1] = data[i][2 * j + 1];
//                       what &= ~mask;
//                       k++;
//                   }
//                   i++;
//               }
//           }
//       }
//       else
//       {
//           k = a->insize;
//           if (a->outbuff != data[0])
//               memcpy(a->outbuff, data[0], a->insize * sizeof(complex));
//       }
//       (*a->Outbound)(a->obid, k, a->outbuff);
//   }
//
// Lyra-cpp translation:
//   - `_InterlockedAnd(&a->run, 1)`: the reference clears all bits
//     except bit 0 atomically and returns the old value.  Since the
//     reference setters only ever set/reset bit 0 (ilv.c:108-110,
//     :140-142), the "clear all but bit 0" effect is a no-op; the
//     intent is a fenced read of bit 0.  C++23 equivalent:
//     `(a->run.load(std::memory_order_acquire) & 1u) != 0`.
//   - `_InterlockedAnd(&a->what, 0xffffffff)`: AND with 0xffffffff
//     is a no-op on a uint32_t -- pure fenced read of all 32 bits.
//     C++23: `a->what.load(std::memory_order_acquire)`.
//   - The j/k double-loop body ports character-for-character: the
//     enabled-input scan reads each bit of `what` from LSB up,
//     copying the corresponding stereo sample pair into outbuff,
//     and increments k once per copied tuple.
//   - The else-branch fast path ports verbatim, modulo
//     `a->outbuff != data[0]` (reference pointer compare): in
//     Lyra-cpp the comparison is `a->outbuff.data() != data[0]`.
//   - Outbound dispatch: the reference does an unguarded
//     function-pointer call `(*a->Outbound)(...)`.  Lyra-cpp's
//     Outbound is std::function (NOT a word-sized pointer -- has
//     SBO storage + vtable + heap allocation) so a concurrent
//     SetILVOutputPointer would race the dispatch.  Translation:
//     copy the std::function under outbound_mutex, release, then
//     invoke the copy.  Same pattern AAMix uses for its identical
//     Outbound field (see AAMix.cpp's xaamix dispatch -- single
//     mutex, micro-contention because the swap is rare).

void xilv(ILV* a, double** data)
{
    int k = 0;
    if ((a->run.load(std::memory_order_acquire) & 1u) != 0u) {
        // Interleave-enabled pump path (reference ilv.c:62-80).
        const int insize = a->insize;  // Snapshot for the j-loop;
                                       // reference reads `a->insize`
                                       // on every iteration but the
                                       // semantics are identical
                                       // (insize doesn't change
                                       // mid-pump in practice).
        for (int j = 0; j < insize; ++j) {
            // Per-sample re-read of `what` matches reference
            // ilv.c:66 -- an operator setter can flip a stream
            // bit mid-block, and the next sample picks it up.
            std::uint32_t what = a->what.load(std::memory_order_acquire);
            int i = 0;
            while (what != 0u) {
                const std::uint32_t mask = 1u << i;
                if ((mask & what) != 0u) {
                    a->outbuff[2 * static_cast<std::size_t>(k) + 0] =
                        data[i][2 * j + 0];
                    a->outbuff[2 * static_cast<std::size_t>(k) + 1] =
                        data[i][2 * j + 1];
                    what &= ~mask;
                    ++k;
                }
                ++i;
            }
        }
    } else {
        // Bypass fast path (reference ilv.c:82-87).  Treats
        // data[0] as already-formed output and memcpy's directly.
        k = a->insize;
        if (a->outbuff.data() != data[0]) {
            std::memcpy(
                a->outbuff.data(),
                data[0],
                static_cast<std::size_t>(a->insize) * 2u * sizeof(double));
        }
    }

    // Outbound dispatch -- copy under mutex, release, invoke copy.
    // See translation note above for the std::function race
    // rationale.  Empty Outbound (caller never registered one) is
    // treated as a no-op rather than UB; the reference would
    // crash on a null function pointer, but Lyra-cpp's safer
    // default better matches Stage C.0/C.1's wire-inert posture.
    std::function<void(int, int, double*)> cb;
    {
        std::scoped_lock lock(a->outbound_mutex);
        cb = a->Outbound;
    }
    if (cb) {
        cb(a->obid, k, a->outbuff.data());
    }
}

// ===== SetILVOutputPointer =====
//
// Reference ilv.c:97-101:
//
//   void SetILVOutputPointer (int xmtr_id, void(*Outbound)(...))
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       a->Outbound = Outbound;
//   }
//
// Lyra-cpp translation: the reference does an unguarded function-
// pointer assignment, relying on word-sized pointer-store atomicity
// for the cross-thread race against xilv's dispatch.  Lyra-cpp's
// std::function is NOT a word-sized pointer (it carries SBO storage
// + a vtable pointer + a heap allocation), so the swap must hold
// outbound_mutex.  Matching AAMix's SetAAudioMixOutputPointer
// pattern verbatim.

void SetILVOutputPointer(
    int xmtr_id,
    std::function<void(int id, int nsamples, double* buff)> Outbound)
{
    ILV* a = resolve_ilv(xmtr_id);
    if (a == nullptr) {
        return;
    }
    std::scoped_lock lock(a->outbound_mutex);
    a->Outbound = std::move(Outbound);
}

// ===== SetILVRun =====
//
// Reference ilv.c:104-111:
//
//   PORT void SetILVRun (int xmtr_id, int run)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       if (run)
//           InterlockedBitTestAndSet(&a->run, 0);
//       else
//           InterlockedBitTestAndReset(&a->run, 0);
//   }
//
// Lyra-cpp translation: `InterlockedBitTestAndSet(&run, 0)` →
// `run.fetch_or(1u << 0, memory_order_acq_rel)`;
// `InterlockedBitTestAndReset(&run, 0)` →
// `run.fetch_and(~(1u << 0), memory_order_acq_rel)`.  The reference
// `InterlockedBitTest*` ops return the prior bit value; Lyra-cpp's
// fetch_or/fetch_and return the prior word value -- callers (none
// in the reference path) that wanted the old bit would extract it
// via `(prior >> bit) & 1u`.  No caller currently uses the return.

void SetILVRun(int xmtr_id, int run)
{
    ILV* a = resolve_ilv(xmtr_id);
    if (a == nullptr) {
        return;
    }
    if (run) {
        a->run.fetch_or(1u << 0, std::memory_order_acq_rel);
    } else {
        a->run.fetch_and(~(1u << 0), std::memory_order_acq_rel);
    }
}

// ===== SetILVWhat =====
//
// Reference ilv.c:114-121:
//
//   PORT void SetILVWhat(int xmtr_id, int stream, int state)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       if (state)
//           InterlockedBitTestAndSet(&a->what, stream);
//       else
//           InterlockedBitTestAndReset(&a->what, stream);
//   }
//
// Lyra-cpp translation: same fetch_or/fetch_and pattern as
// SetILVRun, parameterized on `stream` rather than fixed bit 0.
// The reference accepts `stream` in [0, 31] implicitly; Lyra-cpp
// silently clamps via the bit-shift type (out-of-range shifts on
// uint32_t are UB in C++23, so we guard the precondition).

void SetILVWhat(int xmtr_id, int stream, int state)
{
    ILV* a = resolve_ilv(xmtr_id);
    if (a == nullptr) {
        return;
    }
    if (stream < 0 || stream >= 32) {
        return;  // Out-of-range bit index — defensive no-op.
    }
    const std::uint32_t mask = 1u << static_cast<unsigned>(stream);
    if (state) {
        a->what.fetch_or(mask, std::memory_order_acq_rel);
    } else {
        a->what.fetch_and(~mask, std::memory_order_acq_rel);
    }
}

// ===== SetILVInsize =====
//
// Reference ilv.c:124-128:
//
//   PORT void SetILVInsize(int xmtr_id, int size)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       a->insize = size;
//   }
//
// Plain int store -- reference is unguarded.  Lyra-cpp matches:
// `insize` is read once at the top of xilv's j-loop (snapshot
// pattern), so a torn read produces one off-by-N pump iteration
// then converges.  Operator setting insize is rare (radio-create
// time per the reference's xilv comment in ilv.c:29).

void SetILVInsize(int xmtr_id, int size)
{
    ILV* a = resolve_ilv(xmtr_id);
    if (a == nullptr) {
        return;
    }
    a->insize = size;
}

// ===== SetILVOutboundId =====
//
// Reference ilv.c:131-135:
//
//   PORT void SetILVOutboundId(int xmtr_id, int obid)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       a->obid = obid;
//   }
//
// Plain int store, same unguarded semantics as SetILVInsize.

void SetILVOutboundId(int xmtr_id, int obid)
{
    ILV* a = resolve_ilv(xmtr_id);
    if (a == nullptr) {
        return;
    }
    a->obid = obid;
}

// ===== pSetILVRun =====
//
// Reference ilv.c:137-143:
//
//   void pSetILVRun(ILV a, int run)
//   {
//       if (run)
//           InterlockedBitTestAndSet(&a->run, 0);
//       else
//           InterlockedBitTestAndReset(&a->run, 0);
//   }
//
// Direct-pointer variant -- same body as SetILVRun minus the
// bank lookup.  Used by callers that hold the create_ilv return
// value (typically unmanaged ILVs created with xmtr_id = -1).

void pSetILVRun(ILV* a, int run)
{
    if (a == nullptr) {
        return;
    }
    if (run) {
        a->run.fetch_or(1u << 0, std::memory_order_acq_rel);
    } else {
        a->run.fetch_and(~(1u << 0), std::memory_order_acq_rel);
    }
}

// ===== pSetILVInsize =====
//
// Reference ilv.c:145-148:
//
//   void pSetILVInsize(ILV a, int size)
//   {
//       a->insize = size;
//   }
//
// Direct-pointer variant of SetILVInsize.

void pSetILVInsize(ILV* a, int size)
{
    if (a == nullptr) {
        return;
    }
    a->insize = size;
}

} // namespace lyra::wdsp
