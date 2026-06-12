// Lyra-cpp — ObBuffs.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/obbuffs.h (whole file, verbatim)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// DIRECT PORT 2026-06-12 (P1)
// ============================================================
//
// This is the reference obbuffs.h VERBATIM: the `numRings` /
// `obMAXSIZE` / `OBB_MULT` defines, the `obb, *OBB` twin typedef
// with the exact reference field set and comments, and the nine
// public declarations.  obbuffs is the TX-OUT seam of the wire
// layer (WDSP/ChannelMaster output → ring → ob_main pump thread →
// sendOutbound → the protocol packers); it is a SEPARATE TU from
// cmbuffs (which is inbound-only) and deliberately keeps the
// reference's own 2014-era idioms where they differ from cmbuffs
// (calloc/free not malloc0/_aligned_free; obdata without the
// MW0LGE CS wrap; the csOUT enter/leave pair in ob_main; the
// `out` work buffer carried inside the struct).
//
// Only Lyra-cpp packaging differences (NOT code changes):
//   * `lyra::wire` namespace (reference is global C).
//   * The reference preamble (Windows.h / process.h / intrin.h /
//     math.h / time.h / avrt.h + cmUtilities.h + the local
//     `typedef double complex[2]` + PORT define) becomes
//     `#include "wire/cmcomm.h"`, which carries that exact base
//     surface (the documented cmcomm.h mapping; nothing from
//     cmUtilities.h is consumed by this TU).
//   * The include-guard idiom (`#ifndef _obbuffs_h`) becomes
//     `#pragma once` (repo convention).  Note the reference
//     defines numRings/obMAXSIZE ABOVE its guard; with #pragma
//     once the placement distinction is moot — all defines are
//     carried verbatim.
//   * The reference's literal `__declspec (dllexport)` on
//     OutBound is carried as PORT (the documented cmcomm.h
//     mapping).
//
// `sendOutbound` is declared here verbatim (the reference declares
// it in obbuffs.h and defines it in ChannelMaster/network.c:1237).
// Its Lyra-cpp definition is DEFERRED to P2 (the network.c
// sendOutbound / sendProtocol1Samples reconciliation); until then
// the single reference call site in ob_main is carried as
// reference text with a DEFERRED tag (see ObBuffs.cpp).
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.

#pragma once

#include "wire/cmcomm.h"

namespace lyra::wire {

// Reference obbuffs.h:37-38 (verbatim):

#define numRings						(16)
#define obMAXSIZE						(360)

// Reference obbuffs.h:43-63 (verbatim):

#define OBB_MULT						(2)
typedef struct _obb
{
	int   id;
	int   max_in_size;							// max input number of complex samples
	int   r1_outsize;							// number of complex samples taken out of the ring for processing

	int   r1_size;							// size of a single maximum sized transfer
	int   r1_active_buffsize;				// size of ring (in complex samples)

	double* r1_baseptr;							// pointer to ring
	int   r1_inidx;								// in 'double', actual index into the buffer is 2 times this
	int   r1_outidx;							// in 'double', actual index into the buffer is 2 times this
	int   r1_unqueuedsamps;						// number of input samples not yet queued/released for execution
	volatile long run;							// when 1, thread loops; when 0, thread terminates
	volatile long accept;						// flag indicating whether accepting input data
	HANDLE Sem_BuffReady;						// count = number of output-sized buffers queued for processing
	CRITICAL_SECTION csOUT;						// used to block output while parameters are updated or buffers flushed
	CRITICAL_SECTION csIN;						// used to block input while parameters are updated or buffers flushed
	double* out;
} obb, *OBB;

// Reference obbuffs.h:65-79 (verbatim):

extern void create_obbuffs (int id, int accept, int max_insize, int outsize);

extern void destroy_obbuffs (int id);

extern void flush_obbuffs (int id);

extern PORT void OutBound (int id, int nsamples, double* in);

extern void obdata (int id, double* out);

extern void ob_main (void *pargs);

extern void SetOBRingOutsize (int id, int size);

extern void sendOutbound (int id, double* out);

}  // namespace lyra::wire
