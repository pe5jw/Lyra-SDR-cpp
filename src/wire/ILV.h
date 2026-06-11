// Lyra-cpp — ILV.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/ilv.h (whole file, verbatim)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// DIRECT PORT 2026-06-11 (P0.b — supersedes the 2026-06-09
// retrofit and its operator-rejected deviations)
// ============================================================
//
// This is the reference ilv.h VERBATIM: the `ilv, *ILV` twin
// typedef, the raw `void (*Outbound)(int, int, double*)` field,
// and the exact declaration set the reference header carries
// (create_ilv / destroy_ilv / xilv / SetILVOutputPointer /
// pSetILVRun / pSetILVInsize — the PORT-exported SetILVRun /
// SetILVWhat / SetILVInsize / SetILVOutboundId live in ILV.cpp
// exactly as they live in ilv.c without an ilv.h declaration).
//
// The previously-claimed "C++ collisions" forcing a PascalCase
// tag / std::function / kSizeofComplex were PROVEN FALSE by
// scratch/_typedef_fidelity_test.cpp (compiles /W4-clean):
// `typedef double complex[2]` coexists with std::complex, the
// twin typedef is valid C++, and raw function-pointer fields
// take capture-free lambdas and free functions verbatim.
//
// Only Lyra-cpp packaging differences (NOT code changes):
//   * `lyra::wire` namespace (reference is global C; this is the
//     namespace the ChannelMaster family lives in).
//   * include "wire/cmcomm.h" carries the reference's
//     complex/malloc0/PORT surface (see cmcomm.h preamble).
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.

#pragma once

#include "wire/cmcomm.h"

namespace lyra::wire {

// Reference ilv.h:30-39 (verbatim):

typedef struct _ilv
{
	volatile long run;
	int obid;
	int insize;
	int nin;
	volatile long what;
	double* outbuff;
	void(*Outbound)(int id, int nsamples, double* buff);
} ilv, *ILV;

// Reference ilv.h:41-58 (verbatim):

ILV create_ilv (
	int run,
	int outbound_id,			// id to use in the outbound call
	int insize,					// number of complex samples in EACH INPUT BUFFER
	int ninputs,				// maximum number of inputs
	long what,					// bits specify which inputs are to be interleaved, one bit per input
	void(*Outbound) (int id, int nsamples, double* buff)
	);

void destroy_ilv (ILV a);

void xilv (ILV a, double** data);

void SetILVOutputPointer(int xmtr_id, void(*Outbound)(int id, int nsamples, double* buff));

void pSetILVRun(ILV a, int run);

void pSetILVInsize(ILV a, int size);

}  // namespace lyra::wire
