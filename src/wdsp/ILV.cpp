// Lyra-cpp — ILV.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream:    https://github.com/mi0bot/OpenHPSDR-Thetis
// Source file: ChannelMaster/ilv.c
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
//
// ============================================================
// BYTE-FAITHFUL WIN32 RETROFIT 2026-06-09
// (operator directive: no Lyra-native primitives in Thetis ports)
// ============================================================
//
// See ILV.h for the full per-file GPL attribution + retrofit
// rationale.  Reference C code is inlined as comments above each
// function so a reader can compare line-by-line.

#include "wdsp/ILV.h"

#include <intrin.h>
#include <stdlib.h>
#include <string.h>

namespace lyra::wdsp {

// ===== pilv[] central bank =====
// [Lyra-native] Defined here; the reference uses
// `pcm->xmtr[xmtr_id].pilv`.  Raw C array of pointers matching
// the reference indexing pattern.  Element type is `ILV*` per
// the LYRA-CPP NAMING note in ILV.h (semantics identical to the
// reference's `ILV` which IS a pointer under its twin typedef).
ILV* pilv[MAX_EXT_ILV] = { nullptr, nullptr };

// ilv is a simple SYNCHRONOUS interleaver:  it assumes all
// inputs are available when it is called.
//   (verbatim reference comment, ilv.c:29)

// ===== create_ilv =====
//
// Reference ilv.c:31-49 (verbatim):
//
//   ILV create_ilv (
//       int run,
//       int outbound_id,
//       int insize,
//       int ninputs,
//       long what,
//       void (*Outbound) (int id, int nsamples, double* buff)
//       )
//   {
//       ILV a = (ILV) malloc0 (sizeof (ilv));
//       a->run = run;
//       a->obid = outbound_id;
//       a->insize = insize;
//       a->nin = ninputs;
//       a->what = what;
//       a->Outbound = Outbound;
//       a->outbuff = (double *) malloc0 (a->nin * a->insize * sizeof(complex));
//       return a;
//   }
//
// Lyra-cpp: `malloc0` is `calloc(1, n)` (zero-init heap alloc);
// `_aligned_free` becomes `free` (no alignment needed for double
// pairs).  Extra leading `xmtr_id` parameter publishes into
// pilv[] when >= 0 (the documented Lyra-native sidestep).

ILV* create_ilv(
    int xmtr_id,
    int run,
    int outbound_id,
    int insize,
    int ninputs,
    long what,
    OutboundFn Outbound)
{
    // Reference: `ILV a = (ILV) malloc0 (sizeof (ilv));` — zero-
    // initialised heap alloc of the plain-POD struct.  The
    // retrofit must `new ILV{}` (NOT calloc) because the
    // `Outbound` field is a std::function (the operator-approved
    // public-API-preserved exception, see ILV.h `OutboundFn`
    // typedef rationale) — its implicit ctor must run before
    // assignment.  `new ILV{}` value-initialises every other
    // field to zero (matching malloc0 semantics for the raw
    // longs/ints/pointer); the explicit field stores below match
    // the reference one-to-one.
    ILV* a = new ILV{};
    a->run = run;
    a->obid = outbound_id;
    a->insize = insize;
    a->nin = ninputs;
    a->what = what;
    a->Outbound = std::move(Outbound);
    a->outbuff = (double*) calloc(1, (size_t) a->nin * (size_t) a->insize * kSizeofComplex);
    if (xmtr_id >= 0 && xmtr_id < MAX_EXT_ILV)
        pilv[xmtr_id] = a;
    return a;
}

// ===== destroy_ilv =====
//
// Reference ilv.c:51-55 (verbatim):
//
//   void destroy_ilv (ILV a)
//   {
//       _aligned_free (a->outbuff);
//       _aligned_free (a);
//   }
//
// Lyra-cpp: `_aligned_free` -> `free`.  Extra `xmtr_id` clears
// the central bank slot iff it still points at this instance.

void destroy_ilv(ILV* a, int xmtr_id)
{
    if (xmtr_id >= 0 && xmtr_id < MAX_EXT_ILV && pilv[xmtr_id] == a)
        pilv[xmtr_id] = nullptr;
    free(a->outbuff);
    // `a` was allocated with `new ILV{}` (the Outbound std::function
    // field needs implicit-ctor invocation that calloc skips), so
    // it must be released with `delete` not `free`.  The reference's
    // `_aligned_free(a)` is byte-equivalent for its plain-POD struct.
    delete a;
}

// ===== xilv =====
//
// Reference ilv.c:57-89 (verbatim):
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

void xilv(ILV* a, double** data)
{
    int i, j, k;
    int what, mask;
    if (_InterlockedAnd(&a->run, 1))
    {
        k = 0;
        for (j = 0; j < a->insize; j++)
        {
            what = _InterlockedAnd(&a->what, 0xffffffff);
            i = 0;
            while (what != 0)
            {
                mask = 1 << i;
                if ((mask & what) != 0)
                {
                    a->outbuff[2 * k + 0] = data[i][2 * j + 0];
                    a->outbuff[2 * k + 1] = data[i][2 * j + 1];
                    what &= ~mask;
                    k++;
                }
                i++;
            }
        }
    }
    else
    {
        k = a->insize;
        if (a->outbuff != data[0])
            memcpy(a->outbuff, data[0], (size_t) a->insize * kSizeofComplex);
    }
    // Reference: `(*a->Outbound)(a->obid, k, a->outbuff);`
    // Lyra-cpp: `Outbound` is std::function (the operator-approved
    // public-API-preserved exception — see ILV.h `OutboundFn`
    // typedef rationale) so the call-syntax is function-call
    // operator instead of pointer-deref-then-call.  Observable
    // behaviour is byte-identical to the reference.
    a->Outbound(a->obid, k, a->outbuff);
}

// ********************************************************************
// *                                                                  *
// *                      INTERLEAVER PROPERTIES                      *
// *                                                                  *
// ********************************************************************

// ===== SetILVOutputPointer =====
//
// Reference ilv.c:97-101 (verbatim):
//
//   void SetILVOutputPointer (int xmtr_id, void(*Outbound)(int id, int nsamples, double* buff))
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       a->Outbound = Outbound;
//   }

void SetILVOutputPointer(int xmtr_id, OutboundFn Outbound)
{
    ILV* a = pilv[xmtr_id];
    a->Outbound = std::move(Outbound);
}

// ===== SetILVRun =====
//
// Reference ilv.c:103-111 (verbatim, sans PORT):
//
//   PORT
//   void SetILVRun (int xmtr_id, int run)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       if (run)
//           InterlockedBitTestAndSet(&a->run, 0);
//       else
//           InterlockedBitTestAndReset(&a->run, 0);
//   }

void SetILVRun(int xmtr_id, int run)
{
    ILV* a = pilv[xmtr_id];
    if (run)
        InterlockedBitTestAndSet(&a->run, 0);
    else
        InterlockedBitTestAndReset(&a->run, 0);
}

// ===== SetILVWhat =====
//
// Reference ilv.c:113-121 (verbatim, sans PORT):
//
//   PORT
//   void SetILVWhat(int xmtr_id, int stream, int state)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       if (state)
//           InterlockedBitTestAndSet(&a->what, stream);    // put stream in output
//       else
//           InterlockedBitTestAndReset(&a->what, stream);  // remove stream from output
//   }

void SetILVWhat(int xmtr_id, int stream, int state)
{
    ILV* a = pilv[xmtr_id];
    if (state)
        InterlockedBitTestAndSet(&a->what, stream);   // put stream in output
    else
        InterlockedBitTestAndReset(&a->what, stream); // remove stream from output
}

// ===== SetILVInsize =====
//
// Reference ilv.c:123-128 (verbatim, sans PORT):
//
//   PORT
//   void SetILVInsize(int xmtr_id, int size)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       a->insize = size;
//   }

void SetILVInsize(int xmtr_id, int size)
{
    ILV* a = pilv[xmtr_id];
    a->insize = size;
}

// ===== SetILVOutboundId =====
//
// Reference ilv.c:130-135 (verbatim, sans PORT):
//
//   PORT
//   void SetILVOutboundId(int xmtr_id, int obid)
//   {
//       ILV a = pcm->xmtr[xmtr_id].pilv;
//       a->obid = obid;
//   }

void SetILVOutboundId(int xmtr_id, int obid)
{
    ILV* a = pilv[xmtr_id];
    a->obid = obid;
}

// ===== pSetILVRun =====
//
// Reference ilv.c:137-143 (verbatim):
//
//   void pSetILVRun(ILV a, int run)
//   {
//       if (run)
//           InterlockedBitTestAndSet(&a->run, 0);
//       else
//           InterlockedBitTestAndReset(&a->run, 0);
//   }

void pSetILVRun(ILV* a, int run)
{
    if (run)
        InterlockedBitTestAndSet(&a->run, 0);
    else
        InterlockedBitTestAndReset(&a->run, 0);
}

// ===== pSetILVInsize =====
//
// Reference ilv.c:145-148 (verbatim):
//
//   void pSetILVInsize(ILV a, int size)
//   {
//       a->insize = size;
//   }

void pSetILVInsize(ILV* a, int size)
{
    a->insize = size;
}

} // namespace lyra::wdsp
