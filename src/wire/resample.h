// Lyra-cpp — resample.h (WDSP public resampler ABI)
//
// Ported from: openHPSDR WDSP (Warren Pratt NR0V)
// Upstream: https://github.com/TAPR/OpenHPSDR-wdsp
// Source file: wdsp/resample.h (the `struct _resample` typedef —
//   the public, consumer-visible ABI of the WDSP complex-double
//   polyphase resampler, VERSION FOR COMPLEX DOUBLE-PRECISION)
// Source version: bundled wdsp.dll matches Thetis 2.10.3.13 vendor
// Original copyright: (C) 2013 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// WHY THIS STRUCT IS IN LYRA-CPP SOURCE: the ChannelMaster
// reference (aamix.c) accesses the RESAMPLE handle's `run`, `in`,
// `out`, and `size` fields DIRECTLY (aamix.c:247 `a->rsmp[stream]
// ->run`, :249 `->in = data`, :636 `->size = rs_size`, :639/:678
// `->out = ...`).  That is the upstream-faithful pattern — the
// struct is public ABI, not opaque.  The function entry points
// (create_resample / destroy_resample / flush_resample /
// xresample) are dllexports resolved through the wire/wdspcalls.h
// table; this header supplies the TYPE so ported call sites read
// byte-identical to the reference.
//
// VERBATIM replication — NOT modified, NOT extended, NOT
// reorganized.  Any future WDSP upstream change to this layout
// breaks the port.  Only Lyra-cpp packaging difference: the
// lyra::wire namespace (struct layout is unaffected).

#pragma once

namespace lyra::wire {

// Reference wdsp/resample.h (verbatim):

typedef struct _resample
{
	int run;			// run
	int size;			// number of input samples per buffer
	double* in;			// input buffer for resampler
	double* out;		// output buffer for resampler
	int in_rate;
	int out_rate;
	double fcin;
	double fc;
	double fc_low;
	double gain;
	int idx_in;			// index for input into ring
	int ncoefin;
	int ncoef;			// number of coefficients
	int L;				// interpolation factor
	int M;				// decimation factor
	double* h;			// coefficients
	int ringsize;		// number of complex pairs the ring buffer holds
	double* ring;		// ring buffer
	int cpp;			// coefficients of the phase
	int phnum;			// phase number
} resample, *RESAMPLE;

}  // namespace lyra::wire
