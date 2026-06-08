// Lyra-cpp — Resample.h
//
// Ported from: openHPSDR WDSP (Warren Pratt NR0V)
// Upstream: https://github.com/TAPR/OpenHPSDR-wdsp
// Source file: wdsp/resample.h (struct _resample / typedef RESAMPLE
//   at :36-58 — the public, consumer-visible ABI of the WDSP
//   complex-double polyphase resampler)
// Source version: bundled wdsp.dll matches Thetis 2.10.3.13 vendor
// Original copyright: (C) 2013 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// **Why this struct is replicated into Lyra-cpp source**:
//
// The aamix.c reference (which Stage B is porting as AAMix in
// src/wdsp/AAMix.*) accesses the RESAMPLE handle's `in`, `out`,
// and `size` fields DIRECTLY (e.g. aamix.c:249
// `a->rsmp[stream]->in = data;`, line 636
// `a->rsmp[i]->size = rs_size;`, line 678
// `a->rsmp[i]->out = a->resampbuff[i];`).  This is the
// upstream-faithful pattern; no constructor or setter is provided
// for runtime re-pointing of the in/out buffers + size.
//
// Setter accessors DO exist in resample.h:72-78
// (setBuffers_resample / setSize_resample / setInRate_resample /
// setOutRate_resample) but they are declared `extern` ONLY, NOT
// `__declspec(dllexport)`, so they are not in wdsp.dll's export
// table and cannot be resolved by Lyra-cpp's runtime
// GetProcAddress.  Using them would require either rebuilding
// WDSP from source (out of scope — wdsp.dll is consumed as a
// link-time binary per the established Lyra-cpp posture) or
// patching the DLL (rejected — operator preserves the bundled
// upstream binary unchanged).
//
// Therefore, replicating the public struct definition so the
// port can do direct field writes via a `static_cast<resample*>`
// on the opaque handle returned by `create_resample` is the
// correct, upstream-faithful, no-DLL-touch path.  This file is a
// VERBATIM replication of the WDSP public struct definition —
// **NOT modified, NOT extended, NOT reorganized**.  Any future
// WDSP upstream change to this struct layout breaks the Lyra-cpp
// port; the wisdom/upstream-version check at WdspNative::load()
// is the guard.
//
// **C → C++23 idiom translations applied**: NONE.  This file is
// a verbatim C struct in a C-linkage block so the layout is
// byte-identical to wdsp.dll's view.  AAMix.cpp casts the
// opaque void* handle returned by `api().create_resample(...)`
// to `lyra::wdsp::resample*` and writes to the named fields.

#pragma once

namespace lyra::wdsp {

// Verbatim replication of wdsp/resample.h:36-58 (struct _resample).
// extern "C" linkage to ensure C ABI / no name mangling / layout
// matches the wdsp.dll-internal struct exactly.
extern "C" {

typedef struct _resample
{
    int run;            // run
    int size;           // number of input samples per buffer
    double* in;         // input buffer for resampler
    double* out;        // output buffer for resampler
    int in_rate;
    int out_rate;
    double fcin;
    double fc;
    double fc_low;
    double gain;
    int idx_in;         // index for input into ring
    int ncoefin;
    int ncoef;          // number of coefficients
    int L;              // interpolation factor
    int M;              // decimation factor
    double* h;          // coefficients
    int ringsize;       // number of complex pairs the ring buffer holds
    double* ring;       // ring buffer
    int cpp;            // coefficients of the phase
    int phnum;          // phase number
} resample, *RESAMPLE;

} // extern "C"

} // namespace lyra::wdsp
