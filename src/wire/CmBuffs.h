// Lyra-cpp — CmBuffs.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Source files:
//   - ChannelMaster/cmbuffs.h: struct cmb + 7 function decls (port lines 27-68)
//   - ChannelMaster/cmbuffs.c: implementations (ported at Phase C below)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ----------------------------------------------------------------
// Phase C — Thetis-faithful TX-input decouple architecture
// ----------------------------------------------------------------
//
// Replaces the Stage 7.1 TxIqRing's wrong-side post-TXA placement
// with a reference-faithful pre-TXA elastic CMB ring + cm_main pump
// thread per Thetis cmaster.c:273-286 + cmbuffs.{h,c}.  The Thetis
// architecture:
//
//   Producer side (EP6 read thread):
//     Inbound(id, n, in) -> ring (under csIN) -> Sem_BuffReady release
//
//   Consumer side (cm_main pump thread per stream):
//     wait Sem_BuffReady -> cmdata(id, pcm->in[id]) (under csOUT) ->
//     xcmaster(id) -> fexchange0 + xilv -> Outbound -> wire
//
// Decouples wire-clock producer (EP6 read) from WDSP-clock consumer
// (TX DSP pump) via an elastic ring with semaphore handshake.  The
// CMB_MULT=3 size means the ring holds ~3 output blocks' worth of
// samples — enough cushion for the wire/DSP cadence mismatch without
// the drop-oldest discipline a Lyra-native SPSC ring would need.
//
// ----------------------------------------------------------------
// C → C++23 idiom translations (every translation preserves
// observable behaviour; see per-symbol comments for exceptions):
// ----------------------------------------------------------------
//
//   - `struct cmb` typedef           -> `struct CmBuffs` with in-class
//                                       initializers + RAII members.
//                                       Layout is Lyra-private (no
//                                       DLL-ABI constraint); semantic
//                                       contract preserved.
//   - `volatile long run/accept`     -> `std::atomic<std::uint32_t>`
//      + `_InterlockedAnd / Inter-     with .load() & mask for the
//      lockedBitTestAndSet/Reset`      reference InterlockedAnd reads
//                                       and fetch_or(1u<<bit) /
//                                       fetch_and(~(1u<<bit)) for the
//                                       bit-test-and-set/reset writes.
//                                       Cross-thread: Inbound (EP6
//                                       read thread) vs cm_main (pump
//                                       thread) vs destroy_cmbuffs
//                                       (main thread).  Same pattern
//                                       as ILV.h Stage C.
//   - `double* r1_baseptr`           -> `std::vector<double>` RAII;
//      via `malloc0(...)` /            sized `r1_active_buffsize *
//      `_aligned_free(...)`            sizeof(complex) / sizeof(double)`
//                                       = 2 * r1_active_buffsize
//                                       doubles, allocated in
//                                       create_cmbuffs.  Zero-init
//                                       matches malloc0 semantics;
//                                       freed on CmBuffs destruction.
//   - `HANDLE Sem_BuffReady`         -> `std::counting_semaphore<1000>`
//      `CreateSemaphore(0,0,1000,0)`   (C++20).  Same max=1000 the
//      `ReleaseSemaphore(h, n, 0)`     reference uses.  Same idiom
//      `WaitForSingleObject(h, INF)`   established in AAMix.cpp Stage B.
//                                       Release via .release(n);
//                                       wait via .acquire().
//   - `CRITICAL_SECTION csIN/csOUT`  -> `std::mutex inMtx_ / outMtx_`.
//      `InitializeCriticalSection-     The 2500 spin count is a
//      AndSpinCount(&cs, 2500)`        Windows-specific optimization
//      `EnterCriticalSection(&cs)`     (briefly spin before blocking);
//      `LeaveCriticalSection(&cs)`     std::mutex on Win32 already uses
//                                       a similar adaptive strategy
//                                       (Windows kernel spinlock then
//                                       SRWLock fallback) — behavior
//                                       equivalent in practice.
//   - `_beginthread(cm_main, 0, id)` -> `std::thread t(cm_main, id);
//      `_endthread()`                    t.detach();`.  Reference posture
//                                        EXACTLY: thread runs detached
//                                        (no join), exits via natural
//                                        return from its run-flag loop
//                                        when destroy_cmbuffs traps
//                                        run=0 + releases the semaphore.
//                                        Reference's _endthread() at
//                                        cmbuffs.c:131 + :167 is a
//                                        no-op in C++23 (the thread
//                                        function just returns); the
//                                        Sleep(2) after release in
//                                        destroy_cmbuffs gives the
//                                        thread time to exit before
//                                        the struct is freed (same as
//                                        reference cmbuffs.c:70).
//                                        std::thread::detach() is the
//                                        C++23 equivalent of "fire and
//                                        forget" the reference's
//                                        _beginthread returned-handle
//                                        usage pattern (the reference
//                                        ignores the returned HANDLE
//                                        at cmbuffs.c:31 -- never
//                                        joins or closes it).
//   - `pcm->pcbuff[id]` /            -> Single `CmBuffs*` per stream in
//      `pcm->pdbuff[id]` /             a Lyra-native `pcmbuffs[]` bank
//      `pcm->pebuff[id]` /             (mirrors the AAMix paamix[] /
//      `pcm->pfbuff[id]`               ILV pilv[] pattern).  Thetis's 4
//                                       pointer aliases all point to the
//                                       same struct — Lyra collapses to
//                                       ONE.  Same observable behavior.
//   - `AvSetMmThreadCharacteristics  -> KEPT.  Win32-only API, no
//      ("Pro Audio", &taskIndex)`      portable equivalent.  Same
//      `AvSetMmThreadPriority(2)`      pattern already used in
//                                       AAMix.cpp Stage B start_mixthread.
//   - `complex` (= 2 doubles)        -> Lyra uses raw `double` arrays
//                                       sized 2× the complex count.
//                                       Same byte layout; same
//                                       memcpy compatibility.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <semaphore>
#include <vector>

namespace lyra::wire {

// CMB_MULT — Thetis cmbuffs.h:31 `#define CMB_MULT (3)`.
// Ring size = CMB_MULT × max(max_in_size, max_outsize) complex
// samples — enough cushion for ~3 output blocks of producer/consumer
// cadence mismatch without dropping.
inline constexpr int kCmbMult = 3;

// Per-stream slot count.  Thetis cmaster.h hardcodes `cmSTREAM = 5`
// (5 streams: 0=RX1, 1=TX, 2=RX2, 3=RX-aux, 4=RX-sub).  Lyra-cpp
// v0.2.0 SSB uses ONLY stream 1 (TX) for cmbuffs — RX streams go
// through WdspEngine direct dispatch, not the central cmaster pump.
// MAX_EXT_CMBUFFS sized for future growth (RX-into-central-pump
// migration would bump to 5).
inline constexpr int kMaxExtCmBuffs = 5;

// ---------------------------------------------------------------
// struct CmBuffs — direct port of Thetis cmbuffs.h:32-51 struct cmb.
// Field ordering preserved for grep-parity with the reference.
// ---------------------------------------------------------------
struct CmBuffs {
    int   id           = 0;
    int   maxInSize    = 0;  // max input number of complex samples
    int   maxOutSize   = 0;  // max output number of complex samples
    int   r1OutSize    = 0;  // complex samples taken out of ring per fexchange

    int   r1Size       = 0;  // size of a single maximum-sized transfer
    int   r1ActiveBuffSize = 0;  // size of ring (in complex samples)

    // r1_baseptr equivalent — sized to 2 * r1ActiveBuffSize doubles
    // (each complex == 2 doubles).  Indices below are in 'double'
    // pairs; actual byte offsets are 2 × that value × sizeof(double).
    std::vector<double> r1BasePtr;
    int   r1InIdx      = 0;  // in 'double-pair' units (complex-sized)
    int   r1OutIdx     = 0;
    int   r1UnqueuedSamps = 0;  // input samples not yet queued for execution

    // Thread-coordination atomics — Thetis volatile long with
    // InterlockedBitTestAndSet/Reset(bit 0); Lyra acquire/release
    // load/mask + fetch_or/fetch_and (matches ILV.h Stage C pattern).
    std::atomic<std::uint32_t> run    = 1u;  // 1 = thread loops; 0 = thread exits
    std::atomic<std::uint32_t> accept = 0u;  // 1 = Inbound() accepts input

    // Sem_BuffReady — count = number of output-sized buffers queued
    // for processing.  Max 1000 matches Thetis Win32
    // CreateSemaphore(0, 0, 1000, 0) — initial=0, max=1000.
    std::unique_ptr<std::counting_semaphore<1000>> bufReady;

    // csOUT / csIN — block output / input during param update or flush.
    std::mutex outMtx;
    std::mutex inMtx;

    // pumpThread handle is NOT stored — reference cmbuffs.c:31 ignores
    // the HANDLE returned by _beginthread (no join, no close).  Lyra
    // matches: std::thread spawned + immediately detached() in
    // create_cmbuffs.  Thread exits via natural return when destroy_
    // cmbuffs traps run=0.  Sleep(2) in destroy_cmbuffs gives the
    // thread time to exit before this struct's memory is freed —
    // identical to reference cmbuffs.c:70 Sleep(2) discipline.
};

// ---------------------------------------------------------------
// Public API — reference cmbuffs.h:53-65 surface preserved.
// ---------------------------------------------------------------

// Reference cmbuffs.h:53 — `extern void create_cmbuffs (int id,
//   int accept, int max_insize, int max_outsize, int outsize)`.
//
// Allocates the per-stream CmBuffs slot, sizes the ring (CMB_MULT ×
// max(in,out) complex), creates the semaphore + mutexes, starts the
// cm_main pump thread.  MUST be called once per stream at
// create_cmaster() time, BEFORE any Inbound() call.
void create_cmbuffs(int id, int accept,
                    int max_insize, int max_outsize, int outsize);

// Reference cmbuffs.h:55 — `extern void destroy_cmbuffs (int id)`.
//
// Tears down the per-stream CmBuffs slot — closes the Inbound gate,
// waits for inflight Inbound, traps + joins the cm_main pump thread,
// cleans up.  MUST be called once per stream at destroy_cmaster()
// time, AFTER all Inbound() callers have stopped (the EP6 thread is
// joined first, per Lyra's main.cpp aboutToQuit handler order).
void destroy_cmbuffs(int id);

// Reference cmbuffs.h:57 — `extern void flush_cmbuffs (int id)`.
//
// Wipes the ring + drains the semaphore.  Used by SetCMRingOutsize
// for an at-rate change; otherwise unused in Lyra v0.2.0 SSB.
void flush_cmbuffs(int id);

// Reference cmbuffs.h:59 — `extern __declspec (dllexport) void
//   Inbound (int id, int nsamples, double* in)`.
//
// Producer-side hand-off.  Called from the EP6 read thread per
// datagram (HL2 mic stream id=1) with `n` complex samples in
// interleaved {I, Q, I, Q, ...} doubles.  Copies into the ring under
// inMtx; when accumulated ≥ r1OutSize, releases the semaphore by
// the number of full output blocks.  Gated by accept atomic.
void Inbound(int id, int nsamples, double* in);

// Reference cmbuffs.h:61 — `extern void cmdata (int id, double* out)`.
//
// Consumer-side ring-drain.  Called by the cm_main pump thread per
// semaphore release.  Copies r1OutSize complex samples from the
// ring into `out` (under outMtx).  Handles wrap.  Returns silently
// if run flag is 0 (thread is being torn down).
void cmdata(int id, double* out);

// Reference cmbuffs.h:63 — `extern void cm_main (void *pargs)`.
//
// Pump-thread body.  MMCSS Pro Audio priority + 2 (Windows).  Loop:
// wait on Sem_BuffReady → cmdata(id, pcm->in[id]) → xcmaster(id).
// Exits when the run atomic goes 0 (the destroy_cmbuffs trap).
// Called via std::thread inside create_cmbuffs + immediately
// detached (matches reference _beginthread + no-join posture); not
// invoked directly by callers.
//
// Signature: reference is `cm_main(void* pargs)` with `int id =
// (int)pargs` cast on entry (cmbuffs.c:158).  Lyra takes int
// directly per Rule 26 (C++23 std::thread captures typed args
// natively).  Same semantic.
void cm_main(int id);

// Reference cmbuffs.h:65 — `extern void SetCMRingOutsize (int id,
//   int size)`.
//
// Live-rate change.  Shuts the Inbound gate, joins the pump thread,
// flushes the ring, updates outsize, restarts the pump, reopens the
// gate.  Used by Thetis SetXcmInrate / SetXmtrChannelOutrate paths;
// not used in Lyra v0.2.0 SSB (HL2 mic rate is fixed at 48 kHz per
// CLAUDE.md §3.5).  Ported for completeness + forward-compat.
void SetCMRingOutsize(int id, int size);

// Per-stream input buffer — reference `pcm->in[id]` (sized
// `getbuffsize(cmMAXInRate) * sizeof(complex)`).  Lyra-cpp exposes
// it as a public accessor so xcmaster() / cmdata() / etc. can reach
// it through a Lyra-native lookup instead of pcm-> field access.
// Allocated by create_cmbuffs alongside the ring.
double* pcm_in(int id);  // returns nullptr if create_cmbuffs(id) hasn't run

// Per-stream r1OutSize accessor — reference would read `pcm->pcbuff
// [id]->r1_outsize`.  Used by xcmaster case-1 body to pass the
// per-iteration sample count to xcmasterTickTx.  Returns 0 if the
// stream hasn't been create_cmbuffs'd.
int pcm_in_size(int id);

}  // namespace lyra::wire
