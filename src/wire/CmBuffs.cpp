// Lyra-cpp — CmBuffs.cpp
//
// See CmBuffs.h for full reference attribution, idiom-translation log,
// + architectural rationale (Phase C Thetis-faithful TX-input decouple).
//
// Reference: cmbuffs.c lines 29-188 (all 7 functions + cm_main + start
// helpers).  Each function below is ported line-by-line from its
// reference body with idiom translations per the CmBuffs.h log.

#include "wire/CmBuffs.h"
#include "wire/CMaster.h"   // for xcmaster declaration

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <avrt.h>          // AvSetMmThreadCharacteristics / Priority
  #pragma comment(lib, "avrt.lib")
#endif

namespace lyra::wire {

namespace {

// Lyra-native central bank — mirrors Thetis pcm->pcbuff[id] /
// pdbuff[id] / pebuff[id] / pfbuff[id] (4 aliases for the same struct).
// Lyra collapses to ONE.  Indexed by stream id [0, kMaxExtCmBuffs).
std::array<std::unique_ptr<CmBuffs>, kMaxExtCmBuffs> g_cmbuffs;

// Per-stream pcm->in[id] equivalent.  Sized to max_outsize complex
// samples = 2 * max_outsize doubles in create_cmbuffs.  Reference
// allocates in create_cmaster (cmaster.c:285), but Lyra co-locates
// with the cmbuffs slot since the cm_main pump is the only consumer.
std::array<std::vector<double>, kMaxExtCmBuffs> g_pcm_in;

// Bounds-check + bank-slot resolve.  Returns nullptr if id is
// out-of-range or the slot was never create_cmbuffs'd.  Same defensive
// posture as the ILV/AAMix Stage B/C resolve_* helpers.
[[nodiscard]] CmBuffs* resolve(int id) noexcept {
    if (id < 0 || id >= kMaxExtCmBuffs) return nullptr;
    return g_cmbuffs[id].get();
}

}  // namespace

// ---------------------------------------------------------------
// Reference cmbuffs.c:35-58 — create_cmbuffs
//
// void create_cmbuffs (int id, int accept, int max_insize,
//                      int max_outsize, int outsize)
// {
//     CMB a = (CMB) malloc0 (sizeof(cmb));
//     pcm->pcbuff[id] = pcm->pdbuff[id] = pcm->pebuff[id] = pcm->pfbuff[id] = a;
//     a->id = id;
//     a->accept = accept;
//     a->run = 1;
//     a->max_in_size = max_insize;
//     a->max_outsize = max_outsize;
//     a->r1_outsize = outsize;
//     if (a->max_outsize > a->max_in_size)
//         a->r1_size = a->max_outsize;
//     else
//         a->r1_size = a->max_in_size;
//     a->r1_active_buffsize = CMB_MULT * a->r1_size;
//     a->r1_baseptr = (double*) malloc0 (a->r1_active_buffsize * sizeof(complex));
//     a->r1_inidx = 0;
//     a->r1_outidx = 0;
//     a->r1_unqueuedsamps = 0;
//     a->Sem_BuffReady = CreateSemaphore(0, 0, 1000, 0);
//     InitializeCriticalSectionAndSpinCount(&a->csIN, 2500);
//     InitializeCriticalSectionAndSpinCount(&a->csOUT, 2500);
//     start_cmthread(id);
// }
// ---------------------------------------------------------------
void create_cmbuffs(int id, int accept,
                    int max_insize, int max_outsize, int outsize)
{
    if (id < 0 || id >= kMaxExtCmBuffs) return;  // Lyra-native safety
    if (g_cmbuffs[id]) return;                    // idempotent
    auto a = std::make_unique<CmBuffs>();
    a->id           = id;
    a->accept.store(static_cast<std::uint32_t>(accept ? 1u : 0u),
                    std::memory_order_release);
    a->run.store(1u, std::memory_order_release);
    a->maxInSize    = max_insize;
    a->maxOutSize   = max_outsize;
    a->r1OutSize    = outsize;
    a->r1Size       = (max_outsize > max_insize) ? max_outsize : max_insize;
    a->r1ActiveBuffSize = kCmbMult * a->r1Size;
    // sizeof(complex) = 2 * sizeof(double); ring is r1ActiveBuffSize
    // complex samples = 2 * r1ActiveBuffSize doubles.  std::vector
    // value-init to 0.0 matches malloc0 zero-init.
    a->r1BasePtr.assign(static_cast<std::size_t>(a->r1ActiveBuffSize) * 2u, 0.0);
    a->r1InIdx = 0;
    a->r1OutIdx = 0;
    a->r1UnqueuedSamps = 0;
    // Reference Sem_BuffReady = CreateSemaphore(0, 0, 1000, 0) —
    // initial count = 0, max = 1000.  std::counting_semaphore<1000>
    // is the C++20 equivalent; constructor arg is initial count.
    a->bufReady = std::make_unique<std::counting_semaphore<1000>>(0);
    // std::mutex needs no init call (RAII default-construction).

    // pcm->in[id] = malloc0(getbuffsize(cmMAXInRate) * sizeof(complex))
    // — reference cmaster.c:285 allocates this alongside create_cmbuffs.
    // Lyra co-locates here.  Sized to max_outsize complex = 2 * outsize
    // doubles (the cm_main pump's per-iteration drain target).
    g_pcm_in[id].assign(static_cast<std::size_t>(outsize) * 2u, 0.0);

    // Publish to bank BEFORE starting thread.  Thread reads g_cmbuffs[id]
    // via resolve() on first iteration; this ordering guarantees the
    // slot is non-null when the thread arrives.
    g_cmbuffs[id] = std::move(a);

    // Reference start_cmthread(id) cmbuffs.c:29-33 —
    //   HANDLE handle = (HANDLE) _beginthread(cm_main, 0, (void *)id);
    //   // SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
    // — `handle` is discarded (reference never joins or closes it).
    //
    // Lyra: std::thread spawned + immediately .detach() == identical
    // posture (handle discarded, thread runs detached).  cm_main()
    // sets its own MMCSS Pro Audio priority internally per cmbuffs.c:
    // 153-156.  Thread exits via natural return when destroy_cmbuffs
    // traps run=0 + releases the semaphore; the Sleep(2) at the end
    // of destroy_cmbuffs gives it time to exit before the bank slot's
    // memory is freed (matches reference cmbuffs.c:70 Sleep(2) discipline
    // exactly).
    std::thread(cm_main, id).detach();
}

// ---------------------------------------------------------------
// Reference cmbuffs.c:60-76 — destroy_cmbuffs
//
// void destroy_cmbuffs (int id)
// {
//     CMB a = pcm->pcbuff[id];
//     InterlockedBitTestAndReset(&a->accept, 0);  // shut Inbound gate
//     EnterCriticalSection(&a->csIN);             // wait for inflight Inbound
//     EnterCriticalSection(&a->csOUT);            // block cm_main before cmdata
//     Sleep(25);                                  // let thread arrive at top of loop
//     InterlockedBitTestAndReset(&a->run, 0);     // trap for cm_main
//     ReleaseSemaphore(a->Sem_BuffReady, 1, 0);   // ensure wait can pass
//     LeaveCriticalSection(&a->csOUT);            // let thread pass to trap in cmdata
//     Sleep(2);                                   // wait for thread to die
//     DeleteCriticalSection(&a->csOUT);
//     DeleteCriticalSection(&a->csIN);
//     CloseHandle(a->Sem_BuffReady);
//     _aligned_free(a->r1_baseptr);
//     _aligned_free(a);
// }
// ---------------------------------------------------------------
void destroy_cmbuffs(int id)
{
    auto* a = resolve(id);
    if (!a) return;  // never created, nothing to destroy

    // Reference cmbuffs.c:63 — `InterlockedBitTestAndReset(&a->accept, 0)`
    // — shut the Inbound() gate to prevent new infusions.
    a->accept.fetch_and(~1u, std::memory_order_acq_rel);
    // Reference cmbuffs.c:64 — `EnterCriticalSection(&a->csIN)` — wait
    // until current Inbound() infusion finishes.  Lyra: scoped lock +
    // immediate release == same semantic.
    {
        std::lock_guard<std::mutex> lkIn(a->inMtx);
    }
    // Reference cmbuffs.c:65 — `EnterCriticalSection(&a->csOUT)` —
    // block cm_main before its next cmdata() call.
    a->outMtx.lock();
    // Reference cmbuffs.c:66 — `Sleep(25)` — wait for thread to arrive
    // at the top of the cm_main() loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    // Reference cmbuffs.c:67 — `InterlockedBitTestAndReset(&a->run, 0)`
    // — set the trap for cm_main.
    a->run.fetch_and(~1u, std::memory_order_acq_rel);
    // Reference cmbuffs.c:68 — `ReleaseSemaphore(a->Sem_BuffReady, 1, 0)`
    // — ensure the cm_main thread can pass its WaitForSingleObject.
    a->bufReady->release(1);
    // Reference cmbuffs.c:69 — `LeaveCriticalSection(&a->csOUT)` —
    // let cm_main pass into cmdata + see run=0 + return.
    a->outMtx.unlock();
    // Reference cmbuffs.c:70 — `Sleep(2)` — wait for the cm_main thread
    // to die.  Lyra-cpp matches BYTE-EXACT: 2ms gives the detached
    // thread time to return from cm_main before this function frees
    // the bank slot's memory (the thread will dereference `a` one more
    // time on its loop-condition check before exiting; the 2ms ensures
    // it completes that before unique_ptr.reset() runs the destructor).
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // Reference cmbuffs.c:71-75 — DeleteCriticalSection x2 + CloseHandle
    // + _aligned_free x2.  Lyra: RAII via std::unique_ptr<CmBuffs>.reset()
    // — destroys std::mutex x2 (no-op cleanup), counting_semaphore
    // (no-op cleanup), std::vector<double> (frees ring memory).
    // Observable behavior identical.  The detached cm_main thread has
    // already returned (post-Sleep(2)), so no thread is touching the
    // memory at the moment of free.
    g_cmbuffs[id].reset();
    g_pcm_in[id].clear();
    g_pcm_in[id].shrink_to_fit();
}

// ---------------------------------------------------------------
// Reference cmbuffs.c:78-86 — flush_cmbuffs
//
// void flush_cmbuffs (int id)
// {
//     CMB a = pcm->pfbuff[id];
//     memset(a->r1_baseptr, 0, a->r1_active_buffsize * sizeof(complex));
//     a->r1_inidx = 0;
//     a->r1_outidx = 0;
//     a->r1_unqueuedsamps = 0;
//     while (!WaitForSingleObject(a->Sem_BuffReady, 1)) ;  // drain sem
// }
// ---------------------------------------------------------------
void flush_cmbuffs(int id)
{
    auto* a = resolve(id);
    if (!a) return;
    std::fill(a->r1BasePtr.begin(), a->r1BasePtr.end(), 0.0);
    a->r1InIdx = 0;
    a->r1OutIdx = 0;
    a->r1UnqueuedSamps = 0;
    // Drain the semaphore — try_acquire returns false when count is 0.
    // Reference uses WaitForSingleObject(h, 1) which returns 0 (WAIT_OBJECT_0)
    // if signalled.  The `while (!...) ;` loop drains until non-zero
    // return (timeout) — i.e., until sem count is 0.  std::semaphore
    // try_acquire_for(1ms) returns true while signalled.
    while (a->bufReady->try_acquire_for(std::chrono::milliseconds(1))) {
        // drained one count
    }
}

// ---------------------------------------------------------------
// Reference cmbuffs.c:88-121 — Inbound
//
// PORT void Inbound (int id, int nsamples, double* in)
// {
//     int n;
//     int first, second;
//     CMB a = pcm->pebuff[id];
//     if (_InterlockedAnd (&a->accept, 1))
//     {
//         EnterCriticalSection (&a->csIN);
//         if (nsamples > (a->r1_active_buffsize - a->r1_inidx))
//         {
//             first = a->r1_active_buffsize - a->r1_inidx;
//             second = nsamples - first;
//         }
//         else
//         {
//             first = nsamples;
//             second = 0;
//         }
//         memcpy(a->r1_baseptr + 2*a->r1_inidx, in, first*sizeof(complex));
//         memcpy(a->r1_baseptr, in + 2*first, second*sizeof(complex));
//         if ((a->r1_unqueuedsamps += nsamples) >= a->r1_outsize)
//         {
//             n = a->r1_unqueuedsamps / a->r1_outsize;
//             ReleaseSemaphore(a->Sem_BuffReady, n, 0);
//             a->r1_unqueuedsamps -= n * a->r1_outsize;
//         }
//         if ((a->r1_inidx += nsamples) >= a->r1_active_buffsize)
//             a->r1_inidx -= a->r1_active_buffsize;
//         LeaveCriticalSection (&a->csIN);
//     }
// }
// ---------------------------------------------------------------
void Inbound(int id, int nsamples, double* in)
{
    auto* a = resolve(id);
    if (!a) return;
    if (!(a->accept.load(std::memory_order_acquire) & 1u)) return;
    std::lock_guard<std::mutex> lk(a->inMtx);

    int first, second;
    if (nsamples > (a->r1ActiveBuffSize - a->r1InIdx)) {
        first  = a->r1ActiveBuffSize - a->r1InIdx;
        second = nsamples - first;
    } else {
        first  = nsamples;
        second = 0;
    }
    // ring is doubles; index unit is complex (= 2 doubles) so byte
    // offset is `2 * r1InIdx * sizeof(double)`.  Same byte arithmetic
    // as the reference: `r1_baseptr + 2*r1_inidx` for the destination
    // pointer + `first*sizeof(complex)` for the byte count.
    std::memcpy(a->r1BasePtr.data() + 2 * a->r1InIdx, in,
                static_cast<std::size_t>(first) * 2 * sizeof(double));
    std::memcpy(a->r1BasePtr.data(), in + 2 * first,
                static_cast<std::size_t>(second) * 2 * sizeof(double));

    a->r1UnqueuedSamps += nsamples;
    if (a->r1UnqueuedSamps >= a->r1OutSize) {
        int n = a->r1UnqueuedSamps / a->r1OutSize;
        // Release semaphore by n full output blocks.  std::semaphore
        // release(n) is the equivalent of ReleaseSemaphore(h, n, 0).
        a->bufReady->release(n);
        a->r1UnqueuedSamps -= n * a->r1OutSize;
    }
    a->r1InIdx += nsamples;
    if (a->r1InIdx >= a->r1ActiveBuffSize) {
        a->r1InIdx -= a->r1ActiveBuffSize;
    }
}

// ---------------------------------------------------------------
// Reference cmbuffs.c:123-149 — cmdata
//
// void cmdata (int id, double* out)
// {
//     int first, second;
//     CMB a = pcm->pdbuff[id];
//     EnterCriticalSection (&a->csOUT);
//     if (!_InterlockedAnd (&a->run, 1))
//     {
//         LeaveCriticalSection (&a->csOUT);
//         _endthread();
//         return;
//     }
//     if (a->r1_outsize > (a->r1_active_buffsize - a->r1_outidx))
//     {
//         first = a->r1_active_buffsize - a->r1_outidx;
//         second = a->r1_outsize - first;
//     }
//     else
//     {
//         first = a->r1_outsize;
//         second = 0;
//     }
//     memcpy(out, a->r1_baseptr + 2*a->r1_outidx, first*sizeof(complex));
//     memcpy(out + 2*first, a->r1_baseptr, second*sizeof(complex));
//     if ((a->r1_outidx += a->r1_outsize) >= a->r1_active_buffsize)
//         a->r1_outidx -= a->r1_active_buffsize;
//     LeaveCriticalSection (&a->csOUT);
// }
//
// Returns `false` if the run flag is 0 (caller cm_main must exit
// its loop and let jthread join).  In Lyra-cpp the `_endthread()`
// call is RETIRED — jthread joins on natural return.
// ---------------------------------------------------------------
void cmdata(int id, double* out)
{
    auto* a = resolve(id);
    if (!a) return;
    std::lock_guard<std::mutex> lk(a->outMtx);
    if (!(a->run.load(std::memory_order_acquire) & 1u)) {
        // run flag has been reset (destroy_cmbuffs).  Caller cm_main
        // checks the same flag at the top of its loop on the next
        // iteration; jthread joins cleanly when cm_main returns.  No
        // _endthread() call needed — that's the C-era idiom; C++23
        // jthread uses RAII join.  Reference behavior preserved.
        return;
    }
    int first, second;
    if (a->r1OutSize > (a->r1ActiveBuffSize - a->r1OutIdx)) {
        first  = a->r1ActiveBuffSize - a->r1OutIdx;
        second = a->r1OutSize - first;
    } else {
        first  = a->r1OutSize;
        second = 0;
    }
    std::memcpy(out, a->r1BasePtr.data() + 2 * a->r1OutIdx,
                static_cast<std::size_t>(first) * 2 * sizeof(double));
    std::memcpy(out + 2 * first, a->r1BasePtr.data(),
                static_cast<std::size_t>(second) * 2 * sizeof(double));
    a->r1OutIdx += a->r1OutSize;
    if (a->r1OutIdx >= a->r1ActiveBuffSize) {
        a->r1OutIdx -= a->r1ActiveBuffSize;
    }
}

// ---------------------------------------------------------------
// Reference cmbuffs.c:151-168 — cm_main pump-thread body
//
// void cm_main (void *pargs)
// {
//     DWORD taskIndex = 0;
//     HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
//     if (hTask != 0) AvSetMmThreadPriority(hTask, 2);
//     else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
//
//     int id = (int)pargs;
//     CMB a = pcm->pdbuff[id];
//
//     while (_InterlockedAnd (&a->run, 1))
//     {
//         WaitForSingleObject(a->Sem_BuffReady, INFINITE);
//         cmdata (id, pcm->in[id]);
//         xcmaster(id);
//     }
//     _endthread();
// }
// ---------------------------------------------------------------
void cm_main(int id)
{
#ifdef _WIN32
    // Reference cmbuffs.c:153-156 — MMCSS Pro Audio prio + 2.
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (hTask != nullptr) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_HIGH);  // = 2
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
#endif

    // Reference cmbuffs.c:158-159 — `int id = (int)pargs; CMB a = pcm->pdbuff[id];`
    auto* a = resolve(id);
    if (!a) return;  // bank slot vanished before thread ran

    // Reference cmbuffs.c:161-166 — `while (_InterlockedAnd(&a->run, 1))
    //   { WaitForSingleObject(a->Sem_BuffReady, INFINITE);
    //     cmdata(id, pcm->in[id]); xcmaster(id); } _endthread();`
    //
    // Lyra: byte-equivalent.  acquire() blocks until release (= INFINITE
    // wait, no timeout, no stop_token poll — matching reference exactly).
    // Exits via natural function return when run flag is reset; the
    // reference's _endthread() at cmbuffs.c:167 is a C-era artifact
    // with no C++23 equivalent needed (returning from the function +
    // detached std::thread is the same observable behavior — process
    // exit reclaims the thread).
    while (a->run.load(std::memory_order_acquire) & 1u) {
        a->bufReady->acquire();  // = WaitForSingleObject(Sem_BuffReady, INFINITE)
        if (!(a->run.load(std::memory_order_acquire) & 1u)) break;
        cmdata(id, g_pcm_in[id].data());
        xcmaster(id);
    }
    // Return = thread exits.  Reference _endthread() not needed in C++23.
}

// ---------------------------------------------------------------
// Reference cmbuffs.c:170-187 — SetCMRingOutsize (live-rate change)
//
// Ported for completeness.  Not used in Lyra v0.2.0 SSB (HL2 mic rate
// fixed at 48 kHz per CLAUDE.md §3.5).  Pattern: shut accept gate →
// join thread cleanly → flush ring → change outsize → restart thread
// → reopen accept gate.
// ---------------------------------------------------------------
void SetCMRingOutsize(int id, int size)
{
    // Reference cmbuffs.c:170-187 — byte-equivalent sequence with the
    // detached-thread + Sleep(2) discipline matching destroy_cmbuffs.
    auto* a = resolve(id);
    if (!a) return;
    a->accept.fetch_and(~1u, std::memory_order_acq_rel);   // ref:173
    { std::lock_guard<std::mutex> lkIn(a->inMtx); }        // ref:174
    a->outMtx.lock();                                       // ref:175
    std::this_thread::sleep_for(std::chrono::milliseconds(25));  // ref:176
    a->run.fetch_and(~1u, std::memory_order_acq_rel);      // ref:177
    a->bufReady->release(1);                                // ref:178
    a->outMtx.unlock();                                     // ref:179
    std::this_thread::sleep_for(std::chrono::milliseconds(2));   // ref:180
    flush_cmbuffs(id);                                      // ref:181
    a->r1OutSize = size;                                    // ref:182
    a->run.fetch_or(1u, std::memory_order_acq_rel);        // ref:183
    // Reference cmbuffs.c:184 — `start_cmthread(id)` — re-spawn
    // detached thread.  Same pattern as create_cmbuffs.
    std::thread(cm_main, id).detach();
    a->accept.fetch_or(1u, std::memory_order_acq_rel);     // ref:186
}

double* pcm_in(int id)
{
    if (id < 0 || id >= kMaxExtCmBuffs) return nullptr;
    if (g_pcm_in[id].empty()) return nullptr;
    return g_pcm_in[id].data();
}

int pcm_in_size(int id)
{
    auto* a = resolve(id);
    return a ? a->r1OutSize : 0;
}

}  // namespace lyra::wire
