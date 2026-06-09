// Lyra-cpp — CmBuffs.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Source files:
//   - ChannelMaster/cmbuffs.h: struct cmb + 7 public decls
//   - ChannelMaster/cmbuffs.c: implementations
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// BYTE-FAITHFUL WIN32 RETROFIT + LYRA-NATIVE create_xmtr_hl2
// SPLIT REVERTED 2026-06-09
// (operator directive: no Lyra-native deviations in Thetis ports)
// ============================================================
//
// This file replaces the earlier C++23-idiom translation of
// ChannelMaster/cmbuffs.{h,c}.  The earlier version was observably
// similar but used Lyra-native primitives (std::atomic,
// std::counting_semaphore, std::mutex, std::vector, std::thread,
// per-stream g_cmbuffs unique_ptr bank).  The retrofit uses Win32
// primitives directly so the port mirrors the reference
// byte-for-byte:
//
//   * snake_case struct fields verbatim (id, max_in_size,
//     max_outsize, r1_outsize, r1_size, r1_active_buffsize,
//     r1_baseptr, r1_inidx, r1_outidx, r1_unqueuedsamps, run,
//     accept, Sem_BuffReady, csOUT, csIN).
//   * `volatile long` for run/accept used with `_InterlockedAnd`
//     + `InterlockedBitTestAndSet/Reset` intrinsics (<intrin.h>).
//   * `HANDLE Sem_BuffReady` via `CreateSemaphore(0, 0, 1000, 0)`
//     + `WaitForSingleObject(h, INFINITE)` / `ReleaseSemaphore(h,
//     n, 0)` / `CloseHandle(h)`.
//   * `CRITICAL_SECTION csIN / csOUT` via
//     `InitializeCriticalSectionAndSpinCount(&cs, 2500)` -- the
//     2500 spin count preserved verbatim.
//   * raw `double* r1_baseptr` allocated with `calloc(N,
//     kSizeofComplex)` + freed with `free()` (mirrors the
//     reference's malloc0 + _aligned_free pair).
//   * `_beginthread(cm_main, 0, (void*)(intptr_t)id)` from
//     <process.h>; `_endthread()` for explicit exit.
//   * `AvSetMmThreadCharacteristics("Pro Audio", &taskIndex)`
//     from <avrt.h> in cm_main.
//   * NO defensive bounds checks, NO `nullptr` returns from
//     accessors that don't have them in the reference.
//
// `complex` is the reference's typedef for a stereo double pair
// (sizeof(complex) == 2 * sizeof(double) = 16 bytes).  In Lyra-cpp
// the bare name collides with std::complex, so the byte size is
// exposed via a TU-local constant in CmBuffs.cpp (mirrors AAMix.cpp
// pattern -- sibling ILV.h publishes `kSizeofComplex` in
// lyra::wdsp at namespace scope but cmbuffs lives in lyra::wire so
// the alias is file-local).
//
// ALLOWED FORCED DEVIATIONS (rule-#8 caller-protection only):
//
//   * Struct tag stays as `CmBuffs` (Lyra-cpp PascalCase) instead
//     of reference's `cmb` (typedef `cmb, *CMB`).  Required
//     because external callers declare `lyra::wire::CmBuffs*`
//     (the CMaster.h xmtr substruct's CMB aliases) and would need
//     their own retrofit to flip.  Rule #8 caller-protection.
//
// See NOTICE.md and CREDITS.md (repo root) for full attribution.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace lyra::wire {

// Reference cmbuffs.h:31 -- `#define CMB_MULT (3)`.
// Ring size = CMB_MULT * max(max_in_size, max_outsize) complex
// samples -- enough cushion for ~3 output blocks of producer/
// consumer cadence mismatch without dropping.
inline constexpr int CMB_MULT = 3;

// ---------------------------------------------------------------
// struct CmBuffs -- direct port of cmbuffs.h:32-51 `struct cmb`.
// Field layout + names + types byte-for-byte the reference (only
// the struct tag is `CmBuffs` per the rule-#8 carve-out above).
// ---------------------------------------------------------------
struct CmBuffs {
    int   id;
    int   max_in_size;            // max input number of complex samples
    int   max_outsize;            // max output number of complex samples
    int   r1_outsize;             // complex samples taken out per fexchange

    int   r1_size;                // size of a single maximum-sized transfer
    int   r1_active_buffsize;     // size of ring (in complex samples)

    double* r1_baseptr;           // pointer to ring
    int   r1_inidx;               // in 'double', actual byte off = 2 * this
    int   r1_outidx;              // in 'double', actual byte off = 2 * this
    int   r1_unqueuedsamps;       // input samples not yet queued/released

    volatile long run;            // 1 = thread loops; 0 = thread terminates
    volatile long accept;         // 1 = Inbound() accepts input
    HANDLE Sem_BuffReady;         // count = output-sized buffers queued
    CRITICAL_SECTION csOUT;       // blocks cm_main from cmdata during update
    CRITICAL_SECTION csIN;        // blocks Inbound during update/flush
};

// ---------------------------------------------------------------
// Public API -- reference cmbuffs.h:53-65 surface preserved
// byte-for-byte.  Signatures match the reference exactly; the
// only deviation is `cm_main` taking `void*` per the reference
// `_beginthread` callback contract (the reference also takes
// void* -- cmbuffs.c:151).
// ---------------------------------------------------------------

// Reference cmbuffs.h:53.  Allocates the per-stream cmb, sizes
// the ring (CMB_MULT * max(in,out) complex), creates the
// semaphore + critical sections, starts cm_main.  MUST be called
// once per stream at create_cmaster time, BEFORE any Inbound().
//
// Also publishes the new CMB pointer into the four
// pcm->{pc,pd,pe,pf}buff[id] aliases per cmbuffs.c:38 (the
// reference does `pcm->pcbuff[id] = pcm->pdbuff[id] =
// pcm->pebuff[id] = pcm->pfbuff[id] = a;`) and allocates
// pcm->in[id] (cmaster.c:285 does this -- Lyra-cpp folds it here
// because the allocation is co-located with the per-stream cmb).
void create_cmbuffs(int id, int accept,
                    int max_insize, int max_outsize, int outsize);

// Reference cmbuffs.h:55.  Tears down the per-stream cmb -- shuts
// the Inbound gate, waits for inflight Inbound, traps + joins
// cm_main, closes the semaphore + critical sections, frees the
// ring + the cmb.  Also frees the co-allocated pcm->in[id].
void destroy_cmbuffs(int id);

// Reference cmbuffs.h:57.  Wipes the ring + drains the semaphore.
// Used by SetCMRingOutsize for an at-rate change.
void flush_cmbuffs(int id);

// Reference cmbuffs.h:59.  Producer-side hand-off.  Called from
// the EP6 read thread per datagram with `nsamples` complex
// samples in interleaved {I, Q, I, Q, ...} doubles.  Copies into
// the ring under csIN; when accumulated >= r1_outsize, releases
// the semaphore by the number of full output blocks.  Gated by
// the accept flag.
void Inbound(int id, int nsamples, double* in);

// Reference cmbuffs.h:61.  Consumer-side ring-drain.  Called by
// cm_main per semaphore release.  Copies r1_outsize complex
// samples from the ring into `out` (under csOUT).  Handles wrap.
// Returns silently + _endthread()s if the run flag is 0.
void cmdata(int id, double* out);

// Reference cmbuffs.h:63.  Pump-thread body.  MMCSS Pro Audio
// priority + 2 (Windows).  Loop: wait Sem_BuffReady -> cmdata(id,
// pcm->in[id]) -> xcmaster(id).  Exits via _endthread() when the
// run flag goes 0.  Signature matches the reference's
// `_beginthread` callback contract (void*).  Inside the body the
// pointer is cast back to int.
void cm_main(void* pargs);

// Reference cmbuffs.h:65.  Live-rate change.  Shuts the Inbound
// gate, joins the pump thread, flushes the ring, updates
// outsize, restarts the pump, reopens the gate.
void SetCMRingOutsize(int id, int size);

}  // namespace lyra::wire
