// Lyra-cpp — CmBuffs.cpp
//
// See CmBuffs.h for full reference attribution + the locked
// byte-faithful Win32 retrofit posture + the single allowed
// forced deviation (struct tag `CmBuffs` per rule #8).
//
// Reference: cmbuffs.c lines 29-188.  Each function body is
// ported line-by-line with reference C inlined as block
// comments above each definition.

#include "wire/CmBuffs.h"
#include "wire/CMaster.h"   // pcm + CMasterState (for pcbuff/pdbuff/
                            //   pebuff/pfbuff/in aliases + xcmaster)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>            // AvSetMmThreadCharacteristics / Priority
#include <process.h>         // _beginthread / _endthread
#include <intrin.h>          // _InterlockedAnd /
                             //   InterlockedBitTestAndSet / Reset

#include <cstddef>
#include <cstdlib>           // calloc / free
#include <cstring>           // memcpy / memset

#pragma comment(lib, "avrt.lib")

namespace lyra::wire {

// `sizeof(complex)` in the reference is `2 * sizeof(double)` (16
// bytes).  Re-aliased here at file scope so the .cpp body keeps
// the reference's `sizeof(complex)` byte-count pattern verbatim.
// Mirrors AAMix.cpp's TU-local alias discipline.
namespace { constexpr std::size_t kSizeofComplex = 2 * sizeof(double); }

// =========================================================================
// start_cmthread -- reference cmbuffs.c:29-33.
// =========================================================================
//
//   void start_cmthread (int id)
//   {
//     HANDLE handle = (HANDLE) _beginthread(cm_main, 0, (void *)id);
//     //SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
//   }
//
// Reference discards `handle` (no join, no close).  `id` casts
// through intptr_t for clean 64-bit pointer round-trip (reference
// is Win32 only -- the original `(void *)id` cast relies on int
// being pointer-sized; explicit intptr_t makes the C++23 idiom
// portable without changing observable behaviour).
static void start_cmthread(int id)
{
    HANDLE handle = (HANDLE) _beginthread(cm_main, 0,
                                           (void*)(intptr_t)id);
    (void) handle;
    // SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
}

// =========================================================================
// create_cmbuffs -- reference cmbuffs.c:35-58.
// =========================================================================
//
//   void create_cmbuffs (int id, int accept, int max_insize,
//                        int max_outsize, int outsize)
//   {
//     CMB a = (CMB) malloc0 (sizeof(cmb));
//     pcm->pcbuff[id] = pcm->pdbuff[id] = pcm->pebuff[id]
//                     = pcm->pfbuff[id] = a;
//     a->id = id;
//     a->accept = accept;
//     a->run = 1;
//     a->max_in_size = max_insize;
//     a->max_outsize = max_outsize;
//     a->r1_outsize = outsize;
//     if (a->max_outsize > a->max_in_size)
//       a->r1_size = a->max_outsize;
//     else
//       a->r1_size = a->max_in_size;
//     a->r1_active_buffsize = CMB_MULT * a->r1_size;
//     a->r1_baseptr = (double*) malloc0(a->r1_active_buffsize *
//                                        sizeof(complex));
//     a->r1_inidx = 0;
//     a->r1_outidx = 0;
//     a->r1_unqueuedsamps = 0;
//     a->Sem_BuffReady = CreateSemaphore(0, 0, 1000, 0);
//     InitializeCriticalSectionAndSpinCount(&a->csIN,  2500);
//     InitializeCriticalSectionAndSpinCount(&a->csOUT, 2500);
//     start_cmthread(id);
//   }
//
// Lyra-cpp port: the per-stream cmb is `calloc`-allocated (zeroes
// match the reference malloc0 semantics for the populated fields;
// the in-class initialisers are absent because the struct is a
// POD-layout aggregate to match the reference cmb byte-for-byte).
// Co-allocates pcm->in[id] which the reference allocates separately
// in create_cmaster (cmaster.c:285) -- Lyra-cpp folds it here so
// the per-stream buffer's lifetime matches the cmb's.
void create_cmbuffs(int id, int accept,
                    int max_insize, int max_outsize, int outsize)
{
    CmBuffs* a = (CmBuffs*) calloc(1, sizeof(CmBuffs));

    // Reference cmbuffs.c:38 -- 4-alias publish into pcm.
    pcm->pcbuff[id] = pcm->pdbuff[id]
                    = pcm->pebuff[id] = pcm->pfbuff[id] = a;

    a->id = id;
    a->accept = accept;
    a->run = 1;
    a->max_in_size = max_insize;
    a->max_outsize = max_outsize;
    a->r1_outsize = outsize;
    if (a->max_outsize > a->max_in_size)
        a->r1_size = a->max_outsize;
    else
        a->r1_size = a->max_in_size;
    a->r1_active_buffsize = CMB_MULT * a->r1_size;
    a->r1_baseptr = (double*) calloc((size_t)a->r1_active_buffsize,
                                       kSizeofComplex);
    a->r1_inidx = 0;
    a->r1_outidx = 0;
    a->r1_unqueuedsamps = 0;
    a->Sem_BuffReady = CreateSemaphore(nullptr, 0, 1000, nullptr);
    InitializeCriticalSectionAndSpinCount(&a->csIN,  2500);
    InitializeCriticalSectionAndSpinCount(&a->csOUT, 2500);

    // Co-allocate pcm->in[id] -- reference cmaster.c:285:
    //   pcm->in[i] = (double*)malloc0(getbuffsize(pcm->cmMAXInRate) *
    //                                  sizeof(complex));
    // Lyra-cpp uses max_outsize as the per-iteration drain target
    // size (the cm_main pump calls cmdata(id, pcm->in[id]) for
    // r1_outsize complex samples; size to max_outsize so any
    // outsize within the configured ring fits).
    pcm->in[id] = (double*) calloc((size_t)max_outsize, kSizeofComplex);

    start_cmthread(id);
}

// =========================================================================
// destroy_cmbuffs -- reference cmbuffs.c:60-76.
// =========================================================================
//
//   void destroy_cmbuffs (int id)
//   {
//     CMB a = pcm->pcbuff[id];
//     InterlockedBitTestAndReset(&a->accept, 0);
//     EnterCriticalSection(&a->csIN);
//     EnterCriticalSection(&a->csOUT);
//     Sleep(25);
//     InterlockedBitTestAndReset(&a->run, 0);
//     ReleaseSemaphore(a->Sem_BuffReady, 1, 0);
//     LeaveCriticalSection(&a->csOUT);
//     Sleep(2);
//     DeleteCriticalSection(&a->csOUT);
//     DeleteCriticalSection(&a->csIN);
//     CloseHandle(a->Sem_BuffReady);
//     _aligned_free(a->r1_baseptr);
//     _aligned_free(a);
//   }
//
// Lyra-cpp port: byte-faithful sequence.  After the reference's
// teardown, also free the co-allocated pcm->in[id] and clear the
// four pcm aliases so a future create_cmbuffs(id, ...) starts
// from a clean bank slot.  The reference doesn't clear the
// aliases because the process exits with the cm[] static.
void destroy_cmbuffs(int id)
{
    CmBuffs* a = pcm->pcbuff[id];
    if (a == nullptr) return;  // not created -- nothing to tear down

    InterlockedBitTestAndReset(&a->accept, 0);
    EnterCriticalSection(&a->csIN);
    EnterCriticalSection(&a->csOUT);
    Sleep(25);
    InterlockedBitTestAndReset(&a->run, 0);
    ReleaseSemaphore(a->Sem_BuffReady, 1, nullptr);
    LeaveCriticalSection(&a->csOUT);
    Sleep(2);
    DeleteCriticalSection(&a->csOUT);
    DeleteCriticalSection(&a->csIN);
    CloseHandle(a->Sem_BuffReady);
    free(a->r1_baseptr);
    free(a);

    // Lyra-cpp post-teardown: clear the pcm bank aliases + free
    // the co-allocated pcm->in[id].
    pcm->pcbuff[id] = pcm->pdbuff[id]
                    = pcm->pebuff[id] = pcm->pfbuff[id] = nullptr;
    if (pcm->in[id] != nullptr) {
        free(pcm->in[id]);
        pcm->in[id] = nullptr;
    }
}

// =========================================================================
// flush_cmbuffs -- reference cmbuffs.c:78-86.
// =========================================================================
//
//   void flush_cmbuffs (int id)
//   {
//     CMB a = pcm->pfbuff[id];
//     memset(a->r1_baseptr, 0, a->r1_active_buffsize * sizeof(complex));
//     a->r1_inidx = 0;
//     a->r1_outidx = 0;
//     a->r1_unqueuedsamps = 0;
//     while (!WaitForSingleObject(a->Sem_BuffReady, 1)) ;
//   }
void flush_cmbuffs(int id)
{
    CmBuffs* a = pcm->pfbuff[id];
    memset(a->r1_baseptr, 0,
           (size_t)a->r1_active_buffsize * kSizeofComplex);
    a->r1_inidx = 0;
    a->r1_outidx = 0;
    a->r1_unqueuedsamps = 0;
    while (!WaitForSingleObject(a->Sem_BuffReady, 1)) ;
}

// =========================================================================
// Inbound -- reference cmbuffs.c:88-121.
// =========================================================================
//
//   PORT void Inbound (int id, int nsamples, double* in)
//   {
//     int n;
//     int first, second;
//     CMB a = pcm->pebuff[id];
//     if (_InterlockedAnd (&a->accept, 1))
//     {
//       EnterCriticalSection(&a->csIN);
//       if (nsamples > (a->r1_active_buffsize - a->r1_inidx)) {
//         first = a->r1_active_buffsize - a->r1_inidx;
//         second = nsamples - first;
//       } else {
//         first = nsamples;
//         second = 0;
//       }
//       memcpy(a->r1_baseptr + 2 * a->r1_inidx, in,
//              first * sizeof(complex));
//       memcpy(a->r1_baseptr, in + 2 * first,
//              second * sizeof(complex));
//
//       if ((a->r1_unqueuedsamps += nsamples) >= a->r1_outsize) {
//         n = a->r1_unqueuedsamps / a->r1_outsize;
//         ReleaseSemaphore(a->Sem_BuffReady, n, 0);
//         a->r1_unqueuedsamps -= n * a->r1_outsize;
//       }
//       if ((a->r1_inidx += nsamples) >= a->r1_active_buffsize)
//         a->r1_inidx -= a->r1_active_buffsize;
//       LeaveCriticalSection(&a->csIN);
//     }
//   }
void Inbound(int id, int nsamples, double* in)
{
    int n;
    int first, second;
    CmBuffs* a = pcm->pebuff[id];

    if (_InterlockedAnd(&a->accept, 1))
    {
        EnterCriticalSection(&a->csIN);
        if (nsamples > (a->r1_active_buffsize - a->r1_inidx))
        {
            first = a->r1_active_buffsize - a->r1_inidx;
            second = nsamples - first;
        }
        else
        {
            first = nsamples;
            second = 0;
        }
        memcpy(a->r1_baseptr + 2 * a->r1_inidx, in,
               (size_t)first * kSizeofComplex);
        memcpy(a->r1_baseptr, in + 2 * first,
               (size_t)second * kSizeofComplex);

        if ((a->r1_unqueuedsamps += nsamples) >= a->r1_outsize)
        {
            n = a->r1_unqueuedsamps / a->r1_outsize;
            ReleaseSemaphore(a->Sem_BuffReady, n, nullptr);
            a->r1_unqueuedsamps -= n * a->r1_outsize;
        }
        if ((a->r1_inidx += nsamples) >= a->r1_active_buffsize)
            a->r1_inidx -= a->r1_active_buffsize;
        LeaveCriticalSection(&a->csIN);
    }
}

// =========================================================================
// cmdata -- reference cmbuffs.c:123-149.
// =========================================================================
//
//   void cmdata (int id, double* out)
//   {
//     int first, second;
//     CMB a = pcm->pdbuff[id];
//     EnterCriticalSection(&a->csOUT);
//     if (!_InterlockedAnd(&a->run, 1)) {
//       LeaveCriticalSection(&a->csOUT);
//       _endthread();
//       return;
//     }
//     if (a->r1_outsize > (a->r1_active_buffsize - a->r1_outidx)) {
//       first = a->r1_active_buffsize - a->r1_outidx;
//       second = a->r1_outsize - first;
//     } else {
//       first = a->r1_outsize;
//       second = 0;
//     }
//     memcpy(out, a->r1_baseptr + 2 * a->r1_outidx,
//            first * sizeof(complex));
//     memcpy(out + 2 * first, a->r1_baseptr,
//            second * sizeof(complex));
//     if ((a->r1_outidx += a->r1_outsize) >= a->r1_active_buffsize)
//       a->r1_outidx -= a->r1_active_buffsize;
//     LeaveCriticalSection(&a->csOUT);
//   }
void cmdata(int id, double* out)
{
    int first, second;
    CmBuffs* a = pcm->pdbuff[id];
    EnterCriticalSection(&a->csOUT);
    if (!_InterlockedAnd(&a->run, 1))
    {
        LeaveCriticalSection(&a->csOUT);
        _endthread();
        return;
    }
    if (a->r1_outsize > (a->r1_active_buffsize - a->r1_outidx))
    {
        first = a->r1_active_buffsize - a->r1_outidx;
        second = a->r1_outsize - first;
    }
    else
    {
        first = a->r1_outsize;
        second = 0;
    }
    memcpy(out, a->r1_baseptr + 2 * a->r1_outidx,
           (size_t)first * kSizeofComplex);
    memcpy(out + 2 * first, a->r1_baseptr,
           (size_t)second * kSizeofComplex);
    if ((a->r1_outidx += a->r1_outsize) >= a->r1_active_buffsize)
        a->r1_outidx -= a->r1_active_buffsize;
    LeaveCriticalSection(&a->csOUT);
}

// =========================================================================
// cm_main -- reference cmbuffs.c:151-168.
// =========================================================================
//
//   void cm_main (void *pargs)
//   {
//     DWORD taskIndex = 0;
//     HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"),
//                                                  &taskIndex);
//     if (hTask != 0) AvSetMmThreadPriority(hTask, 2);
//     else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
//
//     int id = (int)pargs;
//     CMB a = pcm->pdbuff[id];
//
//     while (_InterlockedAnd(&a->run, 1))
//     {
//       WaitForSingleObject(a->Sem_BuffReady, INFINITE);
//       cmdata(id, pcm->in[id]);
//       xcmaster(id);
//     }
//     _endthread();
//   }
void cm_main(void* pargs)
{
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"),
                                                  &taskIndex);
    if (hTask != nullptr) AvSetMmThreadPriority(hTask, AVRT_PRIORITY_HIGH);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    int id = (int)(intptr_t)pargs;
    CmBuffs* a = pcm->pdbuff[id];

    while (_InterlockedAnd(&a->run, 1))
    {
        WaitForSingleObject(a->Sem_BuffReady, INFINITE);
        cmdata(id, pcm->in[id]);
        xcmaster(id);
    }
    _endthread();
}

// =========================================================================
// SetCMRingOutsize -- reference cmbuffs.c:170-187.
// =========================================================================
//
//   void SetCMRingOutsize (int id, int size)
//   {
//     CMB a = pcm->pcbuff[id];
//     InterlockedBitTestAndReset(&a->accept, 0);
//     EnterCriticalSection(&a->csIN);
//     EnterCriticalSection(&a->csOUT);
//     Sleep(25);
//     InterlockedBitTestAndReset(&a->run, 0);
//     ReleaseSemaphore(a->Sem_BuffReady, 1, 0);
//     LeaveCriticalSection(&a->csOUT);
//     Sleep(2);
//     flush_cmbuffs(id);
//     a->r1_outsize = size;
//     InterlockedBitTestAndSet(&a->run, 0);
//     start_cmthread(id);
//     LeaveCriticalSection(&a->csIN);
//     InterlockedBitTestAndSet(&a->accept, 0);
//   }
void SetCMRingOutsize(int id, int size)
{
    CmBuffs* a = pcm->pcbuff[id];
    InterlockedBitTestAndReset(&a->accept, 0);
    EnterCriticalSection(&a->csIN);
    EnterCriticalSection(&a->csOUT);
    Sleep(25);
    InterlockedBitTestAndReset(&a->run, 0);
    ReleaseSemaphore(a->Sem_BuffReady, 1, nullptr);
    LeaveCriticalSection(&a->csOUT);
    Sleep(2);
    flush_cmbuffs(id);
    a->r1_outsize = size;
    InterlockedBitTestAndSet(&a->run, 0);
    start_cmthread(id);
    LeaveCriticalSection(&a->csIN);
    InterlockedBitTestAndSet(&a->accept, 0);
}

}  // namespace lyra::wire
