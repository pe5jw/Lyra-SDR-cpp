// Lyra-cpp — CmBuffs.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/cmbuffs.h (whole file, verbatim)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// DIRECT PORT 2026-06-12 (P0.d — supersedes the 2026-06-09
// retrofit and its operator-rejected deviations)
// ============================================================
//
// This is the reference cmbuffs.h VERBATIM: the `#define CMB_MULT`
// and the `cmb, *CMB` twin typedef with the exact reference field
// set and comments, plus the seven public declarations.  The
// retrofit's deviations are gone: no PascalCase `CmBuffs` struct
// tag, no `inline constexpr` CMB_MULT, no reworded field comments,
// no Lyra-side doc-block rewrites of the declarations.
//
// Only Lyra-cpp packaging differences (NOT code changes):
//   * `lyra::wire` namespace (reference is global C).
//   * `#include "comm.h"` (the wdsp common header) becomes
//     `#include "wire/cmcomm.h"`, which carries the comm.h surface
//     the reference reaches transitively (HANDLE /
//     CRITICAL_SECTION / `complex` / malloc0 / PORT).
//   * The include-guard idiom (`#ifndef _cmbuffs_h`) becomes
//     `#pragma once` (repo convention).
//   * The reference's literal `__declspec (dllexport)` on Inbound
//     is carried as PORT (the documented cmcomm.h mapping).
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.

#pragma once

#include "wire/cmcomm.h"

namespace lyra::wire {

// Reference cmbuffs.h:31-51 (verbatim):

#define CMB_MULT		(3)
typedef struct _cmb
{
	int   id;
	int   max_in_size;							// max input number of complex samples
	int   max_outsize;							// max output number of complex samples
	int   r1_outsize;							// number of complex samples taken out of the ring for processing

	int   r1_size;								// size of a single maximum sized transfer
	int   r1_active_buffsize;					// size of ring (in complex samples)

	double* r1_baseptr;							// pointer to ring
	int   r1_inidx;								// in 'double', actual index into the buffer is 2 times this
	int   r1_outidx;							// in 'double', actual index into the buffer is 2 times this
	int   r1_unqueuedsamps;						// number of input samples not yet queued/released for execution
	volatile long run;							// when 1, thread loops; when 0, thread terminates
	volatile long accept;						// flag indicating whether accepting input data
	HANDLE Sem_BuffReady;						// count = number of output-sized buffers queued for processing
	CRITICAL_SECTION csOUT;						// used to block output while parameters are updated or buffers flushed
	CRITICAL_SECTION csIN;						// used to block input while parameters are updated or buffers flushed
} cmb, *CMB;

// Reference cmbuffs.h:53-66 (verbatim):

extern void create_cmbuffs (int id, int accept, int max_insize, int max_outsize, int outsize);

extern void destroy_cmbuffs (int id);

extern void flush_cmbuffs (int id);

extern PORT void Inbound (int id, int nsamples, double* in);

extern void cmdata (int id, double* out);

extern void cm_main (void *pargs);

extern void SetCMRingOutsize (int id, int size);

}  // namespace lyra::wire
