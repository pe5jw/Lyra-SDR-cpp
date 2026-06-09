// Lyra-cpp — AAMix.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream:    https://github.com/mi0bot/OpenHPSDR-Thetis
// Source file: ChannelMaster/aamix.c
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
//
// See AAMix.h for the full per-file GPL attribution + the locked
// byte-faithful Win32 retrofit posture + the allowed forced
// deviations (struct tag `AAMix`, Outbound field/setter as
// std::function) + the public-API contract notes ported verbatim
// from aamix.h:126-201.
//
// ============================================================
// BYTE-FAITHFUL WIN32 RETROFIT 2026-06-09
// (operator directive: no Lyra-native primitives in Thetis ports)
// ============================================================
//
// Per-function reference C code is inlined as block comments above
// each Lyra-cpp definition so a line-by-line comparison against
// `D:/sdrprojects/OpenHPSDR-Thetis-2.10.3.13/Project Files/Source/
// ChannelMaster/aamix.c` is one screen away.

#include "wdsp/AAMix.h"
#include "wdsp/Resample.h"
#include "wdsp_native.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>          // AvSetMmThreadCharacteristics / Priority
#include <intrin.h>        // _InterlockedAnd / _InterlockedOr /
                           // InterlockedBitTestAndSet / Reset
#include <process.h>       // _beginthread / _endthread

#include <cmath>
#include <cstdlib>         // calloc / free
#include <cstring>         // memcpy / memset

#pragma comment(lib, "avrt.lib")

namespace lyra::wdsp {

// Reference defines PI in cmcomm.h; the port uses the standard
// double-precision pi constant (byte-identical at the relevant
// precision since the reference's PI also evaluates to the same
// IEEE-754 double).
namespace { constexpr double kPI = 3.14159265358979323846; }

// `sizeof(complex)` in the reference is `2 * sizeof(double)` (16
// bytes).  The sibling ILV.h retrofit publishes `kSizeofComplex`
// as an inline constexpr in lyra::wdsp at namespace scope, but
// AAMix.h does NOT (re-)declare it to avoid an MSVC C2374 against
// the sibling header when both AAMix.h and ILV.h reach the same
// translation unit through a common consumer (CMaster.cpp).
// Re-aliased here at file scope so the AAMix.cpp body keeps the
// reference's `sizeof (complex)` byte-count pattern verbatim.
namespace { constexpr std::size_t kSizeofComplex = 2 * sizeof(double); }

// Forward declaration so create_aamix can call start_mixthread
// (defined further down in the file alongside the other threading
// bodies).  xaamix is already declared in AAMix.h (public API);
// flush_mix_ring is declared with its definition.
static void start_mixthread(AAMix* a);
static void flush_mix_ring(AAMix* a, int stream);

// =========================================================================
// Reference aamix.c:30 — central bank.
//
//   __declspec (align (16)) AAMIX paamix[MAX_EXT_AAMIX];
//
// Lyra-cpp: raw C array of pointers (matches reference indexing
// byte-for-byte).  `alignas(16)` is the C++ equivalent of the
// reference's `__declspec(align(16))`.
// =========================================================================
alignas(16) AAMix* paamix[MAX_EXT_AAMIX] = { nullptr, nullptr,
                                             nullptr, nullptr };

// =========================================================================
// mix_main + start_mixthread — reference aamix.c:32-55.
// =========================================================================
//
// void mix_main (void *pargs)
// {
//     DWORD taskIndex = 0;
//     HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"),
//                                                  &taskIndex);
//     if (hTask != 0) AvSetMmThreadPriority(hTask, 2);
//     else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
//
//     AAMIX a = (AAMIX) pargs;
//
//     while (_InterlockedAnd (&a->run, 1))
//     {
//         WaitForMultipleObjects (a->nactive, a->Aready, TRUE, INFINITE);
//         xaamix (a);
//         (*a->Outbound)(a->outbound_id, a->outsize, a->out);
//         // WriteAudio (30.0, 48000, a->outsize, a->out, 3);
//     }
//     _endthread();
// }
static void mix_main(void* pargs)
{
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"),
                                                &taskIndex);
    if (hTask != 0) AvSetMmThreadPriority(hTask, AVRT_PRIORITY_HIGH);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    AAMix* a = (AAMix*) pargs;

    while (_InterlockedAnd((volatile long*)&a->run, 1))
    {
        WaitForMultipleObjects(a->nactive, a->Aready, TRUE, INFINITE);
        xaamix(a);
        a->Outbound(a->outbound_id, a->outsize, a->out);
        // WriteAudio (30.0, 48000, a->outsize, a->out, 3);
    }
    _endthread();
}

// void start_mixthread (AAMIX a)
// {
//     HANDLE handle = (HANDLE) _beginthread(mix_main, 0, (void *)a);
//     //SetThreadPriority (handle, THREAD_PRIORITY_HIGHEST);
// }
static void start_mixthread(AAMix* a)
{
    HANDLE handle = (HANDLE) _beginthread(mix_main, 0, (void*) a);
    //SetThreadPriority (handle, THREAD_PRIORITY_HIGHEST);
    a->mix_thread = handle;
}

// Reference aamix.c:57-67 — slew state machine values.
//
//   enum _slew { BEGIN=0, DELAYUP, UPSLEW, ON, DELAYDOWN, DOWNSLEW,
//                ZERO, OFF };
//
// Lyra-cpp uses plain ints (matching the reference's `int ustate;
// int dstate;` storage in the slew struct), so an unscoped enum at
// file scope is the byte-faithful translation.
enum _slew {
    BEGIN = 0,
    DELAYUP,
    UPSLEW,
    ON,
    DELAYDOWN,
    DOWNSLEW,
    ZERO,
    OFF
};

// =========================================================================
// Slew helpers — reference aamix.c:69-125.
// =========================================================================
//
// void create_aaslew (AAMIX a)
// {
//     int i;
//     double delta, theta;
//     a->slew.ustate = BEGIN;
//     a->slew.dstate = BEGIN;
//     a->slew.ucount = 0;
//     a->slew.dcount = 0;
//     a->slew.ndelup = (int)(a->slew.tdelayup * a->outrate);
//     a->slew.ndeldown = (int)(a->slew.tdelaydown * a->outrate);
//     a->slew.ntup = (int)(a->slew.tslewup * a->outrate);
//     a->slew.ntdown = (int)(a->slew.tslewdown * a->outrate);
//     a->slew.cup   = (double *) malloc0 ((a->slew.ntup + 1) * sizeof (double));
//     a->slew.cdown = (double *) malloc0 ((a->slew.ntdown + 1) * sizeof (double));
//
//     delta = PI / (double)a->slew.ntup;
//     theta = 0.0;
//     for (i = 0; i <= a->slew.ntup; i++)
//     {
//         a->slew.cup[i] = 0.5 * (1.0 - cos (theta));
//         theta += delta;
//     }
//
//     delta = PI / (double)a->slew.ntdown;
//     theta = 0.0;
//     for (i = 0; i <= a->slew.ntdown; i++)
//     {
//         a->slew.cdown[i] = 0.5 * (1 + cos (theta));
//         theta += delta;
//     }
//
//     a->slew.uwait = CreateSemaphore (0, 0, 1, 0);
//     a->slew.dwait = CreateSemaphore (0, 0, 1, 0);
//     InterlockedBitTestAndReset (&a->slew.uflag, 0);
//     InterlockedBitTestAndReset (&a->slew.dflag, 0);
//
//     a->slew.utimeout = (int)(1000.0 * (a->slew.tdelayup   + a->slew.tslewup  )) + 2;
//     a->slew.dtimeout = (int)(1000.0 * (a->slew.tdelaydown + a->slew.tslewdown)) + 2;
// }
static void create_aaslew(AAMix* a)
{
    int i;
    double delta, theta;
    a->slew.ustate = BEGIN;
    a->slew.dstate = BEGIN;
    a->slew.ucount = 0;
    a->slew.dcount = 0;
    a->slew.ndelup = (int)(a->slew.tdelayup * a->outrate);
    a->slew.ndeldown = (int)(a->slew.tdelaydown * a->outrate);
    a->slew.ntup = (int)(a->slew.tslewup * a->outrate);
    a->slew.ntdown = (int)(a->slew.tslewdown * a->outrate);
    a->slew.cup   = (double*) calloc((size_t)(a->slew.ntup + 1),
                                     sizeof(double));
    a->slew.cdown = (double*) calloc((size_t)(a->slew.ntdown + 1),
                                     sizeof(double));

    delta = kPI / (double) a->slew.ntup;
    theta = 0.0;
    for (i = 0; i <= a->slew.ntup; i++)
    {
        a->slew.cup[i] = 0.5 * (1.0 - cos(theta));
        theta += delta;
    }

    delta = kPI / (double) a->slew.ntdown;
    theta = 0.0;
    for (i = 0; i <= a->slew.ntdown; i++)
    {
        a->slew.cdown[i] = 0.5 * (1 + cos(theta));
        theta += delta;
    }

    a->slew.uwait = CreateSemaphore(nullptr, 0, 1, nullptr);
    a->slew.dwait = CreateSemaphore(nullptr, 0, 1, nullptr);
    InterlockedBitTestAndReset(&a->slew.uflag, 0);
    InterlockedBitTestAndReset(&a->slew.dflag, 0);

    a->slew.utimeout = (int)(1000.0 * (a->slew.tdelayup   + a->slew.tslewup  )) + 2;
    a->slew.dtimeout = (int)(1000.0 * (a->slew.tdelaydown + a->slew.tslewdown)) + 2;
}

// void destroy_aaslew (AAMIX a)
// {
//     CloseHandle (a->slew.dwait);
//     CloseHandle (a->slew.uwait);
//     _aligned_free (a->slew.cdown);
//     _aligned_free (a->slew.cup);
// }
static void destroy_aaslew(AAMix* a)
{
    CloseHandle(a->slew.dwait);
    CloseHandle(a->slew.uwait);
    free(a->slew.cdown);
    free(a->slew.cup);
}

// void flush_aaslew (AAMIX a)
// {
//     a->slew.ustate = BEGIN;
//     a->slew.dstate = BEGIN;
//     a->slew.ucount = 0;
//     a->slew.dcount = 0;
//     InterlockedBitTestAndReset (&a->slew.uflag, 0);
//     InterlockedBitTestAndReset (&a->slew.dflag, 0);
// }
[[maybe_unused]] static void flush_aaslew(AAMix* a)
{
    a->slew.ustate = BEGIN;
    a->slew.dstate = BEGIN;
    a->slew.ucount = 0;
    a->slew.dcount = 0;
    InterlockedBitTestAndReset(&a->slew.uflag, 0);
    InterlockedBitTestAndReset(&a->slew.dflag, 0);
}

// =========================================================================
// create_aamix — reference aamix.c:128-207.
// =========================================================================
//
// void* create_aamix ( int id, int outbound_id, int ringinsize,
//                     int outsize, int ninputs, long active, long what,
//                     double volume, int ring_size, int* inrates,
//                     int outrate,
//                     void (*Outbound)(int id, int nsamples, double* buff),
//                     double tdelayup, double tslewup,
//                     double tdelaydown, double tslewdown )
// {
//     int i;
//     AAMIX a = (AAMIX) malloc0 (sizeof (aamix));
//     a->id = id;
//     a->outbound_id = outbound_id;
//     a->ringinsize = ringinsize;
//     a->outsize = outsize;
//     a->ninputs = ninputs;
//     a->active = active;
//     a->what = what;
//     a->volume = volume;
//     a->rsize = ring_size;
//     a->outrate = outrate;
//     a->Outbound = Outbound;
//     a->slew.tdelayup = tdelayup;
//     a->slew.tslewup = tslewup;
//     a->slew.tdelaydown = tdelaydown;
//     a->slew.tslewdown = tslewdown;
//     for (i = 0; i < a->ninputs; i++)
//         a->ring[i] = (double*) malloc0 (a->rsize * sizeof (complex));
//     a->out = (double*) malloc0 (a->outsize * sizeof (complex));
//     a->nactive = 0;
//     for (i = 0; i < a->ninputs; i++)
//     {
//         a->vol[i]           = 1.0;
//         a->tvol[i]          = a->volume;
//         a->inidx[i]         = 0;
//         a->outidx[i]        = 0;
//         a->unqueuedsamps[i] = 0;
//         a->Ready[i] = CreateSemaphore (0, 0, 1000, 0);
//         InitializeCriticalSectionAndSpinCount(&a->cs_in[i], 2500);
//         if (_InterlockedAnd(&a->active, 0xffffffff) & (1 << i))
//         {
//             a->Aready[a->nactive++] = a->Ready[i];
//             InterlockedBitTestAndSet (&a->accept[i], 0);
//         }
//         else
//             InterlockedBitTestAndReset (&a->accept[i], 0);
//     }
//     for (i = 0; i < a->ninputs; i++)  // resamplers
//     {
//         int run, size;
//         a->inrate[i] = inrates[i];
//         if (a->inrate[i] != a->outrate) run = 1;
//         else                            run = 0;
//         if (a->inrate[i] > a->outrate)
//             size = a->ringinsize * (a->inrate[i] / a->outrate);
//         else
//             size = a->ringinsize / (a->outrate / a->inrate[i]);
//         a->resampbuff[i] = (double *) malloc0 (a->ringinsize * sizeof (complex));
//         a->rsmp[i] = create_resample (run, size, 0, a->resampbuff[i],
//                                       a->inrate[i], a->outrate, 0.0, 0, 1.0);
//     }
//     InitializeCriticalSectionAndSpinCount(&a->cs_out, 2500);
//     create_aaslew (a);
//     InterlockedBitTestAndSet (&a->run, 0);
//     if (a->nactive) start_mixthread (a);
//     if (a->id >= 0) paamix[id] = a;
//     return (void *)a;
// }
AAMix* create_aamix(
    lyra::dsp::WdspNative& wdsp,
    int id,
    int outbound_id,
    int ringinsize,
    int outsize,
    int ninputs,
    long active,
    long what,
    double volume,
    int ring_size,
    int* inrates,
    int outrate,
    std::function<void(int id, int nsamples, double* buff)> Outbound,
    double tdelayup,
    double tslewup,
    double tdelaydown,
    double tslewdown)
{
    int i;
    AAMix* a = (AAMix*) calloc(1, sizeof(AAMix));
    // [Lyra-native] DLL handle plumbing — single-field divergence.
    a->wdsp = &wdsp;
    a->id = id;
    a->outbound_id = outbound_id;
    a->ringinsize = ringinsize;
    a->outsize = outsize;
    a->ninputs = ninputs;
    a->active = active;
    a->what = what;
    a->volume = volume;
    a->rsize = ring_size;
    a->outrate = outrate;
    a->Outbound = std::move(Outbound);
    a->slew.tdelayup = tdelayup;
    a->slew.tslewup = tslewup;
    a->slew.tdelaydown = tdelaydown;
    a->slew.tslewdown = tslewdown;
    for (i = 0; i < a->ninputs; i++)
        a->ring[i] = (double*) calloc((size_t) a->rsize, kSizeofComplex);
    a->out = (double*) calloc((size_t) a->outsize, kSizeofComplex);
    a->nactive = 0;
    for (i = 0; i < a->ninputs; i++)
    {
        a->vol[i]           = 1.0;
        a->tvol[i]          = a->volume;
        a->inidx[i]         = 0;
        a->outidx[i]        = 0;
        a->unqueuedsamps[i] = 0;
        a->Ready[i] = CreateSemaphore(nullptr, 0, 1000, nullptr);
        InitializeCriticalSectionAndSpinCount(&a->cs_in[i], 2500);
        if (_InterlockedAnd(&a->active, 0xffffffff) & (1 << i))
        {
            a->Aready[a->nactive++] = a->Ready[i];
            InterlockedBitTestAndSet(&a->accept[i], 0);
        }
        else
            InterlockedBitTestAndReset(&a->accept[i], 0);
    }
    const lyra::dsp::WdspApi& api = wdsp.api();
    for (i = 0; i < a->ninputs; i++)   // resamplers
    {
        int run, size;
        a->inrate[i] = inrates[i];
        // inrate & outrate must be related by an integer multiple or sub-multiple
        if (a->inrate[i] != a->outrate) run = 1;
        else                            run = 0;
        if (a->inrate[i] > a->outrate)
            size = a->ringinsize * (a->inrate[i] / a->outrate);
        else
            size = a->ringinsize / (a->outrate / a->inrate[i]);
        a->resampbuff[i] = (double*) calloc((size_t) a->ringinsize,
                                            kSizeofComplex);
        a->rsmp[i] = api.create_resample(run, size, nullptr,
                                         a->resampbuff[i],
                                         a->inrate[i], a->outrate,
                                         0.0, 0, 1.0);
    }
    InitializeCriticalSectionAndSpinCount(&a->cs_out, 2500);
    // slew
    create_aaslew(a);
    // slew_end
    InterlockedBitTestAndSet(&a->run, 0);
    if (a->nactive) start_mixthread(a);
    if (a->id >= 0) paamix[id] = a;
    return a;
}

// =========================================================================
// destroy_aamix — reference aamix.c:209-234.
// =========================================================================
//
// void destroy_aamix (void* ptr, int id)
// {
//     int i;
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     InterlockedBitTestAndReset (&a->run, 0);
//     for (i = 0; i < a->ninputs; i++)
//         ReleaseSemaphore (a->Ready[i], 1, 0);
//     Sleep (2);
//     DeleteCriticalSection (&a->cs_out);
//     for (i = 0; i < a->ninputs; i++)
//     {
//         destroy_resample (a->rsmp[i]);
//         _aligned_free (a->resampbuff[i]);
//         DeleteCriticalSection (&a->cs_in[i]);
//         CloseHandle (a->Ready[i]);
//     }
//     _aligned_free (a->out);
//     for (i = 0; i < a->ninputs; i++)
//         _aligned_free (a->ring[i]);
//     destroy_aaslew (a);
//     _aligned_free (a);
// }
void destroy_aamix(AAMix* ptr, int id)
{
    int i;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    InterlockedBitTestAndReset(&a->run, 0);
    for (i = 0; i < a->ninputs; i++)
        ReleaseSemaphore(a->Ready[i], 1, nullptr);
    Sleep(2);
    DeleteCriticalSection(&a->cs_out);
    for (i = 0; i < a->ninputs; i++)
    {
        a->wdsp->api().destroy_resample(a->rsmp[i]);
        free(a->resampbuff[i]);
        DeleteCriticalSection(&a->cs_in[i]);
        CloseHandle(a->Ready[i]);
    }
    free(a->out);
    for (i = 0; i < a->ninputs; i++)
        free(a->ring[i]);
    // slew
    destroy_aaslew(a);
    // slew_end
    // [Lyra-native] central-bank slot null on teardown so a
    // subsequent destroy_aamix(nullptr, id) is a safe no-op.  The
    // reference does NOT null paamix[id] here; this is the one
    // safety carve-out beyond a literal port (callers in Lyra-cpp
    // re-create + destroy across RX restarts and the reference's
    // implicit "you must not call destroy twice" contract is
    // fragile here).
    if (a->id >= 0 && a->id < MAX_EXT_AAMIX && paamix[a->id] == a)
        paamix[a->id] = nullptr;
    free(a);
}

// =========================================================================
// xMixAudio — reference aamix.c:236-278.
// =========================================================================
//
// // loads data from a buffer into an audio mixer ring
// void xMixAudio (void* ptr, int id, int stream, double* data)
// {
//     int first, second, n;
//     double* indata;
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     if (_InterlockedAnd (&a->accept[stream], 1))
//     {
//         EnterCriticalSection (&a->cs_in[stream]);
//         if (a->rsmp[stream]->run)
//         {
//             a->rsmp[stream]->in = data;
//             indata = a->resampbuff[stream];
//             xresample (a->rsmp[stream]);
//         }
//         else
//             indata = data;
//         if (a->ringinsize > (a->rsize - a->inidx[stream]))
//         {
//             first = a->rsize - a->inidx[stream];
//             second = a->ringinsize - first;
//         }
//         else
//         {
//             first = a->ringinsize;
//             second = 0;
//         }
//         memcpy (a->ring[stream] + 2 * a->inidx[stream], indata,
//                 first  * sizeof (complex));
//         memcpy (a->ring[stream],                       indata + 2 * first,
//                 second * sizeof (complex));
//
//         if ((a->unqueuedsamps[stream] += a->ringinsize) >= a->outsize)
//         {
//             n = a->unqueuedsamps[stream] / a->outsize;
//             ReleaseSemaphore (a->Ready[stream], n, 0);
//             a->unqueuedsamps[stream] -= n * a->outsize;
//         }
//         if ((a->inidx[stream] += a->ringinsize) >= a->rsize)
//             a->inidx[stream] -= a->rsize;
//         LeaveCriticalSection (&a->cs_in[stream]);
//     }
// }
void xMixAudio(AAMix* ptr, int id, int stream, double* data)
{
    int first, second, n;
    double* indata;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    if (_InterlockedAnd(&a->accept[stream], 1))
    {
        EnterCriticalSection(&a->cs_in[stream]);
        resample* rsmp = (resample*) a->rsmp[stream];
        if (rsmp->run)
        {
            rsmp->in = data;
            indata = a->resampbuff[stream];
            a->wdsp->api().xresample(rsmp);
        }
        else
            indata = data;
        if (a->ringinsize > (a->rsize - a->inidx[stream]))
        {
            first = a->rsize - a->inidx[stream];
            second = a->ringinsize - first;
        }
        else
        {
            first = a->ringinsize;
            second = 0;
        }
        memcpy(a->ring[stream] + 2 * a->inidx[stream], indata,
               (size_t) first  * kSizeofComplex);
        memcpy(a->ring[stream],                       indata + 2 * first,
               (size_t) second * kSizeofComplex);

        if ((a->unqueuedsamps[stream] += a->ringinsize) >= a->outsize)
        {
            n = a->unqueuedsamps[stream] / a->outsize;
            ReleaseSemaphore(a->Ready[stream], n, nullptr);
            a->unqueuedsamps[stream] -= n * a->outsize;
        }
        if ((a->inidx[stream] += a->ringinsize) >= a->rsize)
            a->inidx[stream] -= a->rsize;
        LeaveCriticalSection(&a->cs_in[stream]);
    }
}

// =========================================================================
// upslew — reference aamix.c:280-343.
// =========================================================================
//
// void upslew (AAMIX a)
// {
//     int i;
//     double I, Q;
//     double *pin  = a->out;
//     double *pout = a->out;
//     for (i = 0; i < a->outsize; i++)
//     {
//         I = pin[2 * i + 0];
//         Q = pin[2 * i + 1];
//         switch (a->slew.ustate)
//             {
//             case BEGIN: ...
//             case DELAYUP: ...
//             case UPSLEW: ...
//             case ON: ...
//             }
//     }
// }
static void upslew(AAMix* a)
{
    int i;
    double I, Q;
    double* pin  = a->out;
    double* pout = a->out;
    for (i = 0; i < a->outsize; i++)
    {
        I = pin[2 * i + 0];
        Q = pin[2 * i + 1];
        switch (a->slew.ustate)
            {
            case BEGIN:
                pout[2 * i + 0] = 0.0;
                pout[2 * i + 1] = 0.0;
                if ((I != 0.0) || (Q != 0.0))
                {
                    if (a->slew.ndelup > 0)
                    {
                        a->slew.ustate = DELAYUP;
                        a->slew.ucount = a->slew.ndelup;
                    }
                    else if (a->slew.ntup > 0)
                    {
                        a->slew.ustate = UPSLEW;
                        a->slew.ucount = a->slew.ntup;
                    }
                    else
                        a->slew.ustate = ON;
                }
                break;
            case DELAYUP:
                pout[2 * i + 0] = 0.0;
                pout[2 * i + 1] = 0.0;
                if (a->slew.ucount-- == 0)
                {
                    if (a->slew.ntup > 0)
                    {
                        a->slew.ustate = UPSLEW;
                        a->slew.ucount = a->slew.ntup;
                    }
                    else
                        a->slew.ustate = ON;
                }
                break;
            case UPSLEW:
                pout[2 * i + 0] = I * a->slew.cup[a->slew.ntup - a->slew.ucount];
                pout[2 * i + 1] = Q * a->slew.cup[a->slew.ntup - a->slew.ucount];
                if (a->slew.ucount-- == 0)
                    a->slew.ustate = ON;
                break;
            case ON:
                pout[2 * i + 0] = I;
                pout[2 * i + 1] = Q;
                if (i == a->outsize - 1)
                {
                    a->slew.ustate = BEGIN;
                    InterlockedBitTestAndReset(&a->slew.uflag, 0);
                    ReleaseSemaphore(a->slew.uwait, 1, nullptr);
                }
                break;
            }
    }
}

// =========================================================================
// downslew — reference aamix.c:345-420.
// =========================================================================
//
// void downslew (AAMIX a)
// {
//     int i;
//     double I, Q;
//     double *pin  = a->out;
//     double *pout = a->out;
//     for (i = 0; i < a->outsize; i++)
//     {
//         I = pin[2 * i + 0];
//         Q = pin[2 * i + 1];
//         switch (a->slew.dstate)
//             {
//             case BEGIN: ...
//             case DELAYDOWN: ...
//             case DOWNSLEW: ...
//             case ZERO: ...
//             case OFF: ...
//             }
//     }
// }
static void downslew(AAMix* a)
{
    int i;
    double I, Q;
    double* pin  = a->out;
    double* pout = a->out;
    for (i = 0; i < a->outsize; i++)
    {
        I = pin[2 * i + 0];
        Q = pin[2 * i + 1];
        switch (a->slew.dstate)
            {
            case BEGIN:
                pout[2 * i + 0] = I;
                pout[2 * i + 1] = Q;
                if (a->slew.ndeldown > 0)
                {
                    a->slew.dstate = DELAYDOWN;
                    a->slew.dcount = a->slew.ndeldown;
                }
                else if (a->slew.ntdown > 0)
                {
                    a->slew.dstate = DOWNSLEW;
                    a->slew.dcount = a->slew.ntdown;
                }
                else
                {
                    a->slew.dstate = ZERO;
                    a->slew.dcount = a->outsize;
                }
                break;
            case DELAYDOWN:
                pout[2 * i + 0] = I;
                pout[2 * i + 1] = Q;
                if (a->slew.dcount-- == 0)
                {
                    if (a->slew.ntdown > 0)
                    {
                        a->slew.dstate = DOWNSLEW;
                        a->slew.dcount = a->slew.ntdown;
                    }
                    else
                    {
                        a->slew.dstate = ZERO;
                        a->slew.dcount = a->outsize;
                    }
                }
                break;
            case DOWNSLEW:
                pout[2 * i + 0] = I * a->slew.cdown[a->slew.ntdown - a->slew.dcount];
                pout[2 * i + 1] = Q * a->slew.cdown[a->slew.ntdown - a->slew.dcount];
                if (a->slew.dcount-- == 0)
                {
                    a->slew.dstate = ZERO;
                    a->slew.dcount = a->outsize;
                }
                break;
            case ZERO:
                pout[2 * i + 0] = 0.0;
                pout[2 * i + 1] = 0.0;
                if (a->slew.dcount-- == 0)
                    a->slew.dstate = OFF;
                break;
            case OFF:
                pout[2 * i + 0] = 0.0;
                pout[2 * i + 1] = 0.0;
                if (i == a->outsize - 1)
                {
                    a->slew.dstate = BEGIN;
                    InterlockedBitTestAndReset(&a->slew.dflag, 0);
                    ReleaseSemaphore(a->slew.dwait, 1, nullptr);
                }
                break;
            }
    }
}

// =========================================================================
// xaamix — reference aamix.c:422-459.
// =========================================================================
//
// // pulls data from audio rings and mixes with output
// void xaamix (AAMIX a)
// {
//     int i, j;
//     int what, mask, idx;
//     EnterCriticalSection(&a->cs_out);
//     if (!_InterlockedAnd (&a->run, 1))
//     {
//         LeaveCriticalSection (&a->cs_out);
//         _endthread();
//         return; //MW0LGE_21j
//     }
//     memset (a->out, 0, a->outsize * sizeof (complex));
//     what = _InterlockedAnd(&a->what, 0xffffffff) & _InterlockedAnd(&a->active, 0xffffffff);
//     i = 0;
//     while (what != 0)
//     {
//         mask = 1 << i;
//         if ((mask & what) != 0)
//         {
//             idx = a->outidx[i];
//             for (j = 0; j < a->outsize; j++)
//             {
//                 a->out[2 * j + 0] += a->tvol[i] * a->ring[i][2 * idx + 0];
//                 a->out[2 * j + 1] += a->tvol[i] * a->ring[i][2 * idx + 1];
//                 if (++idx == a->rsize) idx = 0;
//             }
//             what &= ~mask;
//         }
//         i++;
//     }
//     for (i = 0; i < a->ninputs; i++)
//         if (_InterlockedAnd (&a->accept[i], 1))
//             if ((a->outidx[i] += a->outsize) >= a->rsize) a->outidx[i] -= a->rsize;
//     if (_InterlockedAnd (&a->slew.uflag, 1)) upslew   (a);
//     if (_InterlockedAnd (&a->slew.dflag, 1)) downslew (a);
//     LeaveCriticalSection(&a->cs_out);
// }
void xaamix(AAMix* a)
{
    int i, j;
    int what, mask, idx;
    EnterCriticalSection(&a->cs_out);
    if (!_InterlockedAnd((volatile long*)&a->run, 1))
    {
        LeaveCriticalSection(&a->cs_out);
        _endthread();
        return; //MW0LGE_21j
    }
    memset(a->out, 0, (size_t) a->outsize * kSizeofComplex);
    what = _InterlockedAnd(&a->what, 0xffffffff) & _InterlockedAnd(&a->active, 0xffffffff);
    i = 0;
    while (what != 0)
    {
        mask = 1 << i;
        if ((mask & what) != 0)
        {
            idx = a->outidx[i];
            for (j = 0; j < a->outsize; j++)
            {
                a->out[2 * j + 0] += a->tvol[i] * a->ring[i][2 * idx + 0];
                a->out[2 * j + 1] += a->tvol[i] * a->ring[i][2 * idx + 1];
                if (++idx == a->rsize) idx = 0;
            }
            what &= ~mask;
        }
        i++;
    }
    for (i = 0; i < a->ninputs; i++)
        if (_InterlockedAnd(&a->accept[i], 1))
            if ((a->outidx[i] += a->outsize) >= a->rsize) a->outidx[i] -= a->rsize;
    if (_InterlockedAnd(&a->slew.uflag, 1)) upslew  (a);
    if (_InterlockedAnd(&a->slew.dflag, 1)) downslew(a);
    LeaveCriticalSection(&a->cs_out);
}

// =========================================================================
// flush_mix_ring — reference aamix.c:461-469.
// =========================================================================
//
// void flush_mix_ring (AAMIX a, int stream)
// {
//     memset (a->ring[stream], 0, a->rsize * sizeof (complex));
//     a->inidx [stream] = 0;
//     a->outidx[stream] = 0;
//     a->unqueuedsamps[stream] = 0;
//     while (!WaitForSingleObject (a->Ready[stream], 1)) ;
//     flush_resample (a->rsmp[stream]);
// }
static void flush_mix_ring(AAMix* a, int stream)
{
    memset(a->ring[stream], 0, (size_t) a->rsize * kSizeofComplex);
    a->inidx [stream] = 0;
    a->outidx[stream] = 0;
    a->unqueuedsamps[stream] = 0;
    while (!WaitForSingleObject(a->Ready[stream], 1)) ;
    a->wdsp->api().flush_resample(a->rsmp[stream]);
}

// =========================================================================
// close_mixer — reference aamix.c:471-491.
// =========================================================================
//
// void close_mixer (AAMIX a)
// {
//     int i;
//     InterlockedBitTestAndSet (&a->slew.dflag, 0);
//     WaitForSingleObject (a->slew.dwait, a->slew.dtimeout);
//     InterlockedBitTestAndReset (&a->slew.dflag, 0);
//     for (i = 0; i < a->ninputs; i++)
//         InterlockedBitTestAndReset(&a->accept[i], 0);
//     Sleep(1);
//     for (i = 0; i < a->ninputs; i++)
//         EnterCriticalSection (&a->cs_in[i]);
//     EnterCriticalSection (&a->cs_out);
//     Sleep (25);
//     InterlockedBitTestAndReset(&a->run, 0);
//     for (i = 0; i < a->ninputs; i++)
//         ReleaseSemaphore(a->Ready[i], 1, 0);
//     LeaveCriticalSection (&a->cs_out);
//     Sleep (2);
//     for (i = 0; i < a->ninputs; i++)
//         flush_mix_ring (a, i);
// }
static void close_mixer(AAMix* a)
{
    int i;
    InterlockedBitTestAndSet(&a->slew.dflag, 0);        // set a bit telling downslew to proceed
    WaitForSingleObject(a->slew.dwait, a->slew.dtimeout); // block until downslew is complete or timeout if data is not flowing
    InterlockedBitTestAndReset(&a->slew.dflag, 0);
    for (i = 0; i < a->ninputs; i++)
        InterlockedBitTestAndReset(&a->accept[i], 0);   // shut the gates to prevent new infusions
    Sleep(1);                                           // if a thread has just passed the gate, allow time to get cs_in and get through
    for (i = 0; i < a->ninputs; i++)
        EnterCriticalSection(&a->cs_in[i]);             // wait until the current infusions are all finished
    EnterCriticalSection(&a->cs_out);                   // block the mixer thread at the beginning of xaamix()
    Sleep(25);                                          // wait for thread to arrive at the top of the main() loop
    InterlockedBitTestAndReset(&a->run, 0);             // set a trap for the mixer thread
    for (i = 0; i < a->ninputs; i++)
        ReleaseSemaphore(a->Ready[i], 1, nullptr);      // be sure the mixer thread can pass WaitForMultipleObjects in main()
    LeaveCriticalSection(&a->cs_out);                   // let the thread pass to the trap in xaamix()
    Sleep(2);                                           // wait for the mixer thread to die
    for (i = 0; i < a->ninputs; i++)
        flush_mix_ring(a, i);                           // restore rings to pristine condition
}

// =========================================================================
// open_mixer — reference aamix.c:493-505.
// =========================================================================
//
// void open_mixer (AAMIX a)
// {
//     int i;
//     InterlockedBitTestAndSet (&a->slew.uflag, 0);
//     InterlockedBitTestAndSet(&a->run,0);
//     if (a->nactive) start_mixthread (a);
//     for (i = a->ninputs - 1; i >= 0; i--)
//         LeaveCriticalSection (&a->cs_in[i]);
//     for (i = a->ninputs - 1; i >= 0; i--)
//         if (_InterlockedAnd (&a->active, 0xffffffff) & (1 << i))
//             InterlockedBitTestAndSet(&a->accept[i], 0);
//     WaitForSingleObject (a->slew.uwait, a->slew.utimeout);
// }
static void open_mixer(AAMix* a)
{
    int i;
    InterlockedBitTestAndSet(&a->slew.uflag, 0);        // set a bit telling upslew to proceed (when there are samples flowing)
    InterlockedBitTestAndSet(&a->run, 0);               // remove the mixer thread trap
    if (a->nactive) start_mixthread(a);                 // start the mixer thread if there's anything to mix
    for (i = a->ninputs - 1; i >= 0; i--)
        LeaveCriticalSection(&a->cs_in[i]);             // enable xMixAudio() processing
    for (i = a->ninputs - 1; i >= 0; i--)
        if (_InterlockedAnd(&a->active, 0xffffffff) & (1 << i))
            InterlockedBitTestAndSet(&a->accept[i], 0); // open the xMixAudio() gates for active streams
    WaitForSingleObject(a->slew.uwait, a->slew.utimeout); // block on semaphore until upslew complete, or until timeout if no data flow
}

/********************************************************************************************************
*                                                                                                       *
*                                         MIXER PROPERTIES                                              *
*                                                                                                       *
********************************************************************************************************/

// Reference aamix.c:513-519 — SetAAudioMixOutputPointer.
//
// void SetAAudioMixOutputPointer(void* ptr, int id,
//                                void (*Outbound)(int id, int nsamples, double* buff))
// {
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     a->Outbound = Outbound;
// }
//
// ALLOWED FORCED DEVIATION (rule #8 caller-protection): Outbound is
// kept as std::function so existing callers (CMaster.cpp +
// wdsp_engine.cpp) can pass lambdas with captures.
void SetAAudioMixOutputPointer(
    AAMix* ptr, int id,
    std::function<void(int id, int nsamples, double* buff)> Outbound)
{
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    a->Outbound = std::move(Outbound);
}

// Reference aamix.c:521-546 — SetAAudioMixState.
//
// PORT
// void SetAAudioMixState (void* ptr, int id, int stream, int state)
// {
//     int i;
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     if (((_InterlockedAnd(&a->active, 0xffffffff) >> stream) & 1) != state)
//     {
//         close_mixer(a);
//         if (state)
//             _InterlockedOr(&a->active, 1 << stream);
//         else
//             _InterlockedAnd(&a->active, ~(1 << stream));
//         a->nactive = 0;
//         for (i = 0; i < a->ninputs; i++)
//             if (_InterlockedAnd(&a->active, 0xffffffff) & (1 << i))
//             {
//                 a->Aready[a->nactive++] = a->Ready[i];
//                 InterlockedBitTestAndSet(&a->accept[i], 0);
//             }
//             else
//                 InterlockedBitTestAndReset(&a->accept[i], 0);
//         open_mixer(a);
//     }
// }
void SetAAudioMixState(AAMix* ptr, int id, int stream, int state)
{
    int i;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    if (((_InterlockedAnd(&a->active, 0xffffffff) >> stream) & 1) != state)
    {
        close_mixer(a);
        if (state)
            _InterlockedOr(&a->active, 1 << stream);                // set stream active
        else
            _InterlockedAnd(&a->active, ~(1 << stream));            // set stream inactive
        a->nactive = 0;
        for (i = 0; i < a->ninputs; i++)
            if (_InterlockedAnd(&a->active, 0xffffffff) & (1 << i))
            {
                a->Aready[a->nactive++] = a->Ready[i];
                InterlockedBitTestAndSet(&a->accept[i], 0);
            }
            else
                InterlockedBitTestAndReset(&a->accept[i], 0);
        open_mixer(a);
    }
}

// Reference aamix.c:548-582 — SetAAudioMixStates.
//
// PORT
// void SetAAudioMixStates (void* ptr, int id, int streams, int states)
// { ... batched-set variant of SetAAudioMixState ... }
void SetAAudioMixStates(AAMix* ptr, int id, int streams, int states)
{
    int i;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    if ((_InterlockedAnd(&a->active, 0xffffffff) & streams) != (states & streams))
    {
        close_mixer(a);
        for (i = 0; i < a->ninputs; i++)
            if ((streams >> i) & 1)
                if ((states >> i) & 1)
                    _InterlockedOr(&a->active, 1 << i);             // set stream active
                else
                    _InterlockedAnd(&a->active, ~(1 << i));         // set stream inactive

        a->nactive = 0;
        for (i = 0; i < a->ninputs; i++)
            if (_InterlockedAnd(&a->active, 0xffffffff) & (1 << i))
            {
                a->Aready[a->nactive++] = a->Ready[i];
                InterlockedBitTestAndSet(&a->accept[i], 0);
            }
            else
                InterlockedBitTestAndReset(&a->accept[i], 0);
        open_mixer(a);
    }
}

// Reference aamix.c:584-594 — SetAAudioMixWhat.
//
// PORT
// void SetAAudioMixWhat (void* ptr, int id, int stream, int state)
// {
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     if (state) InterlockedBitTestAndSet   (&a->what, stream);
//     else       InterlockedBitTestAndReset (&a->what, stream);
// }
void SetAAudioMixWhat(AAMix* ptr, int id, int stream, int state)
{
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    if (state)
        InterlockedBitTestAndSet  (&a->what, stream);   // turn on mixing
    else
        InterlockedBitTestAndReset(&a->what, stream);   // turn off mixing
}

// Reference aamix.c:596-608 — SetAAudioMixVolume.
//
// PORT
// void SetAAudioMixVolume (void* ptr, int id, double volume)
// {
//     int i;
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     EnterCriticalSection(&a->cs_out);
//     a->volume = volume;
//     for (i = 0; i < 32; i++)
//         a->tvol[i] = a->volume * a->vol[i];
//     LeaveCriticalSection(&a->cs_out);
// }
void SetAAudioMixVolume(AAMix* ptr, int id, double volume)
{
    int i;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    EnterCriticalSection(&a->cs_out);
    a->volume = volume;
    for (i = 0; i < 32; i++)
        a->tvol[i] = a->volume * a->vol[i];
    LeaveCriticalSection(&a->cs_out);
}

// Reference aamix.c:610-620 — SetAAudioMixVol.
//
// PORT
// void SetAAudioMixVol (void* ptr, int id, int stream, double vol)
// {
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     EnterCriticalSection(&a->cs_out);
//     a->vol [stream] = vol;
//     a->tvol[stream] = a->vol[stream] * a->volume;
//     LeaveCriticalSection(&a->cs_out);
// }
void SetAAudioMixVol(AAMix* ptr, int id, int stream, double vol)
{
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    EnterCriticalSection(&a->cs_out);
    a->vol [stream] = vol;
    a->tvol[stream] = a->vol[stream] * a->volume;
    LeaveCriticalSection(&a->cs_out);
}

// Reference aamix.c:622-642 — SetAAudioRingInsize.
//
// void SetAAudioRingInsize (void* ptr, int id, int size)
// { ... close/edit/open atom; per-input rsmp->size + resampbuff
//   reallocation, mirrors aamix.c body ... }
void SetAAudioRingInsize(AAMix* ptr, int id, int size)
{
    int i, rs_size;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    close_mixer(a);
    a->ringinsize = size;
    for (i = 0; i < a->ninputs; i++)
    {   // inrate & outrate must be related by an integer multiple or sub-multiple
        if (a->inrate[i] > a->outrate)
            rs_size = a->ringinsize * (a->inrate[i] / a->outrate);
        else
            rs_size = a->ringinsize / (a->outrate / a->inrate[i]);
        ((resample*) a->rsmp[i])->size = rs_size;
        free(a->resampbuff[i]);
        a->resampbuff[i] = (double*) calloc((size_t) a->ringinsize,
                                            kSizeofComplex);
        ((resample*) a->rsmp[i])->out = a->resampbuff[i];
    }
    open_mixer(a);
}

// Reference aamix.c:644-654 — SetAAudioRingOutsize.
//
// void SetAAudioRingOutsize (void* ptr, int id, int size)
// {
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     close_mixer (a);
//     a->outsize = size;
//     _aligned_free (a->out);
//     a->out = (double*) malloc0 (a->outsize * sizeof (complex));
//     open_mixer (a);
// }
void SetAAudioRingOutsize(AAMix* ptr, int id, int size)
{
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    close_mixer(a);
    a->outsize = size;
    free(a->out);
    a->out = (double*) calloc((size_t) a->outsize, kSizeofComplex);
    open_mixer(a);
}

// Reference aamix.c:656-681 — SetAAudioOutRate.
//
// void SetAAudioOutRate (void* ptr, int id, int rate)
// { ... close/edit/open atom; per-input resampler destroyed + recreated
//   at new outrate, mirrors aamix.c body ... }
void SetAAudioOutRate(AAMix* ptr, int id, int rate)
{
    int i;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    close_mixer(a);
    a->outrate = rate;
    const lyra::dsp::WdspApi& api = a->wdsp->api();
    for (i = 0; i < a->ninputs; i++)   // resamplers
    {
        int run, size;
        api.destroy_resample(a->rsmp[i]);
        free(a->resampbuff[i]);
        // inrate & outrate must be related by an integer multiple or sub-multiple
        if (a->inrate[i] != a->outrate) run = 1;
        else                            run = 0;
        if (a->inrate[i] > a->outrate)
            size = a->ringinsize * (a->inrate[i] / a->outrate);
        else
            size = a->ringinsize / (a->outrate / a->inrate[i]);
        a->resampbuff[i] = (double*) calloc((size_t) a->ringinsize,
                                            kSizeofComplex);
        a->rsmp[i] = api.create_resample(run, size, nullptr,
                                         a->resampbuff[i],
                                         a->inrate[i], a->outrate,
                                         0.0, 0, 1.0);
        ((resample*) a->rsmp[i])->out = a->resampbuff[i];
    }
    open_mixer(a);
}

// Reference aamix.c:683-699 — SetAAudioStreamRate.
//
// void SetAAudioStreamRate (void* ptr, int id, int mixinid, int rate)
// {   // NOTE: you must set the stream state to INACTIVE before using this function!
//     int run, size;
//     AAMIX a;
//     if (ptr == 0)   a = paamix[id];
//     else            a = (AAMIX)ptr;
//     a->inrate[mixinid] = rate;
//     destroy_resample (a->rsmp[mixinid]);
//     if (a->inrate[mixinid] != a->outrate)   run = 1;
//     else                                    run = 0;
//     if (a->inrate[mixinid] > a->outrate)
//         size = a->ringinsize * (a->inrate[mixinid] / a->outrate);
//     else
//         size = a->ringinsize / (a->outrate / a->inrate[mixinid]);
//     a->rsmp[mixinid] = create_resample (run, size, 0,
//                                         a->resampbuff[mixinid],
//                                         a->inrate[mixinid],
//                                         a->outrate, 0.0, 0, 1.0);
// }
void SetAAudioStreamRate(AAMix* ptr, int id, int mixinid, int rate)
{   // NOTE: you must set the stream state to INACTIVE before using this function!
    int run, size;
    AAMix* a;
    if (ptr == nullptr) a = paamix[id];
    else                a = ptr;
    const lyra::dsp::WdspApi& api = a->wdsp->api();
    a->inrate[mixinid] = rate;
    api.destroy_resample(a->rsmp[mixinid]);
    // inrate & outrate must be related by an integer multiple or sub-multiple
    if (a->inrate[mixinid] != a->outrate)    run = 1;
    else                                     run = 0;
    if (a->inrate[mixinid] > a->outrate)
        size = a->ringinsize * (a->inrate[mixinid] / a->outrate);
    else
        size = a->ringinsize / (a->outrate / a->inrate[mixinid]);
    a->rsmp[mixinid] = api.create_resample(run, size, nullptr,
                                           a->resampbuff[mixinid],
                                           a->inrate[mixinid],
                                           a->outrate, 0.0, 0, 1.0);
}

} // namespace lyra::wdsp
